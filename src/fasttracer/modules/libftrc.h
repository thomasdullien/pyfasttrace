/*
 * libftrc — C API for reading .ftrc binary trace files.
 *
 * Provides a pull-based iterator that reads .ftrc files (format v2),
 * matches entry/exit pairs per thread, and yields completed events.
 *
 * Usage:
 *   ftrc_reader* r = ftrc_open("trace.ftrc");
 *   ftrc_event ev;
 *   while (ftrc_next(r, &ev) == 0) {
 *       // ev.ts_us, ev.dur_us, ev.pid, ev.tid, ev.name, ev.name_len, ...
 *   }
 *   ftrc_close(r);
 */

#ifndef LIBFTRC_H
#define LIBFTRC_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Event types ───────────────────────────────────────────────────── */

#define FTRC_EVENT_COMPLETE  0   /* matched entry+exit → duration event */
#define FTRC_EVENT_METADATA  1   /* thread/process name metadata        */

typedef struct {
    double      ts_us;      /* timestamp in microseconds (CLOCK_MONOTONIC)   */
    double      dur_us;     /* duration in microseconds (0 for metadata)     */
    uint32_t    pid;        /* process ID                                    */
    uint64_t    tid;        /* thread ID (OS tid)                            */
    const char* name;       /* function name (points into mmap'd file)       */
    uint32_t    name_len;   /* length of name (not null-terminated)          */
    const char* cat;        /* category string (static, null-terminated)     */
    int         type;       /* FTRC_EVENT_COMPLETE or FTRC_EVENT_METADATA    */
    int         depth;      /* call stack depth (0 = top-level)              */
    /* For metadata events: the metadata value (e.g. actual thread/process name) */
    const char* meta_value;     /* null-terminated, valid until next ftrc_next() */
    uint32_t    meta_value_len;
} ftrc_event;

/* ── Reader API ────────────────────────────────────────────────────── */

typedef struct ftrc_reader ftrc_reader;

/*
 * Open a .ftrc file for reading. Returns NULL on error.
 * The file is memory-mapped; the reader owns the mapping.
 */
ftrc_reader* ftrc_open(const char* path);

/*
 * Read the next event. Returns 0 on success, -1 when no more events.
 * The event's `name` pointer is valid until ftrc_close().
 */
int ftrc_next(ftrc_reader* r, ftrc_event* out);

/*
 * Close the reader and release all resources.
 */
void ftrc_close(ftrc_reader* r);

/*
 * Total raw events read so far (entry + exit, before pairing).
 */
uint64_t ftrc_raw_event_count(const ftrc_reader* r);

#ifdef __cplusplus
}
#endif

#endif /* LIBFTRC_H */
