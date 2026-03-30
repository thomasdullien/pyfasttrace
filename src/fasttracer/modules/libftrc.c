/*
 * libftrc — Implementation of the .ftrc reader API.
 *
 * Reads format v2 .ftrc files: 12-byte events, uint32 func_id,
 * exit flag in flags bit 7, uint32 string lengths.
 */

#define _GNU_SOURCE
#include "libftrc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/* ── Binary format (must match fasttracer.h v2) ────────────────────── */

#define FT_MAGIC       0x43525446
#define FT_MAX_THREADS 256
#define FT_THREAD_NAME_LEN 64

#define FT_FLAG_EXIT       (1 << 7)
#define FT_FLAG_C_FUNCTION (1 << 0)
#define FT_FLAG_SYNTHETIC  (1 << 1)

struct __attribute__((packed)) BinaryEvent {
    uint32_t ts_delta_us;
    uint32_t func_id;
    uint8_t  tid_idx;
    uint8_t  flags;
    uint16_t _pad;
};

/* v2 header (legacy) */
struct __attribute__((packed)) BufferHeaderV2 {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t num_strings;
    int64_t  base_ts_ns;
    uint32_t num_events;
    uint8_t  num_threads;
    uint8_t  _pad1;
    uint16_t _pad2;
    uint64_t thread_table[FT_MAX_THREADS];
    uint32_t string_table_offset;
    uint32_t events_offset;
};

/* v3 header (adds thread names) */
struct __attribute__((packed)) BufferHeaderV3 {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t num_strings;
    int64_t  base_ts_ns;
    uint32_t num_events;
    uint8_t  num_threads;
    uint8_t  _pad1;
    uint16_t _pad2;
    uint64_t thread_table[FT_MAX_THREADS];
    char     thread_names[FT_MAX_THREADS][FT_THREAD_NAME_LEN];
    char     process_name[FT_THREAD_NAME_LEN];
    uint32_t string_table_offset;
    uint32_t events_offset;
};

/* ── String table ──────────────────────────────────────────────────── */

struct StringEntry {
    const char* data;
    uint32_t    len;
};

/* ── Per-thread call stack ─────────────────────────────────────────── */

#define MAX_STACK_DEPTH 4096

struct StackEntry {
    double   ts_us;
    uint32_t func_id;
};

struct ThreadStack {
    struct StackEntry entries[MAX_STACK_DEPTH];
    int depth;
};

/* ── Reader state ──────────────────────────────────────────────────── */

/* Unified chunk view — extracted from either v2 or v3 header */
struct ChunkView {
    uint32_t    pid;
    uint32_t    num_strings;
    int64_t     base_ts_ns;
    uint32_t    num_events;
    uint8_t     num_threads;
    const uint64_t* thread_table;       /* points into mmap'd data */
    /* v3 thread names (NULL for v2) */
    const char  (*thread_names)[FT_THREAD_NAME_LEN];
    const char* process_name;           /* NULL for v2 */
    uint32_t    string_table_offset;
    uint32_t    events_offset;
};

struct ftrc_reader {
    /* mmap */
    const char* data;
    size_t      file_size;
    int         fd;

    /* Chunk iteration */
    size_t      chunk_offset;       /* current position in file          */

    /* Current chunk state */
    struct ChunkView cv;
    int         version;            /* 2 or 3                            */
    struct StringEntry* strings;    /* heap-allocated, num_strings+1     */
    uint32_t    num_strings;
    uint32_t    event_idx;          /* next raw event index in chunk     */
    const struct BinaryEvent* events;
    size_t      chunk_end;          /* byte offset of end of chunk       */

    /* Per-thread stacks (persist across events within a chunk) */
    struct ThreadStack stacks[FT_MAX_THREADS];

    /* Deduplication: fasttracer buffer flushes can produce overlapping
     * chunks where the same raw events for a thread appear in both an
     * earlier and a later chunk.  We track the last raw event timestamp
     * per tid_idx and use it as a threshold when the next chunk has the
     * same OS thread — raw events at or before the threshold are skipped. */
    double      last_raw_ts[FT_MAX_THREADS];
    /* Previous chunk's thread table, used to detect tid_idx remapping. */
    uint64_t    prev_thread_table[FT_MAX_THREADS];
    uint8_t     prev_num_threads;
    /* Per-tid_idx: raw-event timestamp threshold; events <= this are skipped. */
    double      dedup_threshold_ts[FT_MAX_THREADS];  /* 0 = no dedup */

