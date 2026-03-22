#define PY_SSIZE_T_CLEAN
#include <Python.h>
#include <frameobject.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
/* No sys/wait.h — we use a writer thread instead of fork+waitpid */
#include <sys/syscall.h>
#include <errno.h>

/* gettid() wrapper — returns Linux kernel TID (not pthread_t) */
static inline pid_t ft_gettid(void) {
    return (pid_t)syscall(SYS_gettid);
}

#include "fasttracer.h"

/* ── Forward declarations ──────────────────────────────────────────── */

static int  ft_flush_buffer(FastTracerObject* self, int sync);
static uint32_t ft_intern_function(FastTracerObject* self, PyObject* func_obj, int is_c_func);
static struct ThreadStack* ft_get_thread_stack(FastTracerObject* self);
static uint8_t ft_get_tid_idx(FastTracerObject* self);
static void ft_rollover(FastTracerObject* self);
static PyObject* ft_thread_profile_func(PyObject* obj, PyObject* args);

/* ── Writer thread ─────────────────────────────────────────────────── */

static void*
ft_writer_thread(void* arg)
{
    FastTracerObject* self = (FastTracerObject*)arg;
    while (1) {
        sem_wait(&self->flush_sem);
        if (self->writer_stop) break;

        int fd = open(self->flush_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            size_t written = 0;
            while (written < self->flush_bytes) {
                ssize_t n = write(fd,
                    (char*)self->buffers[self->flush_buf] + written,
                    self->flush_bytes - written);
                if (n <= 0) break;
                written += n;
            }
            close(fd);
        }
        sem_post(&self->flush_done);
    }
    return NULL;
}

/* ── File path helper ──────────────────────────────────────────────── */

static void
ft_make_output_path(FastTracerObject* self, char* buf, size_t buflen)
{
    if (self->rollover_threshold > 0) {
        snprintf(buf, buflen, "%s/%d_%04u.ftrc",
                 self->output_dir, (int)self->pid, self->file_seq);
    } else {
        snprintf(buf, buflen, "%s/%d.ftrc",
                 self->output_dir, (int)self->pid);
    }
}

/* ── Event recording ───────────────────────────────────────────────── */

static inline void
ft_record_event(FastTracerObject* self, uint32_t func_id, uint8_t flags)
{
    /* Fork detection — if the application forks, stop tracing in the child.
     * We no longer fork ourselves, but the app might. */
    if (getpid() != self->pid) {
        self->collecting = 0;
        return;
    }

    int64_t now_ns = ft_get_monotonic_ns();
    uint32_t delta_us = (uint32_t)((now_ns - self->base_ts_ns) / 1000);

    /* Allocate slot atomically */
    size_t offset = atomic_fetch_add_explicit(
        &self->write_offset, sizeof(struct BinaryEvent), memory_order_relaxed);

    /* Check if buffer is full */
    if (offset + sizeof(struct BinaryEvent) > self->buffer_size) {
        /* Need to flush. Only one thread should do this. Try to be the one. */
        /* For simplicity, we allow a small race: multiple threads may see
           the buffer as full simultaneously. We use a simple approach:
           flush and reset, accepting that a few events may be lost at the
           boundary. A production implementation could use a CAS loop. */
        ft_flush_buffer(self, 0);
        /* After flush, write_offset has been reset. Re-allocate. */
        offset = atomic_fetch_add_explicit(
            &self->write_offset, sizeof(struct BinaryEvent), memory_order_relaxed);
        if (offset + sizeof(struct BinaryEvent) > self->buffer_size) {
            /* Both buffers full, back-pressure wait happened, try once more */
            return;  /* drop event rather than block indefinitely */
        }
    }

    uint8_t tid_idx = ft_get_tid_idx(self);

    struct BinaryEvent* event =
        (struct BinaryEvent*)((char*)self->buffers[self->active_buf] + offset);
    event->ts_delta_us = delta_us;
    event->func_id = func_id;
    event->tid_idx = tid_idx;
    event->flags = flags;
}

/* ── Profiler callback (Python 3.10/3.11: PyEval_SetProfile) ───────── */

