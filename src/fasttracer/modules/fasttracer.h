#ifndef FASTTRACER_H
#define FASTTRACER_H

#include <Python.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* ── Binary event format (12 bytes) ────────────────────────────────── */

/* Exit flag lives in the flags byte, not in func_id */
#define FT_FLAG_EXIT        (1 << 7)
#define FT_FLAG_C_FUNCTION  (1 << 0)
#define FT_FLAG_SYNTHETIC   (1 << 1)

/* No practical limit on func_id (full uint32), but cap the intern table */
#define FT_MAX_FUNC_ID     16777215   /* 2^24 - 1, ~16M unique functions */

struct __attribute__((packed)) BinaryEvent {
    uint32_t ts_delta_us;   /* microseconds since buffer base_ts_ns         */
    uint32_t func_id;       /* string table index (1-based, 0 = unknown)    */
    uint8_t  tid_idx;       /* thread index 0-255                           */
    uint8_t  flags;         /* bit 0: C function, bit 1: synthetic, bit 7: EXIT */
    uint16_t _pad;          /* alignment / future use                       */
};

_Static_assert(sizeof(struct BinaryEvent) == 12, "BinaryEvent must be 12 bytes");

/* ── Buffer header ─────────────────────────────────────────────────── */

#define FT_MAGIC       0x43525446   /* "FTRC" in little-endian */
#define FT_VERSION     2            /* v2: 12-byte events, uint32 func_id */
#define FT_MAX_THREADS 256

struct __attribute__((packed)) BufferHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t num_strings;          /* was uint16 + pad in v1              */
    int64_t  base_ts_ns;           /* CLOCK_MONOTONIC nanoseconds         */
    uint32_t num_events;
    uint8_t  num_threads;
    uint8_t  _pad1;
    uint16_t _pad2;
    uint64_t thread_table[FT_MAX_THREADS]; /* tid_idx → OS thread ID     */
    uint32_t string_table_offset;  /* byte offset from buffer start       */
    uint32_t events_offset;        /* byte offset from buffer start       */
};

/* ── String intern table ───────────────────────────────────────────── */

#define FT_INTERN_INITIAL_CAP 256

struct InternEntry {
    void*    key;          /* PyCodeObject* or PyCFunctionObject* pointer */
    uint32_t func_id;      /* sequential ID, 1-based                     */
};

struct InternTable {
    struct InternEntry* entries;
    uint32_t capacity;     /* always power of 2 */
    uint32_t count;
};

/* String table: growable buffer of [uint32_t len][char data[len]] */
struct StringTable {
    char*    data;
    size_t   len;
    size_t   cap;
};

/* ── Thread mapping ────────────────────────────────────────────────── */

struct ThreadStack;  /* forward declaration */

struct ThreadMapEntry {
    uint64_t os_tid;
    uint8_t  tid_idx;
    struct ThreadStack* stack;  /* pointer to TLS stack for rollover iteration */
};

struct ThreadMap {
    struct ThreadMapEntry entries[FT_MAX_THREADS];
    uint16_t count;
};

/* ── Per-thread call stack ─────────────────────────────────────────── */

#define FT_MAX_STACK_DEPTH 256

struct ThreadStack {
    uint32_t func_ids[FT_MAX_STACK_DEPTH];
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

    /* Writer thread (replaces fork-based flush to avoid CUDA conflicts) */
    pthread_t       writer_thread;
    sem_t           flush_sem;          /* posted when a buffer is ready     */
    sem_t           flush_done;         /* posted when the write completes   */
    int             flush_buf;          /* buffer index to write (0 or 1)    */
    size_t          flush_bytes;        /* bytes to write                    */
    char            flush_path[512];    /* output path for pending write     */
    int             writer_stop;        /* 1 = tell writer thread to exit    */
    int             writer_started;     /* 1 = thread was created            */
    char*           output_dir;         /* directory for .ftrc files         */

    /* Rollover */
    size_t          rollover_threshold; /* file size limit in bytes, 0=off   */
    size_t          cumulative_bytes;   /* bytes written to current file     */
    uint32_t        file_seq;           /* sequence number for file naming   */

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
uint32_t intern_lookup(struct InternTable* table, void* key);
/* Insert key→func_id. Returns 0 on success, -1 on failure. */
int  intern_insert(struct InternTable* table, void* key, uint32_t func_id);

int  string_table_init(struct StringTable* st);
void string_table_free(struct StringTable* st);
/* Append a string, return its offset within the table. Returns -1 on error. */
int  string_table_append(struct StringTable* st, const char* str, uint32_t len);

#endif /* FASTTRACER_H */
