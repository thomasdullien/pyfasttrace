# fasttracer

High-performance Python function tracer with 8-byte binary events, double-buffered
mmap'd memory regions, and fork-based async flushing. Designed for tracing
production systems for hours with predictable memory usage and minimal overhead.

Timestamps use `CLOCK_MONOTONIC` for direct correlation with Linux `perf` data
via [perf-viz-merge](https://github.com/thomasdullien/perf-viz-merge).

## Features

- **8 bytes per event** — fixed-size entry/exit records with microsecond timestamps
- **~50ns per event overhead** — `clock_gettime` + atomic write to mmap'd buffer
- **Fork-based flush** — child process writes to disk via COW; parent never blocks
- **Double buffering** — one buffer active, one flushing; predictable memory (2 x buffer_size)
- **File rollover** — automatic rollover at configurable size with synthetic stack events, so each file is self-contained and old files can be deleted
- **CLOCK_MONOTONIC timestamps** — directly correlatable with `perf record -k CLOCK_MONOTONIC`
- **Python 3.10+** — uses `PyEval_SetProfile` on 3.10/3.11, `sys.monitoring` on 3.12+

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

Convert and merge:

```bash
ftrc2json /tmp/traces/*.ftrc -o trace.json
perf-viz-merge --perf perf.data --viz trace.json -o merged.perfetto-trace
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

## Binary format

Each `.ftrc` file contains one or more self-contained chunks:

```
[BufferHeader][StringTable][Event0][Event1]...[EventN]
```

Events are 8 bytes each:

| Field | Size | Description |
|-------|------|-------------|
| `ts_delta_us` | 4 bytes | Microseconds since chunk's base timestamp |
| `func_id` | 2 bytes | Bits 0-14: string table index; bit 15: 0=entry, 1=exit |
| `tid_idx` | 1 byte | Thread index (0-255) |
| `flags` | 1 byte | Bit 0: C function; bit 1: synthetic (rollover) |

The buffer header contains the CLOCK_MONOTONIC base timestamp, PID, thread ID
table, and string table (function names resolved at trace time).

## Running tests

```bash
python -m venv venv
./venv/bin/pip install -e .
./venv/bin/pip install pytest perfetto
./venv/bin/pytest tests/ -v
```

The test suite validates the full pipeline: tracing, binary output, C and Python
converters, Perfetto trace_processor SQL queries, and perf-viz-merge integration.