static int
ft_profile_callback(PyObject* obj, PyFrameObject* frame, int what, PyObject* arg)
{
    FastTracerObject* self = (FastTracerObject*)obj;
    if (!self->collecting) return 0;

    struct ThreadStack* stack = ft_get_thread_stack(self);
    if (!stack) return 0;

    switch (what) {
    case PyTrace_CALL: {
        PyCodeObject* code = PyFrame_GetCode(frame);
        uint32_t fid = ft_intern_function(self, (PyObject*)code, 0);
        Py_DECREF(code);
        if (fid == 0) return 0;  /* intern table full */
        ft_record_event(self, fid, 0);
        if (stack->depth < FT_MAX_STACK_DEPTH) {
            stack->func_ids[stack->depth] = fid;
        }
        stack->depth++;
        break;
    }
    case PyTrace_RETURN: {
        stack->depth--;
        uint32_t fid = 0;
        if (stack->depth >= 0 && stack->depth < FT_MAX_STACK_DEPTH) {
            fid = stack->func_ids[stack->depth];
        }
        if (stack->depth < 0) stack->depth = 0;
        if (fid != 0) {
            ft_record_event(self, fid, FT_FLAG_EXIT);
        }
        break;
    }
    case PyTrace_C_CALL: {
        uint32_t fid = ft_intern_function(self, arg, 1);
        if (fid == 0) return 0;
        ft_record_event(self, fid, FT_FLAG_C_FUNCTION);
        if (stack->depth < FT_MAX_STACK_DEPTH) {
            stack->func_ids[stack->depth] = fid;
        }
        stack->depth++;
        break;
    }
    case PyTrace_C_RETURN:
    case PyTrace_C_EXCEPTION: {
        stack->depth--;
        uint32_t fid = 0;
        if (stack->depth >= 0 && stack->depth < FT_MAX_STACK_DEPTH) {
            fid = stack->func_ids[stack->depth];
        }
        if (stack->depth < 0) stack->depth = 0;
        if (fid != 0) {
            ft_record_event(self, fid, FT_FLAG_EXIT | FT_FLAG_C_FUNCTION);
        }
        break;
    }
    default:
        break;
    }

    return 0;
}

#if PY_VERSION_HEX >= 0x030C0000
/* ── sys.monitoring callbacks (Python 3.12+) ───────────────────────── */

#define FT_TOOL_ID 3

static PyObject*
ft_mon_pystart(PyObject* obj, PyObject* const* args, Py_ssize_t nargs)
{
    FastTracerObject* self = (FastTracerObject*)obj;
    if (!self->collecting) Py_RETURN_NONE;

    /* args[0] = code, args[1] = instruction_offset */
    PyCodeObject* code = (PyCodeObject*)args[0];
    struct ThreadStack* stack = ft_get_thread_stack(self);
    if (!stack) Py_RETURN_NONE;

    uint32_t fid = ft_intern_function(self, (PyObject*)code, 0);
    if (fid == 0) Py_RETURN_NONE;

    ft_record_event(self, fid, 0);
    if (stack->depth < FT_MAX_STACK_DEPTH) {
        stack->func_ids[stack->depth] = fid;
    }
    stack->depth++;

    Py_RETURN_NONE;
}

static PyObject*
ft_mon_pyreturn(PyObject* obj, PyObject* const* args, Py_ssize_t nargs)
{
    FastTracerObject* self = (FastTracerObject*)obj;
    if (!self->collecting) Py_RETURN_NONE;

    struct ThreadStack* stack = ft_get_thread_stack(self);
    if (!stack) Py_RETURN_NONE;

    stack->depth--;
    uint32_t fid = 0;
    if (stack->depth >= 0 && stack->depth < FT_MAX_STACK_DEPTH) {
        fid = stack->func_ids[stack->depth];
    }
    if (stack->depth < 0) stack->depth = 0;
    if (fid != 0) {
        ft_record_event(self, fid, FT_FLAG_EXIT);
    }

    Py_RETURN_NONE;
}

static PyObject*
ft_mon_ccall(PyObject* obj, PyObject* const* args, Py_ssize_t nargs)
{
    FastTracerObject* self = (FastTracerObject*)obj;
    if (!self->collecting) Py_RETURN_NONE;

    /* args[0] = code, args[1] = instruction_offset, args[2] = callable */
    PyObject* callable = args[2];
    struct ThreadStack* stack = ft_get_thread_stack(self);
    if (!stack) Py_RETURN_NONE;

    uint32_t fid = ft_intern_function(self, callable, 1);
    if (fid == 0) Py_RETURN_NONE;

    ft_record_event(self, fid, FT_FLAG_C_FUNCTION);
    if (stack->depth < FT_MAX_STACK_DEPTH) {
        stack->func_ids[stack->depth] = fid;
    }
    stack->depth++;

    Py_RETURN_NONE;
}

static PyObject*
ft_mon_creturn(PyObject* obj, PyObject* const* args, Py_ssize_t nargs)
{
    FastTracerObject* self = (FastTracerObject*)obj;
    if (!self->collecting) Py_RETURN_NONE;

    struct ThreadStack* stack = ft_get_thread_stack(self);
    if (!stack) Py_RETURN_NONE;

    stack->depth--;
    uint32_t fid = 0;
    if (stack->depth >= 0 && stack->depth < FT_MAX_STACK_DEPTH) {
        fid = stack->func_ids[stack->depth];
    }
    if (stack->depth < 0) stack->depth = 0;
    if (fid != 0) {
        ft_record_event(self, fid, FT_FLAG_EXIT | FT_FLAG_C_FUNCTION);
    }

    Py_RETURN_NONE;
}

