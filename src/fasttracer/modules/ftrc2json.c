/*
 * ftrc2json — Convert .ftrc binary trace files to Chrome Trace JSON.
 *
 * Usage: ftrc2json [-o output.json] input1.ftrc [input2.ftrc ...]
 *
 * Reads binary chunks, reconstructs string tables, emits Chrome Trace JSON.
 * Designed for multi-gigabyte inputs: mmap's the input, streams the output.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <errno.h>

/* ── Must match fasttracer.h ───────────────────────────────────────── */

#define FT_MAGIC       0x43525446
#define FT_VERSION     1
#define FT_MAX_THREADS 256
#define FT_EVENT_EXIT_BIT  0x8000
#define FT_FUNC_ID_MASK    0x7FFF
#define FT_FLAG_C_FUNCTION (1 << 0)

struct __attribute__((packed)) BinaryEvent {
    uint32_t ts_delta_us;
    uint16_t func_id;
    uint8_t  tid_idx;
    uint8_t  flags;
};

struct __attribute__((packed)) BufferHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t pid;
    uint32_t _pad0;
    int64_t  base_ts_ns;
    uint32_t num_events;
    uint16_t num_strings;
    uint8_t  num_threads;
    uint8_t  _pad1;
    uint64_t thread_table[FT_MAX_THREADS];
    uint32_t string_table_offset;
    uint32_t events_offset;
};

/* ── String table: array of pointers + lengths ─────────────────────── */

struct StringEntry {
    const char* data;
    uint16_t    len;
};

/*
 * Parse the string table from a chunk.
 * Returns heap-allocated array of (num_strings+1) entries (index 0 = "<unknown>").
 * Caller must free the returned array.
 */
static struct StringEntry*
parse_string_table(const char* base, uint32_t st_offset, uint16_t num_strings,
                   size_t chunk_size)
{
    struct StringEntry* strings = calloc(num_strings + 1, sizeof(struct StringEntry));
    if (!strings) return NULL;

    strings[0].data = "<unknown>";
    strings[0].len = 9;

    const char* p = base + st_offset;
    const char* end = base + chunk_size;

    for (uint16_t i = 0; i < num_strings; i++) {
        if (p + 2 > end) break;
        uint16_t slen;
        memcpy(&slen, p, 2);
        p += 2;
        if (p + slen > end) break;
        strings[i + 1].data = p;
        strings[i + 1].len = slen;
        p += slen;
    }

    return strings;
}

/* ── Escape a string for JSON output ───────────────────────────────── */

static void
fwrite_json_string(FILE* out, const char* s, uint16_t len)
{
    fputc('"', out);
    for (uint16_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        switch (c) {
        case '"':  fputs("\\\"", out); break;
        case '\\': fputs("\\\\", out); break;
        case '\n': fputs("\\n", out);  break;
        case '\r': fputs("\\r", out);  break;
        case '\t': fputs("\\t", out);  break;
        default:
            if (c < 0x20) {
                fprintf(out, "\\u%04x", c);
            } else {
                fputc(c, out);
            }
        }
    }
    fputc('"', out);
}

/* ── Process one .ftrc file ────────────────────────────────────────── */

