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
#define FT_VERSION     2
#define FT_MAX_THREADS 256

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

struct __attribute__((packed)) BufferHeader {
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

struct ftrc_reader {
    /* mmap */
    const char* data;
    size_t      file_size;
    int         fd;

    /* Chunk iteration */
    size_t      chunk_offset;       /* current position in file          */

    /* Current chunk state */
    const struct BufferHeader* hdr;
    struct StringEntry* strings;    /* heap-allocated, num_strings+1     */
    uint32_t    num_strings;
    uint32_t    event_idx;          /* next raw event index in chunk     */
    const struct BinaryEvent* events;
    size_t      chunk_end;          /* byte offset of end of chunk       */

    /* Per-thread stacks (persist across events within a chunk) */
    struct ThreadStack stacks[FT_MAX_THREADS];

    /* Metadata emission state */
    int         meta_phase;         /* 0=process_name, 1..N=thread_name, N+1=done */

    /* Stats */
    uint64_t    raw_events;

    /* Static strings for metadata */
    char        meta_name_buf[256];
};

/* ── Internal: advance to the next chunk ───────────────────────────── */

static int
advance_to_next_chunk(ftrc_reader* r)
{
    /* Free previous string table */
    free(r->strings);
    r->strings = NULL;

    if (r->chunk_offset >= r->file_size) return -1;

    /* Find next valid chunk */
    while (r->chunk_offset + sizeof(struct BufferHeader) <= r->file_size) {
        const struct BufferHeader* hdr =
            (const struct BufferHeader*)(r->data + r->chunk_offset);

        if (hdr->magic != FT_MAGIC) {
            fprintf(stderr, "libftrc: bad magic at offset %zu: 0x%08x\n",
                    r->chunk_offset, hdr->magic);
            return -1;
        }
        if (hdr->version != FT_VERSION) {
            fprintf(stderr, "libftrc: unsupported version %u at offset %zu\n",
                    hdr->version, r->chunk_offset);
            return -1;
        }

        size_t chunk_end = r->chunk_offset + hdr->events_offset +
                           (size_t)hdr->num_events * sizeof(struct BinaryEvent);
        if (chunk_end > r->file_size) {
            fprintf(stderr, "libftrc: truncated chunk at offset %zu\n",
                    r->chunk_offset);
            return -1;
        }

        /* Parse string table */
        struct StringEntry* strings =
            calloc(hdr->num_strings + 1, sizeof(struct StringEntry));
        if (!strings) return -1;

        strings[0].data = "<unknown>";
        strings[0].len = 9;

        const char* p = r->data + r->chunk_offset + hdr->string_table_offset;
        const char* end = r->data + chunk_end;

        for (uint32_t i = 0; i < hdr->num_strings; i++) {
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
        r->hdr = hdr;
        r->strings = strings;
        r->num_strings = hdr->num_strings;
        r->event_idx = 0;
        r->events = (const struct BinaryEvent*)
            (r->data + r->chunk_offset + hdr->events_offset);
        r->chunk_end = chunk_end;

        /* Reset stacks for this chunk */
        memset(r->stacks, 0, sizeof(r->stacks));

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
    if (!r || !r->hdr) return -1;

    for (;;) {
        /* Phase 1: emit metadata events for current chunk */
        if (r->meta_phase == 0) {
            /* Process name */
            out->type = FTRC_EVENT_METADATA;
            out->ts_us = 0;
            out->dur_us = 0;
            out->pid = r->hdr->pid;
            out->tid = (r->hdr->num_threads > 0) ? r->hdr->thread_table[0] : 0;
            out->name = "process_name";
            out->name_len = 12;
            out->cat = "metadata";
            r->meta_phase = 1;
            return 0;
        }
        if (r->meta_phase >= 1 &&
            r->meta_phase <= (int)r->hdr->num_threads) {
            int idx = r->meta_phase - 1;
            out->type = FTRC_EVENT_METADATA;
            out->ts_us = 0;
            out->dur_us = 0;
            out->pid = r->hdr->pid;
            out->tid = r->hdr->thread_table[idx];
            snprintf(r->meta_name_buf, sizeof(r->meta_name_buf),
                     "%s", idx == 0 ? "MainThread" : "Thread");
            out->name = r->meta_name_buf;
            out->name_len = (uint32_t)strlen(r->meta_name_buf);
            out->cat = "metadata";
            r->meta_phase++;
            return 0;
        }

        /* Phase 2: process raw events, emit completed X events */
        while (r->event_idx < r->hdr->num_events) {
            const struct BinaryEvent* ev = &r->events[r->event_idx];
            r->event_idx++;
            r->raw_events++;

            int is_exit = (ev->flags & FT_FLAG_EXIT) != 0;
            uint32_t fid = ev->func_id;

            double abs_ts_us = (double)r->hdr->base_ts_ns / 1000.0 +
                               (double)ev->ts_delta_us;

            uint64_t os_tid = ev->tid_idx;
            if (ev->tid_idx < r->hdr->num_threads) {
                os_tid = r->hdr->thread_table[ev->tid_idx];
            }

            uint8_t tidx = ev->tid_idx;
            if (tidx >= FT_MAX_THREADS) tidx = FT_MAX_THREADS - 1;

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
                out->pid = r->hdr->pid;
                out->tid = os_tid;
                out->name = name;
                out->name_len = name_len;
                out->cat = "FEE";
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