static PyMethodDef ft_mon_pystart_def = {
    "_ft_pystart", (PyCFunction)ft_mon_pystart, METH_FASTCALL, NULL
};
static PyMethodDef ft_mon_pyreturn_def = {
    "_ft_pyreturn", (PyCFunction)ft_mon_pyreturn, METH_FASTCALL, NULL
};
static PyMethodDef ft_mon_ccall_def = {
    "_ft_ccall", (PyCFunction)ft_mon_ccall, METH_FASTCALL, NULL
};
static PyMethodDef ft_mon_creturn_def = {
    "_ft_creturn", (PyCFunction)ft_mon_creturn, METH_FASTCALL, NULL
};

static int
ft_enable_monitoring(FastTracerObject* self)
{
    PyObject* sys = PyImport_ImportModule("sys");
    if (!sys) return -1;

    PyObject* monitoring = PyObject_GetAttrString(sys, "monitoring");
    Py_DECREF(sys);
    if (!monitoring) return -1;

    /* Register tool */
    PyObject* ret = PyObject_CallMethod(monitoring, "use_tool_id",
                                         "is", FT_TOOL_ID, "fasttracer");
    if (!ret) {
        PyErr_Clear();
        PyObject_CallMethod(monitoring, "free_tool_id", "i", FT_TOOL_ID);
        ret = PyObject_CallMethod(monitoring, "use_tool_id",
                                   "is", FT_TOOL_ID, "fasttracer");
        if (!ret) { Py_DECREF(monitoring); return -1; }
    }
    Py_DECREF(ret);

    /* Register callbacks */
    struct {
        int event;
        PyMethodDef* method;
    } callbacks[] = {
        {PY_MONITORING_EVENT_PY_START,  &ft_mon_pystart_def},
        {PY_MONITORING_EVENT_PY_RESUME, &ft_mon_pystart_def},
        {PY_MONITORING_EVENT_PY_RETURN, &ft_mon_pyreturn_def},
        {PY_MONITORING_EVENT_PY_YIELD,  &ft_mon_pyreturn_def},
        {PY_MONITORING_EVENT_PY_UNWIND, &ft_mon_pyreturn_def},
        {PY_MONITORING_EVENT_CALL,      &ft_mon_ccall_def},
        {PY_MONITORING_EVENT_C_RETURN,  &ft_mon_creturn_def},
        {PY_MONITORING_EVENT_C_RAISE,   &ft_mon_creturn_def},
        {0, NULL}
    };

    unsigned int all_events = 0;
    for (int i = 0; callbacks[i].method != NULL; i++) {
        unsigned int event_bit = 1u << callbacks[i].event;
        PyObject* cb = PyCFunction_New(callbacks[i].method, (PyObject*)self);
        if (!cb) { Py_DECREF(monitoring); return -1; }

        PyObject* r = PyObject_CallMethod(monitoring, "register_callback",
                                           "iiO", FT_TOOL_ID, event_bit, cb);
        Py_DECREF(cb);
        if (!r) { Py_DECREF(monitoring); return -1; }
        Py_DECREF(r);
        all_events |= event_bit;
    }

    PyObject* r = PyObject_CallMethod(monitoring, "set_events",
                                       "ii", FT_TOOL_ID, all_events);
    if (!r) { Py_DECREF(monitoring); return -1; }
    Py_DECREF(r);

    Py_DECREF(monitoring);
    return 0;
}

static int
ft_disable_monitoring(FastTracerObject* self)
{
    PyObject* sys = PyImport_ImportModule("sys");
    if (!sys) return -1;
    PyObject* monitoring = PyObject_GetAttrString(sys, "monitoring");
    Py_DECREF(sys);
    if (!monitoring) return -1;

    PyObject* r = PyObject_CallMethod(monitoring, "set_events",
                                       "ii", FT_TOOL_ID, 0);
    if (r) Py_DECREF(r);
    r = PyObject_CallMethod(monitoring, "free_tool_id", "i", FT_TOOL_ID);
    if (r) Py_DECREF(r);

    Py_DECREF(monitoring);
    PyErr_Clear();  /* ignore errors during cleanup */
    return 0;
}
#endif /* PY_VERSION_HEX >= 0x030C0000 */

/* ── String interning ──────────────────────────────────────────────── */

