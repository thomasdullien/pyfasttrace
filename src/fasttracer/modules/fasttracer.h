#ifndef FASTTRACER_H
#define FASTTRACER_H

#include <Python.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Binary event format (8 bytes) ─────────────────────────────────── */

#define FT_EVENT_EXIT_BIT  0x8000
#define FT_FUNC_ID_MASK    0x7FFF
#define FT_MAX_FUNC_ID     32767

#define FT_FLAG_C_FUNCTION  (1 << 0)

struct __attribute__((packed)) BinaryEvent {
    uint32_t ts_delta_us;   /* microseconds since buffer base_ts_ns         */
    uint16_t func_id;       /* bits 0-14: string table index, bit 15: exit  */
    uint8_t  tid_idx;       /* thread index 0-255                           */
    uint8_t  flags;         /* bit 0: C function; rest reserved             */
};

_Static_assert(sizeof(struct BinaryEvent) == 8, "BinaryEvent must be 8 bytes");

/* ── Buffer header ─────────────────────────────────────────────────── */

#define FT_MAGIC       0x43525446   /* "FTRC" in little-endian */
#define FT_VERSION     1
#define FT_MAX_THREADS 256

struct __attribute__((packed)) BufferHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t _pad0;
    int64_t  base_ts_ns;           /* CLOCK_MONOTONIC nanoseconds           */
    uint32_t num_events;
    uint16_t num_strings;
    uint8_t  num_threads;
    uint8_t  _pad1;
    uint64_t thread_table[FT_MAX_THREADS]; /* tid_idx → OS thread ID       */
    uint32_t string_table_offset;  /* byte offset from buffer start         */
    uint32_t events_offset;        /* byte offset from buffer start         */
};

/* ── String intern table ───────────────────────────────────────────── */

#define FT_INTERN_INITIAL_CAP 256

struct InternEntry {
    void*    key;          /* PyCodeObject* or PyCFunctionObject* pointer */
    uint16_t func_id;      /* sequential ID, 1-based                     */
};

struct InternTable {
    struct InternEntry* entries;
    uint32_t capacity;     /* always power of 2 */
    uint32_t count;
};

/* String table: growable buffer of [uint16_t len][char data[len]] */
struct StringTable {
    char*    data;
    size_t   len;
    size_t   cap;
};

/* ── Thread mapping ────────────────────────────────────────────────── */

struct ThreadMapEntry {
    uint64_t os_tid;
    uint8_t  tid_idx;
};

struct ThreadMap {
    struct ThreadMapEntry entries[FT_MAX_THREADS];
    uint16_t count;
};

/* ── Per-thread call stack ─────────────────────────────────────────── */

#define FT_MAX_STACK_DEPTH 256

struct ThreadStack {
    uint16_t func_ids[FT_MAX_STACK_DEPTH];
    int      depth;
    uint8_t  tid_idx;       /* this thread's index */
};

/* ── Main tracer object ────────────────────────────────────────────── */

typedef struct {
    PyObject_HEAD

    /* Double buffer */
    void*           buffers[2];
    size_t          buffer_size;        /* size of each buffer in bytes      */
    int             active_buf;         /* 0 or 1                            */
    _Atomic size_t  write_offset;       /* current write position            */
    size_t          events_start;       /* offset where events begin         */

    /* Timing */
    int64_t         base_ts_ns;         /* CLOCK_MONOTONIC ns at buffer start*/

    /* Identity */
    pid_t           pid;                /* cached getpid() for fork detect   */

    /* String interning */
    struct InternTable intern;
    struct StringTable strings;

    /* Thread mapping */
    struct ThreadMap thread_map;
    pthread_key_t   tls_key;            /* ThreadStack per thread            */

    /* Flush */
    pid_t           flush_child;        /* PID of child doing flush, or 0    */
    char*           output_dir;         /* directory for .ftrc files         */

    /* State */
    int             collecting;         /* nonzero while tracing is active   */
    int             max_stack_depth;
    unsigned int    check_flags;
} FastTracerObject;

/* ── Inline helpers ────────────────────────────────────────────────── */

static inline int64_t
ft_get_monotonic_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + (int64_t)t.tv_nsec;
}

/* ── Intern API (intern.c) ─────────────────────────────────────────── */

int  intern_init(struct InternTable* table);
void intern_free(struct InternTable* table);
/* Returns existing func_id, or 0 if not found */
uint16_t intern_lookup(struct InternTable* table, void* key);
/* Insert key→func_id. Returns 0 on success, -1 on failure. */
int  intern_insert(struct InternTable* table, void* key, uint16_t func_id);

int  string_table_init(struct StringTable* st);
void string_table_free(struct StringTable* st);
/* Append a string, return its offset within the table. Returns -1 on error. */
int  string_table_append(struct StringTable* st, const char* str, uint16_t len);

#endif /* FASTTRACER_H */