    /* Metadata emission state */
    int         meta_phase;         /* 0=process_name, 1..N=thread_name, N+1=done */

    /* Stats */
    uint64_t    raw_events;
    uint64_t    dedup_skipped;

    /* Static strings for metadata */
    char        meta_name_buf[256];
    char        meta_value_buf[256];
};

/* ── Internal: advance to the next chunk ───────────────────────────── */

static int
advance_to_next_chunk(ftrc_reader* r)
{
    /* Free previous string table */
    free(r->strings);
    r->strings = NULL;

    if (r->chunk_offset >= r->file_size) return -1;

    /* Need at least enough bytes to read magic + version */
    while (r->chunk_offset + 8 <= r->file_size) {
        const char* base = r->data + r->chunk_offset;
        uint32_t magic, version;
        memcpy(&magic, base, 4);
        memcpy(&version, base + 4, 4);

        if (magic != FT_MAGIC) {
            fprintf(stderr, "libftrc: bad magic at offset %zu: 0x%08x\n",
                    r->chunk_offset, magic);
            return -1;
        }

        /* Parse header based on version */
        struct ChunkView cv;
        memset(&cv, 0, sizeof(cv));
        size_t header_size;

        if (version == 2) {
            header_size = sizeof(struct BufferHeaderV2);
            if (r->chunk_offset + header_size > r->file_size) return -1;
            const struct BufferHeaderV2* h2 = (const struct BufferHeaderV2*)base;
            cv.pid = h2->pid;
            cv.num_strings = h2->num_strings;
            cv.base_ts_ns = h2->base_ts_ns;
            cv.num_events = h2->num_events;
            cv.num_threads = h2->num_threads;
            cv.thread_table = h2->thread_table;
            cv.thread_names = NULL;   /* v2: no thread names */
            cv.process_name = NULL;
            cv.string_table_offset = h2->string_table_offset;
            cv.events_offset = h2->events_offset;
            r->version = 2;
        } else if (version == 3) {
            header_size = sizeof(struct BufferHeaderV3);
            if (r->chunk_offset + header_size > r->file_size) return -1;
            const struct BufferHeaderV3* h3 = (const struct BufferHeaderV3*)base;
            cv.pid = h3->pid;
            cv.num_strings = h3->num_strings;
            cv.base_ts_ns = h3->base_ts_ns;
            cv.num_events = h3->num_events;
            cv.num_threads = h3->num_threads;
            cv.thread_table = h3->thread_table;
            cv.thread_names = (const char (*)[FT_THREAD_NAME_LEN])h3->thread_names;
            cv.process_name = h3->process_name;
            cv.string_table_offset = h3->string_table_offset;
            cv.events_offset = h3->events_offset;
            r->version = 3;
        } else {
            fprintf(stderr, "libftrc: unsupported version %u at offset %zu\n",
                    version, r->chunk_offset);
            return -1;
        }

        size_t chunk_end = r->chunk_offset + cv.events_offset +
                           (size_t)cv.num_events * sizeof(struct BinaryEvent);
        if (chunk_end > r->file_size) {
            fprintf(stderr, "libftrc: truncated chunk at offset %zu\n",
                    r->chunk_offset);
            return -1;
        }

        /* Parse string table */
        struct StringEntry* strings =
            calloc(cv.num_strings + 1, sizeof(struct StringEntry));
        if (!strings) return -1;

        strings[0].data = "<unknown>";
        strings[0].len = 9;

        const char* p = r->data + r->chunk_offset + cv.string_table_offset;
        const char* end = r->data + chunk_end;

        for (uint32_t i = 0; i < cv.num_strings; i++) {
            if (p + 4 > end) break;
            uint32_t slen;
            memcpy(&slen, p, 4);
            p += 4;
            if (p + slen > end) break;
            strings[i + 1].data = p;
            strings[i + 1].len = slen;
            p += slen;
        }

        /* Set up chunk state */
        r->cv = cv;
        r->strings = strings;
        r->num_strings = cv.num_strings;
        r->event_idx = 0;
        r->events = (const struct BinaryEvent*)
            (r->data + r->chunk_offset + cv.events_offset);
        r->chunk_end = chunk_end;

        /* Do NOT reset stacks between chunks within the same file.
         * Chunks are buffer flushes, not separate traces — open call
         * frames from one chunk continue into the next. Only rollover
         * file boundaries should reset stacks. */

        /* ── Deduplication setup ──────────────────────────────────────
         * fasttracer may flush overlapping buffer contents: a later chunk
         * can contain the same raw events for a thread that were already
         * in an earlier chunk.  Detect this by matching OS TIDs between
         * the previous and current chunk's thread tables. For any thread
         * that appeared in both, set a dedup threshold so we skip
         * completed events whose exit timestamp we've already seen. */
        memset(r->dedup_threshold_ts, 0, sizeof(r->dedup_threshold_ts));
        for (uint8_t new_idx = 0; new_idx < cv.num_threads; new_idx++) {
            uint64_t new_tid = cv.thread_table[new_idx];
            for (uint8_t old_idx = 0; old_idx < r->prev_num_threads; old_idx++) {
                if (r->prev_thread_table[old_idx] == new_tid &&
                    r->last_raw_ts[old_idx] > 0) {
                    r->dedup_threshold_ts[new_idx] =
                        r->last_raw_ts[old_idx];
                    /* Carry forward the stack from the old tid_idx if the
                     * index changed (thread table was re-ordered). */
                    if (new_idx != old_idx) {
                        r->stacks[new_idx] = r->stacks[old_idx];
                    }
                    break;
                }
            }
        }

        /* Save current thread table for next chunk transition */
        memcpy(r->prev_thread_table, cv.thread_table,
               cv.num_threads * sizeof(uint64_t));
        r->prev_num_threads = cv.num_threads;

        /* Start metadata emission: process_name first, then thread names */
        r->meta_phase = 0;

        return 0;
    }

    return -1;
}