/* Compute a lightweight identity tag for a Python object.
 * This is used alongside the pointer to detect address reuse:
 *   - PyCodeObject:    co_firstlineno (unique per function definition)
 *   - PyCFunctionObject: hash of ml_name C string pointer (stable for
 *     the lifetime of the extension module)
 *   - Other:           0 (no tag — rare path, accept potential stale hit)
 */
static inline uint32_t
ft_identity_tag(PyObject* func_obj, int is_c_func)
{
    if (!is_c_func && PyCode_Check(func_obj)) {
        return (uint32_t)((PyCodeObject*)func_obj)->co_firstlineno;
    } else if (PyCFunction_Check(func_obj)) {
        uintptr_t p = (uintptr_t)((PyCFunctionObject*)func_obj)->m_ml->ml_name;
        return (uint32_t)(p ^ (p >> 16));
    }
    return 0;
}

static uint32_t
ft_intern_function(FastTracerObject* self, PyObject* func_obj, int is_c_func)
{
    uint32_t tag = ft_identity_tag(func_obj, is_c_func);
    uint32_t fid = intern_lookup(&self->intern, (void*)func_obj, tag);
    if (fid != 0) return fid;

    /* Cold path: first time seeing this function (or pointer was reused) */
    if (self->intern.count >= FT_MAX_FUNC_ID) return 0;

    fid = (uint32_t)(self->intern.count + 1);

    char namebuf[512];
    int namelen = 0;

    if (!is_c_func && PyCode_Check(func_obj)) {
        PyCodeObject* code = (PyCodeObject*)func_obj;
        const char* qualname = NULL;
        const char* filename = NULL;

#if PY_VERSION_HEX >= 0x030B0000
        qualname = PyUnicode_AsUTF8(code->co_qualname);
#else
        qualname = PyUnicode_AsUTF8(code->co_name);
#endif
        filename = PyUnicode_AsUTF8(code->co_filename);

        if (!qualname) qualname = "<unknown>";
        if (!filename) filename = "<unknown>";

        namelen = snprintf(namebuf, sizeof(namebuf), "%s (%s:%d)",
                          qualname, filename, code->co_firstlineno);
    } else if (PyCFunction_Check(func_obj)) {
        PyCFunctionObject* cfunc = (PyCFunctionObject*)func_obj;
        const char* name = cfunc->m_ml->ml_name;
        PyObject* module = cfunc->m_module;

        if (module && PyUnicode_Check(module)) {
            namelen = snprintf(namebuf, sizeof(namebuf), "%s.%s",
                              PyUnicode_AsUTF8(module), name ? name : "?");
        } else {
            namelen = snprintf(namebuf, sizeof(namebuf), "%s",
                              name ? name : "<unknown>");
        }
    } else {
        /* Fallback: use repr */
        PyObject* repr = PyObject_Repr(func_obj);
        if (repr) {
            const char* s = PyUnicode_AsUTF8(repr);
            namelen = snprintf(namebuf, sizeof(namebuf), "%s", s ? s : "?");
            Py_DECREF(repr);
        } else {
            PyErr_Clear();
            namelen = snprintf(namebuf, sizeof(namebuf), "<unknown>");
        }
    }

    if (namelen < 0) namelen = 0;
    if (namelen >= (int)sizeof(namebuf)) namelen = sizeof(namebuf) - 1;

    /* Append to string table */
    if (string_table_append(&self->strings, namebuf, (uint32_t)namelen) < 0) {
        return 0;
    }

    /* Keep the Python object alive so its pointer stays valid as an
     * intern table key.  Without this, the object can be GC'd and its
     * address reused by a different object, causing name corruption.
     * The memory cost is negligible — these objects are already alive
     * in the Python runtime. */
    Py_INCREF(func_obj);
    if (intern_insert(&self->intern, (void*)func_obj, tag, fid) < 0) {
        return 0;
    }

    return fid;
}

/* ── Thread management ─────────────────────────────────────────────── */

static void
ft_thread_stack_destructor(void* ptr)
{
    /* Don't free — the thread_map holds a pointer to this stack for rollover
       iteration. Just zero the depth so rollover won't emit synthetic events
       for dead threads. The memory (512 bytes) is freed in FastTracer_dealloc. */
    struct ThreadStack* stack = (struct ThreadStack*)ptr;
    if (stack) stack->depth = 0;
}

static struct ThreadStack*
ft_get_thread_stack(FastTracerObject* self)
{
    struct ThreadStack* stack = pthread_getspecific(self->tls_key);
    if (stack) return stack;

    stack = calloc(1, sizeof(struct ThreadStack));
    if (!stack) return NULL;

    stack->tid_idx = ft_get_tid_idx(self);
    pthread_setspecific(self->tls_key, stack);

    /* Register in thread map so rollover can iterate all stacks */
    if (stack->tid_idx < FT_MAX_THREADS) {
        self->thread_map.entries[stack->tid_idx].stack = stack;
    }

    return stack;
}

