# fasttracer

High-performance Python function tracer with 12-byte binary events, double-buffered
mmap'd memory regions, and thread-based async flushing. Designed for tracing
GPU-accelerated production systems for hours with predictable memory usage and
minimal overhead.

Timestamps use `CLOCK_MONOTONIC` for direct correlation with Linux `perf` data
via [perf-viz-merge](https://github.com/thomasdullien/perf-viz-merge).

## Features

- **12 bytes per event** — fixed-size entry/exit records with microsecond timestamps and up to 16M unique function names
- **~50ns per event overhead** — `clock_gettime` + atomic write to mmap'd buffer
- **Thread-based flush** — dedicated writer thread handles disk I/O; no fork (safe with CUDA)
- **Double buffering** — one buffer active, one flushing; predictable memory (2 × buffer_size)
- **File rollover** — automatic rollover at configurable size with synthetic stack events, so each file is self-contained and old files can be deleted
- **CLOCK_MONOTONIC timestamps** — directly correlatable with `perf record -k CLOCK_MONOTONIC`
- **Linux kernel TIDs** — uses `gettid()` for thread IDs that match perf data
- **Python 3.10+** — uses `PyEval_SetProfile` on 3.10/3.11, `sys.monitoring` on 3.12+
- **libftrc C API** — read `.ftrc` files from C/C++ without JSON conversion

## Installation

```bash
pip install -e .
```

Requires a C compiler. Builds two artifacts:
- `fasttracer._fasttracer` — Python C extension (the tracer)
- `fasttracer/ftrc2json` — standalone C binary (the converter)

## Usage

### Basic tracing

```python
from fasttracer import FastTracer

with FastTracer(output_dir="/tmp/traces") as t:
    my_code()

print(t.output_path)  # /tmp/traces/12345.ftrc
```

Or manually:

```python
t = FastTracer()
t.start()
my_code()
t.stop()
```

### Parameters

```python
FastTracer(
    buffer_size=256 * 1024 * 1024,  # 256MB per buffer (2 buffers total)
    output_dir="/tmp/fasttracer",    # directory for .ftrc files
    rollover_size=0,                 # 0 = no rollover; e.g. 4*1024*1024*1024 for 4GB
)
```

### Converting to Chrome Trace JSON

Using the C converter (fast, handles multi-GB files):

```python
from fasttracer import ftrc2json

ftrc2json("/tmp/traces/12345.ftrc", "trace.json")

# Multiple files (e.g. from rollover):
ftrc2json(["/tmp/traces/12345_0000.ftrc", "/tmp/traces/12345_0001.ftrc"], "trace.json")
```

Or from the command line:

```bash
# C converter (installed with the package)
ftrc2json /tmp/traces/12345.ftrc -o trace.json

# Python fallback
ftrc2json-py /tmp/traces/12345.ftrc -o trace.json
```

### Reading .ftrc files from C/C++ (libftrc)

For tools that need to read `.ftrc` files directly (e.g., perf-viz-merge):

```c
#include "libftrc.h"

ftrc_reader* r = ftrc_open("trace.ftrc");
ftrc_event ev;
while (ftrc_next(r, &ev) == 0) {
    printf("%.*s: %.3f us\n", ev.name_len, ev.name, ev.dur_us);
}
ftrc_close(r);
```

### Viewing in Perfetto

Open https://ui.perfetto.dev and load the JSON file, or convert to native Perfetto
format with perf-viz-merge.

### Merging with perf data

Record perf data with `CLOCK_MONOTONIC`:

```bash
sudo perf record -e sched:sched_switch -e sched:sched_wakeup \
    -k CLOCK_MONOTONIC -a -- python my_script.py
```

Trace with fasttracer in the script:

```python
from fasttracer import FastTracer

with FastTracer(output_dir="/tmp/traces"):
    main()
```

Merge directly (no JSON conversion needed):

```bash
perf-viz-merge --perf perf.data --viz /tmp/traces/*.ftrc -o merged.perfetto-trace
```

### File rollover for long-running traces

For production systems running hours/days, enable rollover to keep file sizes
manageable and allow deletion of old data:

```python
with FastTracer(output_dir="/tmp/traces", rollover_size=4 * 1024 * 1024 * 1024):
    long_running_server()
```

This produces sequentially numbered files (`12345_0000.ftrc`, `12345_0001.ftrc`, ...).
Each file is self-contained — synthetic entry/exit events at file boundaries ensure
stack traces are valid even if earlier files are deleted.

## Binary format (v2)

Each `.ftrc` file contains one or more chunks (from buffer flushes):

```
[BufferHeader][StringTable][Event0][Event1]...[EventN]
```

Events are 12 bytes each:

| Field | Size | Description |
|-------|------|-------------|
| `ts_delta_us` | 4 bytes | Microseconds since chunk's base timestamp |
| `func_id` | 4 bytes | String table index (1-based, 0 = unknown) |
| `tid_idx` | 1 byte | Thread index (0-255) |
| `flags` | 1 byte | Bit 0: C function; bit 1: synthetic; bit 7: exit |
| `_pad` | 2 bytes | Reserved |

The buffer header contains the CLOCK_MONOTONIC base timestamp, PID, thread ID
table (using Linux kernel TIDs from `gettid()`), and string table with uint32
length-prefixed entries. String tables grow monotonically across chunks — func_ids
from earlier chunks remain valid in later chunks.

## Running tests

```bash
python -m venv venv
./venv/bin/pip install -e .
./venv/bin/pip install pytest perfetto
./venv/bin/pytest tests/ -v
```

The test suite validates the full pipeline: tracing, binary output, C and Python
converters, Perfetto trace_processor SQL queries, and perf-viz-merge integration.