/* ── Public API ────────────────────────────────────────────────────── */

ftrc_reader*
ftrc_open(const char* path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "libftrc: cannot open %s: %s\n", path, strerror(errno));
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "libftrc: cannot stat %s: %s\n", path, strerror(errno));
        close(fd);
        return NULL;
    }

    size_t file_size = (size_t)st.st_size;
    if (file_size == 0) {
        close(fd);
        return NULL;
    }

    const char* data = (const char*)mmap(NULL, file_size, PROT_READ,
                                          MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "libftrc: cannot mmap %s: %s\n", path, strerror(errno));
        close(fd);
        return NULL;
    }

    madvise((void*)data, file_size, MADV_SEQUENTIAL);

    ftrc_reader* r = (ftrc_reader*)calloc(1, sizeof(ftrc_reader));
    if (!r) {
        munmap((void*)data, file_size);
        close(fd);
        return NULL;
    }

    r->data = data;
    r->file_size = file_size;
    r->fd = fd;
    r->chunk_offset = 0;

    /* Load first chunk */
    if (advance_to_next_chunk(r) < 0) {
        ftrc_close(r);
        return NULL;
    }

    return r;
}

int
ftrc_next(ftrc_reader* r, ftrc_event* out)
{
    if (!r || !r->cv.num_events) return -1;

    for (;;) {
        /* Phase 1: emit metadata events for current chunk */
        {
            /* Use the chunk's base timestamp for metadata events so they
             * don't skew clock alignment (ts=0 breaks the heuristic). */
            double chunk_ts_us = (double)r->cv.base_ts_ns / 1000.0;

            if (r->meta_phase == 0) {
                out->type = FTRC_EVENT_METADATA;
                out->ts_us = chunk_ts_us;
                out->dur_us = 0;
                out->depth = 0;
                out->pid = r->cv.pid;
                out->tid = (r->cv.num_threads > 0) ? r->cv.thread_table[0] : 0;
                out->name = "process_name";
                out->name_len = 12;
                /* Actual process name in meta_value */
                if (r->cv.process_name && r->cv.process_name[0]) {
                    snprintf(r->meta_value_buf, sizeof(r->meta_value_buf),
                             "%s", r->cv.process_name);
                } else {
                    snprintf(r->meta_value_buf, sizeof(r->meta_value_buf),
                             "python [%u]", r->cv.pid);
                }
                out->meta_value = r->meta_value_buf;
                out->meta_value_len = (uint32_t)strlen(r->meta_value_buf);
                out->cat = "metadata";
                r->meta_phase = 1;
                return 0;
            }
            if (r->meta_phase >= 1 &&
                r->meta_phase <= (int)r->cv.num_threads) {
                int idx = r->meta_phase - 1;
                out->type = FTRC_EVENT_METADATA;
                out->ts_us = chunk_ts_us;
                out->dur_us = 0;
                out->depth = 0;
                out->pid = r->cv.pid;
                out->tid = r->cv.thread_table[idx];
                out->name = "thread_name";
                out->name_len = 11;
                /* Actual thread name in meta_value */
                if (r->cv.thread_names && r->cv.thread_names[idx][0]) {
                    snprintf(r->meta_value_buf, sizeof(r->meta_value_buf),
                             "%s", r->cv.thread_names[idx]);
                } else {
                    snprintf(r->meta_value_buf, sizeof(r->meta_value_buf),
                             "%s", idx == 0 ? "MainThread" : "Thread");
                }
                out->meta_value = r->meta_value_buf;
                out->meta_value_len = (uint32_t)strlen(r->meta_value_buf);
                out->cat = "metadata";
                r->meta_phase++;
                return 0;
            }
        }

        /* Phase 2: process raw events, emit completed X events */
        while (r->event_idx < r->cv.num_events) {
            const struct BinaryEvent* ev = &r->events[r->event_idx];
            r->event_idx++;
            r->raw_events++;

            int is_exit = (ev->flags & FT_FLAG_EXIT) != 0;
            uint32_t fid = ev->func_id;

            uint8_t tidx = ev->tid_idx;
            if (tidx >= FT_MAX_THREADS) tidx = FT_MAX_THREADS - 1;

            double abs_ts_us = (double)r->cv.base_ts_ns / 1000.0 +
                               (double)ev->ts_delta_us;

            /* Track last raw timestamp per tid_idx for dedup threshold */
            r->last_raw_ts[tidx] = abs_ts_us;

            uint64_t os_tid = ev->tid_idx;
            if (ev->tid_idx < r->cv.num_threads) {
                os_tid = r->cv.thread_table[ev->tid_idx];
            }

            /* Deduplication: skip raw events at or before the threshold
             * for this tid_idx (already processed from a previous chunk). */
            if (r->dedup_threshold_ts[tidx] > 0) {
                if (abs_ts_us <= r->dedup_threshold_ts[tidx]) {
                    r->dedup_skipped++;
                    continue;  /* skip this raw event entirely */
                }
                /* Past the overlap region — disable dedup for this thread.
                 * Reset the stack since we skipped the matching entries. */
                r->dedup_threshold_ts[tidx] = 0;
                r->stacks[tidx].depth = 0;
            }

            if (!is_exit) {
                /* Push entry */
                struct ThreadStack* ts = &r->stacks[tidx];
                if (ts->depth < MAX_STACK_DEPTH) {
                    ts->entries[ts->depth].ts_us = abs_ts_us;
                    ts->entries[ts->depth].func_id = fid;
                    ts->depth++;
                }
            } else {
                /* Pop and emit */
                struct ThreadStack* ts = &r->stacks[tidx];
                if (ts->depth <= 0) continue;
                ts->depth--;

                struct StackEntry* entry = &ts->entries[ts->depth];

                const char* name = "<unknown>";
                uint32_t name_len = 9;
                if (entry->func_id > 0 && entry->func_id <= r->num_strings) {
                    name = r->strings[entry->func_id].data;
                    name_len = r->strings[entry->func_id].len;
                }

                out->type = FTRC_EVENT_COMPLETE;
                out->ts_us = entry->ts_us;
                out->dur_us = abs_ts_us - entry->ts_us;
                out->pid = r->cv.pid;
                out->tid = os_tid;
                out->name = name;
                out->name_len = name_len;
                out->cat = "FEE";
                out->depth = ts->depth;  /* depth after pop = this event's depth */
                return 0;
            }
        }

        /* Current chunk exhausted — advance to next */
        r->chunk_offset = r->chunk_end;
        if (advance_to_next_chunk(r) < 0) {
            return -1;  /* no more chunks */
        }
        /* Loop back to emit metadata for the new chunk */
    }
}

void
ftrc_close(ftrc_reader* r)
{
    if (!r) return;
    free(r->strings);
    if (r->data && r->data != MAP_FAILED) {
        munmap((void*)r->data, r->file_size);
    }
    if (r->fd >= 0) {
        close(r->fd);
    }
    free(r);
}

uint64_t
ftrc_raw_event_count(const ftrc_reader* r)
{
    return r ? r->raw_events : 0;
}