static uint8_t
ft_get_tid_idx(FastTracerObject* self)
{
    uint64_t os_tid = (uint64_t)ft_gettid();  /* Linux kernel TID, not pthread_t */

    /* Linear scan — max 256 entries, typically < 10 */
    for (int i = 0; i < self->thread_map.count; i++) {
        if (self->thread_map.entries[i].os_tid == os_tid) {
            return self->thread_map.entries[i].tid_idx;
        }
    }

    /* New thread */
    if (self->thread_map.count >= FT_MAX_THREADS) {
        return 255;  /* overflow: lump into last slot */
    }

    uint8_t idx = self->thread_map.count;
    self->thread_map.entries[idx].os_tid = os_tid;
    self->thread_map.entries[idx].tid_idx = idx;
    self->thread_map.count++;
    return idx;
}

/* (Fork detection is now handled inline in ft_record_event —
   if the app forks, we simply stop tracing in the child.) */

/* ── Synthetic event recording (for rollover) ──────────────────────── */

static inline void
ft_record_event_for_thread(FastTracerObject* self, uint32_t func_id,
                           uint8_t flags, uint8_t tid_idx)
{
    int64_t now_ns = ft_get_monotonic_ns();
    uint32_t delta_us = (uint32_t)((now_ns - self->base_ts_ns) / 1000);

    size_t offset = atomic_fetch_add_explicit(
        &self->write_offset, sizeof(struct BinaryEvent), memory_order_relaxed);

    if (offset + sizeof(struct BinaryEvent) > self->buffer_size) {
        return;  /* buffer full during synthetic emission — shouldn't happen */
    }

    struct BinaryEvent* event =
        (struct BinaryEvent*)((char*)self->buffers[self->active_buf] + offset);
    event->ts_delta_us = delta_us;
    event->func_id = func_id;
    event->tid_idx = tid_idx;
    event->flags = flags;
}

static void
ft_emit_synthetic_exits(FastTracerObject* self)
{
    for (int t = 0; t < self->thread_map.count; t++) {
        struct ThreadStack* stack = self->thread_map.entries[t].stack;
        if (!stack || stack->depth <= 0) continue;

        uint8_t tid_idx = self->thread_map.entries[t].tid_idx;
        /* Emit exits deepest-first */
        for (int d = stack->depth - 1; d >= 0; d--) {
            if (d < FT_MAX_STACK_DEPTH) {
                ft_record_event_for_thread(self,
                    stack->func_ids[d],
                    FT_FLAG_SYNTHETIC | FT_FLAG_EXIT, tid_idx);
            }
        }
    }
}

static void
ft_emit_synthetic_entries(FastTracerObject* self)
{
    for (int t = 0; t < self->thread_map.count; t++) {
        struct ThreadStack* stack = self->thread_map.entries[t].stack;
        if (!stack || stack->depth <= 0) continue;

        uint8_t tid_idx = self->thread_map.entries[t].tid_idx;
        /* Emit entries shallowest-first */
        for (int d = 0; d < stack->depth && d < FT_MAX_STACK_DEPTH; d++) {
            ft_record_event_for_thread(self,
                stack->func_ids[d],
                FT_FLAG_SYNTHETIC, tid_idx);
        }
    }
}

/* ── Flush mechanism ───────────────────────────────────────────────── */