static int
process_file(const char* path, FILE* out, int* first_event)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Cannot open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        fprintf(stderr, "Cannot stat %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    size_t file_size = (size_t)st.st_size;
    if (file_size == 0) {
        close(fd);
        return 0;
    }

    const char* data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (data == MAP_FAILED) {
        fprintf(stderr, "Cannot mmap %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    /* Advise sequential access for readahead */
    madvise((void*)data, file_size, MADV_SEQUENTIAL);

    size_t offset = 0;
    size_t total_events = 0;

    while (offset + sizeof(struct BufferHeader) <= file_size) {
        const struct BufferHeader* hdr = (const struct BufferHeader*)(data + offset);

        if (hdr->magic != FT_MAGIC) {
            fprintf(stderr, "Bad magic at offset %zu in %s: 0x%08x\n",
                    offset, path, hdr->magic);
            break;
        }
        if (hdr->version != FT_VERSION) {
            fprintf(stderr, "Unknown version %u at offset %zu in %s\n",
                    hdr->version, offset, path);
            break;
        }

        /* Compute chunk size: events_offset + num_events * 8 */
        size_t chunk_end = offset + hdr->events_offset +
                           (size_t)hdr->num_events * sizeof(struct BinaryEvent);
        if (chunk_end > file_size) {
            fprintf(stderr, "Truncated chunk at offset %zu in %s\n", offset, path);
            break;
        }

        /* Parse string table */
        struct StringEntry* strings = parse_string_table(
            data + offset, hdr->string_table_offset, hdr->num_strings,
            chunk_end - offset);
        if (!strings) {
            fprintf(stderr, "Out of memory parsing string table\n");
            break;
        }

        uint16_t num_strings = hdr->num_strings;

        /* Emit thread name metadata events */
        for (uint8_t i = 0; i < hdr->num_threads; i++) {
            if (!*first_event) {
                fputc(',', out);
            }
            *first_event = 0;
            fprintf(out,
                "{\"ph\":\"M\",\"pid\":%u,\"tid\":%lu,"
                "\"name\":\"thread_name\","
                "\"args\":{\"name\":\"Thread %lu\"}}",
                hdr->pid,
                (unsigned long)hdr->thread_table[i],
                (unsigned long)hdr->thread_table[i]);
        }

        /* Walk events */
        const struct BinaryEvent* events =
            (const struct BinaryEvent*)(data + offset + hdr->events_offset);

        for (uint32_t i = 0; i < hdr->num_events; i++) {
            const struct BinaryEvent* ev = &events[i];

            int is_exit = (ev->func_id & FT_EVENT_EXIT_BIT) != 0;
            uint16_t fid = ev->func_id & FT_FUNC_ID_MASK;

            /* Absolute timestamp in microseconds (CLOCK_MONOTONIC) */
            double abs_ts_us = (double)hdr->base_ts_ns / 1000.0 +
                               (double)ev->ts_delta_us;

            /* Resolve thread ID */
            uint64_t os_tid = ev->tid_idx;
            if (ev->tid_idx < hdr->num_threads) {
                os_tid = hdr->thread_table[ev->tid_idx];
            }

            /* Resolve function name */
            const char* name;
            uint16_t name_len;
            if (fid > 0 && fid <= num_strings) {
                name = strings[fid].data;
                name_len = strings[fid].len;
            } else {
                name = "<unknown>";
                name_len = 9;
            }

            if (!*first_event) {
                fputc(',', out);
            }
            *first_event = 0;

            /* Emit JSON event */
            fprintf(out, "{\"ph\":\"%c\",\"pid\":%u,\"tid\":%lu,\"ts\":%.3f,\"name\":",
                    is_exit ? 'E' : 'B',
                    hdr->pid,
                    (unsigned long)os_tid,
                    abs_ts_us);
            fwrite_json_string(out, name, name_len);

            if (ev->flags & FT_FLAG_C_FUNCTION) {
                fputs(",\"cat\":\"c_function\"", out);
            }

            fputc('}', out);
        }

        total_events += hdr->num_events;
        free(strings);
        offset = chunk_end;
    }

    fprintf(stderr, "Read %zu events from %s\n", total_events, path);

    munmap((void*)data, file_size);
    close(fd);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int
main(int argc, char** argv)
{
    const char* output_path = NULL;

    /* Simple arg parsing */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [-o output.json] input1.ftrc [input2.ftrc ...]\n",
                    argv[0]);
            return 0;
        }
    }

    /* Collect input files (everything that's not -o or its argument) */
    const char** input_files = malloc(argc * sizeof(const char*));
    int num_inputs = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            i++;  /* skip -o and its argument */
            continue;
        }
        input_files[num_inputs++] = argv[i];
    }

    if (num_inputs == 0) {
        fprintf(stderr, "Usage: %s [-o output.json] input1.ftrc [input2.ftrc ...]\n",
                argv[0]);
        free(input_files);
        return 1;
    }

    FILE* out;
    if (output_path) {
        out = fopen(output_path, "w");
        if (!out) {
            fprintf(stderr, "Cannot open %s for writing: %s\n",
                    output_path, strerror(errno));
            free(input_files);
            return 1;
        }
    } else {
        out = stdout;
    }

    /* Use a large output buffer for fewer write syscalls */
    char* outbuf = malloc(4 * 1024 * 1024);
    if (outbuf) {
        setvbuf(out, outbuf, _IOFBF, 4 * 1024 * 1024);
    }

    fputs("{\"traceEvents\":[", out);

    int first_event = 1;
    for (int i = 0; i < num_inputs; i++) {
        if (process_file(input_files[i], out, &first_event) < 0) {
            fprintf(stderr, "Error processing %s\n", input_files[i]);
        }
    }

    fputs("]}\n", out);

    if (output_path) {
        fclose(out);
        fprintf(stderr, "Wrote %s\n", output_path);
    }

    free(outbuf);
    free(input_files);
    return 0;
}
