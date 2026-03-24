/*
 * ftrc2json — Convert .ftrc binary trace files to Chrome Trace JSON.
 *
 * Usage: ftrc2json [-o output.json] input1.ftrc [input2.ftrc ...]
 *
 * Uses libftrc for parsing. Streams JSON output for multi-gigabyte inputs.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "libftrc.h"

/* ── Escape a string for JSON output ───────────────────────────────── */

static void
fwrite_json_string(FILE* out, const char* s, uint32_t len)
{
    fputc('"', out);
    for (uint32_t i = 0; i < len; i++) {
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
    ftrc_reader* r = ftrc_open(path);
    if (!r) {
        fprintf(stderr, "Cannot open %s\n", path);
        return -1;
    }

    ftrc_event ev;
    size_t count = 0;

    while (ftrc_next(r, &ev) == 0) {
        if (!*first_event) {
            fputc(',', out);
        }
        *first_event = 0;

        if (ev.type == FTRC_EVENT_METADATA) {
            fprintf(out,
                "{\"ph\":\"M\",\"pid\":%u,\"tid\":%lu,"
                "\"name\":",
                ev.pid, (unsigned long)ev.tid);
            fwrite_json_string(out, ev.name, ev.name_len);
            if (ev.cat && strcmp(ev.cat, "thread_name") == 0) {
                fputs(",\"args\":{\"name\":", out);
                fwrite_json_string(out, ev.name, ev.name_len);
                fputs("}}", out);
            } else {
                fputc('}', out);
            }
        } else {
            fprintf(out,
                "{\"pid\":%u,\"tid\":%lu,\"ts\":%.3f,\"ph\":\"X\","
                "\"dur\":%.3f,\"name\":",
                ev.pid, (unsigned long)ev.tid, ev.ts_us, ev.dur_us);
            fwrite_json_string(out, ev.name, ev.name_len);
            fprintf(out, ",\"cat\":\"%s\"}", ev.cat ? ev.cat : "FEE");
            count++;
        }
    }

    fprintf(stderr, "Read %zu events from %s\n", count, path);
    ftrc_close(r);
    return 0;
}

/* ── Main ──────────────────────────────────────────────────────────── */

int
main(int argc, char** argv)
{
    const char* output_path = NULL;

    /* Simple arg parsing */
    const char** input_files = malloc(argc * sizeof(const char*));
    int num_inputs = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [-o output.json] input1.ftrc [input2.ftrc ...]\n",
                    argv[0]);
            free(input_files);
            return 0;
        } else {
            input_files[num_inputs++] = argv[i];
        }
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

    /* Large output buffer for fewer write syscalls */
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

    fputs("],\"viztracer_metadata\":{\"version\":\"0.0.1\",\"overflow\":false},"
          "\"file_info\":{\"files\":{},\"functions\":{}}}\n", out);

    if (output_path) {
        fclose(out);
        fprintf(stderr, "Wrote %s\n", output_path);
    }

    free(outbuf);
    free(input_files);
    return 0;
}