static int
ft_flush_buffer(FastTracerObject* self, int sync)
{
    int buf_to_flush = self->active_buf;
    size_t bytes_written = atomic_load_explicit(&self->write_offset, memory_order_relaxed);

    /* Calculate number of events */
    uint32_t num_events = 0;
    if (bytes_written > self->events_start) {
        num_events = (uint32_t)((bytes_written - self->events_start) / sizeof(struct BinaryEvent));
    }

    if (num_events == 0) return 0;  /* nothing to flush */

    /* Write header into the buffer */
    struct BufferHeader* hdr = (struct BufferHeader*)self->buffers[buf_to_flush];
    hdr->magic = FT_MAGIC;
    hdr->version = FT_VERSION;
    hdr->pid = (uint32_t)self->pid;
    hdr->base_ts_ns = self->base_ts_ns;
    hdr->num_events = num_events;
    hdr->num_strings = (uint32_t)self->intern.count;
    hdr->num_threads = self->thread_map.count;
    hdr->string_table_offset = (uint32_t)sizeof(struct BufferHeader);
    hdr->events_offset = (uint32_t)self->events_start;

    /* Copy thread table */
    for (int i = 0; i < self->thread_map.count; i++) {
        hdr->thread_table[i] = self->thread_map.entries[i].os_tid;
    }

    /* Copy string table into buffer (between header and events) */
    size_t st_space = self->events_start - sizeof(struct BufferHeader);
    size_t st_len = self->strings.len < st_space ? self->strings.len : st_space;
    memcpy((char*)self->buffers[buf_to_flush] + sizeof(struct BufferHeader),
           self->strings.data, st_len);

    /* Switch to other buffer */
    self->active_buf = 1 - self->active_buf;
    self->base_ts_ns = ft_get_monotonic_ns();
    atomic_store_explicit(&self->write_offset, self->events_start, memory_order_relaxed);

    /* Total bytes to write: up to the end of events */
    size_t total_bytes = bytes_written;

    /* Build output path */
    char path[512];
    ft_make_output_path(self, path, sizeof(path));

    if (sync) {
        /* Synchronous flush (called from stop() or rollover).
         * Wait for any in-flight async write first. */
        sem_wait(&self->flush_done);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            write(fd, self->buffers[buf_to_flush], total_bytes);
            close(fd);
        }
        /* Re-post flush_done so future waits don't block */
        sem_post(&self->flush_done);
        self->cumulative_bytes += total_bytes;
        return 0;
    }

    /* Async flush via writer thread (no fork — safe with CUDA) */
    /* Wait for previous async write to complete (back-pressure) */
    sem_wait(&self->flush_done);

    /* Set up the flush request */
    self->flush_buf = buf_to_flush;
    self->flush_bytes = total_bytes;
    ft_make_output_path(self, self->flush_path, sizeof(self->flush_path));

    /* Signal the writer thread */
    sem_post(&self->flush_sem);

    self->cumulative_bytes += total_bytes;

    /* Check rollover threshold */
    if (self->rollover_threshold > 0 &&
        self->cumulative_bytes >= self->rollover_threshold) {
        ft_rollover(self);
    }

    return 0;
}

/* ── Rollover mechanism ────────────────────────────────────────────── */

static void
ft_rollover(FastTracerObject* self)
{
    int was_collecting = self->collecting;
    self->collecting = 0;

    /* Wait for any in-flight async write to complete */
    sem_wait(&self->flush_done);
    sem_post(&self->flush_done);  /* re-post so sync flush doesn't block */

    /* Emit synthetic exits for all open frames into active buffer */
    ft_emit_synthetic_exits(self);

    /* Synchronous flush — final chunk to current file */
    ft_flush_buffer(self, 1);

    /* Start new file */
    self->file_seq++;
    self->cumulative_bytes = 0;

    /* Reset timing for new file */
    self->base_ts_ns = ft_get_monotonic_ns();
    atomic_store_explicit(&self->write_offset, self->events_start, memory_order_relaxed);

    /* Emit synthetic entries to re-open the stack in the new file */
    ft_emit_synthetic_entries(self);

    /* Resume tracing */
    self->collecting = was_collecting;
}

/* ── Python type methods ───────────────────────────────────────────── */

static PyObject*
FastTracer_new(PyTypeObject* type, PyObject* Py_UNUSED(args), PyObject* Py_UNUSED(kwds))
{
    FastTracerObject* self = (FastTracerObject*)type->tp_alloc(type, 0);
    if (!self) return NULL;

    self->buffers[0] = NULL;
    self->buffers[1] = NULL;
    self->buffer_size = 0;
    self->active_buf = 0;
    self->write_offset = 0;
    self->events_start = 0;
    self->base_ts_ns = 0;
    self->pid = 0;
    self->writer_stop = 0;
    self->writer_started = 0;
    self->output_dir = NULL;
    self->collecting = 0;
    self->max_stack_depth = FT_MAX_STACK_DEPTH;

    memset(&self->intern, 0, sizeof(self->intern));
    memset(&self->strings, 0, sizeof(self->strings));
    memset(&self->thread_map, 0, sizeof(self->thread_map));

    return (PyObject*)self;
}

static int
FastTracer_init(FastTracerObject* self, PyObject* args, PyObject* kwds)
{
    static char* kwlist[] = {"buffer_size", "output_dir", "rollover_size", NULL};
    Py_ssize_t buffer_size = 256 * 1024 * 1024;  /* 256MB default */
    const char* output_dir = "/tmp/fasttracer";
    Py_ssize_t rollover_size = 0;  /* 0 = disabled */

    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|nsn", kwlist,
                                      &buffer_size, &output_dir, &rollover_size)) {
        return -1;
    }

    /* Compute layout: header + string table space + events */
    /* Reserve 16MB for string table (enough for ~160K functions at ~100 chars) */
    size_t string_table_reserve = 16 * 1024 * 1024;
    self->events_start = sizeof(struct BufferHeader) + string_table_reserve;
    /* Align to 8 bytes */
    self->events_start = (self->events_start + 7) & ~(size_t)7;

    if ((size_t)buffer_size < self->events_start + 1024) {
        PyErr_SetString(PyExc_ValueError, "buffer_size too small");
        return -1;
    }

    self->buffer_size = (size_t)buffer_size;

    /* Allocate buffers via mmap */
    for (int i = 0; i < 2; i++) {
        self->buffers[i] = mmap(NULL, self->buffer_size,
                                PROT_READ | PROT_WRITE,
                                MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (self->buffers[i] == MAP_FAILED) {
            PyErr_SetFromErrno(PyExc_OSError);
            return -1;
        }
    }

    /* Initialize state */
    self->active_buf = 0;
    self->write_offset = self->events_start;
    self->base_ts_ns = ft_get_monotonic_ns();
    self->pid = getpid();

    /* Writer thread setup — init flush_done to 1 so the first
     * sem_wait(&flush_done) in ft_flush_buffer returns immediately */
    sem_init(&self->flush_sem, 0, 0);
    sem_init(&self->flush_done, 0, 1);
    self->writer_stop = 0;
    if (pthread_create(&self->writer_thread, NULL, ft_writer_thread, self) != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }
    self->writer_started = 1;

    /* Output directory */
    self->output_dir = strdup(output_dir);
    if (!self->output_dir) {
        PyErr_NoMemory();
        return -1;
    }

    /* Create output directory (best effort) */
    mkdir(self->output_dir, 0755);

    /* Initialize intern table and string table */
    if (intern_init(&self->intern) < 0 || string_table_init(&self->strings) < 0) {
        PyErr_NoMemory();
        return -1;
    }

    /* Rollover */
    self->rollover_threshold = (size_t)rollover_size;
    self->cumulative_bytes = 0;
    self->file_seq = 0;

    /* Thread-local storage */
    if (pthread_key_create(&self->tls_key, ft_thread_stack_destructor) != 0) {
        PyErr_SetFromErrno(PyExc_OSError);
        return -1;
    }

    return 0;
}

static void
FastTracer_dealloc(FastTracerObject* self)
{
    /* Shut down writer thread */
    if (self->writer_started) {
        /* Wait for any in-flight write to finish */
        sem_wait(&self->flush_done);
        /* Tell writer thread to exit */
        self->writer_stop = 1;
        sem_post(&self->flush_sem);
        pthread_join(self->writer_thread, NULL);
        sem_destroy(&self->flush_sem);
        sem_destroy(&self->flush_done);
    }

    for (int i = 0; i < 2; i++) {
        if (self->buffers[i] && self->buffers[i] != MAP_FAILED) {
            munmap(self->buffers[i], self->buffer_size);
        }
    }

    intern_free(&self->intern);
    string_table_free(&self->strings);

    /* Free all thread stacks (destructor only zeroed depth, didn't free) */
    for (int i = 0; i < self->thread_map.count; i++) {
        if (self->thread_map.entries[i].stack) {
            free(self->thread_map.entries[i].stack);
            self->thread_map.entries[i].stack = NULL;
        }
    }

    free(self->output_dir);

    pthread_key_delete(self->tls_key);

    Py_TYPE(self)->tp_free((PyObject*)self);
}

static PyObject*
FastTracer_start(FastTracerObject* self, PyObject* Py_UNUSED(unused))
{
    if (self->collecting) {
        Py_RETURN_NONE;
    }

    self->collecting = 1;
    self->base_ts_ns = ft_get_monotonic_ns();
    self->pid = getpid();
    atomic_store_explicit(&self->write_offset, self->events_start, memory_order_relaxed);

#if PY_VERSION_HEX >= 0x030C0000
    if (ft_enable_monitoring(self) < 0) {
        self->collecting = 0;
        return NULL;
    }
#else
    PyEval_SetProfile(ft_profile_callback, (PyObject*)self);

    /* Also set profile for future threads via threading.setprofile.
     * We create a PyCFunction wrapping ft_thread_profile_func with
     * self as the bound object. This is a proper Python callable that
     * threading.setprofile can invoke as func(frame, event, arg). */
    {
        static PyMethodDef ml = {
            "ft_thread_profile_func",
            (PyCFunction)ft_thread_profile_func,
            METH_VARARGS,
            "FastTracer thread profile hook"
        };
        PyObject* threading = PyImport_ImportModule("threading");
        if (threading) {
            PyObject* handler = PyCFunction_New(&ml, (PyObject*)self);
            PyObject* r = PyObject_CallMethod(threading, "setprofile", "N", handler);
            Py_XDECREF(r);
            Py_DECREF(threading);
        }
        PyErr_Clear();
    }
#endif

    Py_RETURN_NONE;
}

static PyObject*
FastTracer_stop(FastTracerObject* self, PyObject* Py_UNUSED(unused))
{
    if (!self->collecting) {
        Py_RETURN_NONE;
    }

    self->collecting = 0;

#if PY_VERSION_HEX >= 0x030C0000
    ft_disable_monitoring(self);
#else
    PyEval_SetProfile(NULL, NULL);

    PyObject* threading = PyImport_ImportModule("threading");
    if (threading) {
        PyObject* r = PyObject_CallMethod(threading, "setprofile", "O", Py_None);
        Py_XDECREF(r);
        Py_DECREF(threading);
    }
    PyErr_Clear();
#endif

    /* Synchronous final flush */
    ft_flush_buffer(self, 1);

    Py_RETURN_NONE;
}

static PyObject*
FastTracer_get_output_path(FastTracerObject* self, PyObject* Py_UNUSED(unused))
{
    char path[512];
    ft_make_output_path(self, path, sizeof(path));
    return PyUnicode_FromString(path);
}

/* ── Thread profile hook for threading.setprofile ─────────────────── */
/*
 * Python-level profile function passed to threading.setprofile().
 * When a new thread starts, Python calls this as func(frame, event, arg).
 * We install the fast C-level profiler on the thread and also process
 * the first event so it isn't lost.
 *
 * Created via PyCFunction_New with the FastTracer object as 'self',
 * following the same pattern as VizTracer's tracer_threadtracefunc.
 */
static PyObject*
ft_thread_profile_func(PyObject* obj, PyObject* args)
{
    FastTracerObject* self = (FastTracerObject*)obj;
    PyFrameObject* frame = NULL;
    const char* event = NULL;
    PyObject* trace_arg = NULL;

    if (!PyArg_ParseTuple(args, "OsO", &frame, &event, &trace_arg)) {
        Py_RETURN_NONE;
    }

    /* Install the C-level profiler on this thread */
    PyEval_SetProfile(ft_profile_callback, obj);

    /* Process the first event so it isn't lost */
    int what = -1;
    if      (strcmp(event, "call") == 0)        what = PyTrace_CALL;
    else if (strcmp(event, "return") == 0)      what = PyTrace_RETURN;
    else if (strcmp(event, "c_call") == 0)      what = PyTrace_C_CALL;
    else if (strcmp(event, "c_return") == 0)    what = PyTrace_C_RETURN;
    else if (strcmp(event, "c_exception") == 0) what = PyTrace_C_EXCEPTION;

    if (what >= 0 && self->collecting) {
        ft_profile_callback(obj, frame, what, trace_arg);
    }

    Py_RETURN_NONE;
}

static PyMethodDef FastTracer_methods[] = {
    {"start", (PyCFunction)FastTracer_start, METH_NOARGS, "Start tracing"},
    {"stop",  (PyCFunction)FastTracer_stop,  METH_NOARGS, "Stop tracing and flush"},
    {"get_output_path", (PyCFunction)FastTracer_get_output_path, METH_NOARGS,
     "Get path to output .ftrc file"},
    {NULL}
};

static PyTypeObject FastTracerType = {
    PyVarObject_HEAD_INIT(NULL, 0)
    .tp_name = "fasttracer._fasttracer.FastTracer",
    .tp_basicsize = sizeof(FastTracerObject),
    .tp_flags = Py_TPFLAGS_DEFAULT,
    .tp_doc = "High-performance binary trace recorder",
    .tp_new = FastTracer_new,
    .tp_init = (initproc)FastTracer_init,
    .tp_dealloc = (destructor)FastTracer_dealloc,
    .tp_methods = FastTracer_methods,
};

/* ── Module definition ─────────────────────────────────────────────── */

static struct PyModuleDef fasttracer_module = {
    PyModuleDef_HEAD_INIT,
    "_fasttracer",
    "High-performance binary trace recorder C extension",
    -1,
    NULL,  /* m_methods */
    NULL,  /* m_slots */
    NULL,  /* m_traverse */
    NULL,  /* m_clear */
    NULL,  /* m_free */
};

PyMODINIT_FUNC
PyInit__fasttracer(void)
{
    PyObject* m = PyModule_Create(&fasttracer_module);
    if (!m) return NULL;

    if (PyType_Ready(&FastTracerType) < 0) return NULL;

    Py_INCREF(&FastTracerType);
    if (PyModule_AddObject(m, "FastTracer", (PyObject*)&FastTracerType) < 0) {
        Py_DECREF(&FastTracerType);
        Py_DECREF(m);
        return NULL;
    }

    return m;
}
