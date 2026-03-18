# fasttracer — High-Performance Binary Trace Recorder

## Overview

A Python C extension that records function entry/exit events into a compact 8-byte
binary format, using double-buffered mmap'd memory regions and fork-based async
flushing to disk. A separate offline tool converts the binary output to Chrome Trace
JSON for viewing in Perfetto/chrome://tracing.

Traces use CLOCK_MONOTONIC timestamps so they can be directly correlated with
perf-derived event data (perf record, perf script) without clock skew or calibration.

## Binary Event Format (8 bytes)

```c
struct BinaryEvent {
    uint32_t ts_delta_us;   // microseconds since buffer base time (CLOCK_MONOTONIC)
    uint16_t func_id;       // bits 0-14: function string table index (1-32767)
                            // bit 15: 0=entry, 1=exit
    uint8_t  tid_idx;       // thread index (0-255)
    uint8_t  flags;         // bit 0: 0=Python, 1=C function; other bits reserved
};
```

func_id encodes both identity and direction:
- Entry: func_id = string_table_index (1-32767), bit 15 clear
- Exit:  func_id = string_table_index | 0x8000, bit 15 set

This makes every event self-contained — the converter needs no state machine or
stack reconstruction. The func_id for exit events comes from a lightweight per-thread
stack (see below).

## Per-Thread Call Stack

Each thread maintains a small stack of uint16_t func_ids, used solely to stamp exit
events with the correct function identity.

```c
struct ThreadStack {
    uint16_t func_ids[256];  // 512 bytes, pre-allocated
    int depth;
};
```

On entry: push func_id, then write entry event.
On exit/unwind/exception: pop func_id, write exit event with that func_id | 0x8000.

Python's profiling API guarantees a return/unwind/exception event for every call
event, even when exceptions propagate through multiple frames. viztracer relies on
the same guarantee (PY_UNWIND and C_RAISE are treated as returns). So the stack
stays balanced without special exception handling logic.

Thread-local storage via pthread_key_t. The ThreadStack is heap-allocated on first
use per thread (one malloc per thread lifetime, not per event).

## Timestamp Design

**Clock source: CLOCK_MONOTONIC only.** No rdtsc, no calibration factors.

- `clock_gettime(CLOCK_MONOTONIC)` is the sole time source
- Typical cost: ~20-25ns on modern Linux (vDSO, no syscall)
- Buffer header stores `base_ts_ns`: the CLOCK_MONOTONIC nanosecond value at buffer start
- Each event stores `ts_delta_us = (current_ns - base_ns) / 1000` (integer division)
- uint32 microsecond delta covers ~71 minutes per buffer — sufficient between flushes
- The offline converter reconstructs absolute time: `abs_ns = base_ts_ns + delta_us * 1000`

**Why not rdtsc:** rdtsc requires calibration to convert to wall time, and the
calibration drifts with CPU frequency scaling (even with constant_tsc, the relationship
to CLOCK_MONOTONIC is approximate). Since our primary use case is correlation with
perf data (which uses CLOCK_MONOTONIC), we must use the same clock directly.

**Correlation with perf:** `perf record` and `perf script` report timestamps in
CLOCK_MONOTONIC (seconds.nanoseconds). Our converter outputs the same clock base,
making it possible to merge Python-level call traces with hardware performance
counter data, cache miss events, context switches, etc. in a single timeline.

## Buffer Layout

```
┌──────────────────────────────────────────────────────┐
│ BufferHeader (fixed, page-aligned)                   │
│   magic: "FTRC"                                      │
│   version: uint32 = 1                                │
│   pid: uint32                                        │
│   base_ts_ns: int64 (CLOCK_MONOTONIC nanoseconds)    │
│   num_events: uint32                                 │
│   num_strings: uint16                                │
│   num_threads: uint8                                 │
│   thread_table: [uint8 idx → uint64 os_tid] × 256   │
│   string_table_offset: uint32 (offset from buf start)│
│   events_offset: uint32 (offset from buf start)      │
├──────────────────────────────────────────────────────┤
│ String Table (variable length)                       │
│   For each func_id (1..num_strings):                 │
│     uint16_t len                                     │
│     char name[len]   (UTF-8, no null terminator)     │
├──────────────────────────────────────────────────────┤
│ Event Array (bulk of the buffer)                     │
│   BinaryEvent[0], BinaryEvent[1], ...                │
│   (each 8 bytes, tightly packed)                     │
└──────────────────────────────────────────────────────┘
```

## Architecture

### Double Buffering

Two mmap(MAP_ANONYMOUS | MAP_PRIVATE) regions of configurable size (default 256MB).
One is "active" (being written to by the tracer), the other is either idle or being
flushed by a forked child.

### Recording Hot Path (per event)

1. clock_gettime(CLOCK_MONOTONIC) → ~20-25ns
2. Compare getpid() against self->pid → ~1ns (glibc caches it)
   - If mismatch: we're in a forked child, call reset_child_after_fork()
3. Subtract base_ns, integer divide by 1000 → ts_delta_us
4. Look up func_id from pointer → uint16 hash map (cache-hot)
5. For exit events: pop func_id from per-thread stack, set bit 15
6. __atomic_fetch_add(&write_offset, 8, __ATOMIC_RELAXED) → lock-free slot
7. Write 8 bytes to slot
8. For entry events: push func_id onto per-thread stack

Target: <50ns per event.

### Fork Detection (Application Forks)

When a traced application forks, the child inherits a COW copy of the active buffer
containing the parent's pre-fork events. We must not flush those events from the
child — the parent owns them.

**Mechanism:** Every event recording compares `getpid()` against `self->pid`.
`getpid()` is cached by glibc (~1ns, no syscall). On mismatch, we're in a forked
child and call `reset_child_after_fork()`, which:

1. Resets `write_offset` to start of event area (discards pre-fork events)
2. Takes a new `base_ts_ns` from `clock_gettime(CLOCK_MONOTONIC)`
3. Records the new PID (`self->pid = getpid()`)
4. Resets thread table (only the forking thread survives a fork)
5. Sets `flush_child = 0` (parent's flush child is not ours)
6. **Keeps** the string intern table (same code objects are loaded)

This avoids `pthread_atfork` entirely — no global state, no interaction with other
atfork handlers, no need to distinguish flush forks from application forks, works
correctly with multiple tracer instances.

**Result:** No overlapping events between parent and child output files. The
converter merges files from different PIDs on the common CLOCK_MONOTONIC timeline
without deduplication.

### Flush Fork Guard

Our own flush mechanism also calls fork(). In the flush child, we do:
```c
pid_t pid = fork();
if (pid == 0) {
    // flush child: write buffer, then _exit(0)
    // Never returns to tracing, so the getpid() check never triggers
    write(fd, buffer, size);
    _exit(0);
}
```
The flush child calls `_exit(0)` immediately — it never records another event,
so the `getpid()` check in the hot path never runs in a flush child. No guard
flag needed.

### String Interning

- Hash map keyed by pointer (PyCodeObject* for Python, PyCFunctionObject* for C)
- Value: uint16_t func_id (sequential, starting at 1)
- On first encounter: resolve name string, append to string table, Py_INCREF the key
- The string table lives in a separate growable buffer, copied into the flush buffer's
  header area at flush time
- Max 32767 unique functions per buffer (15 bits, bit 15 reserved for entry/exit)

### Thread Mapping

- Hash map keyed by OS thread ID (pthread_self() or similar)
- Value: uint8_t tid_idx (sequential, starting at 0)
- Max 256 threads
- Written to buffer header at flush time

### Flush Mechanism

Triggered when active buffer is full or tracer.stop() is called:

1. Copy string table + thread table into active buffer's header area
2. Record num_events in header
3. Switch active_buf to the other buffer, reset write_offset
4. If previous flush child still running: waitpid() (back-pressure)
5. fork()
   - Child: open(path, O_WRONLY|O_CREAT|O_APPEND), write() entire buffer, _exit(0)
   - Parent: record child PID, continue tracing

### Fork Safety

The flush child process does ZERO Python API calls. It only:
- Calls write() on the mmap'd region (read-only access, COW-safe)
- Calls _exit(0)

No atexit handlers, no GIL, no Python object access.

### Output Files

Each flush appends to a single file: `<output_dir>/<pid>.ftrc`
The file is a concatenation of self-contained buffer dumps.
The offline converter reads these sequentially.

## File Structure

```
fasttracer/
├── PLAN.md                  (this file)
├── setup.py                 (build configuration)
├── src/
│   └── fasttracer/
│       ├── __init__.py      (Python API: FastTracer class)
│       ├── convert.py       (ftrc → Chrome Trace JSON converter)
│       └── modules/
│           ├── fasttracer.c (C extension: module init, Python type, callbacks)
│           ├── fasttracer.h (structs: BinaryEvent, BufferHeader, FastTracer)
│           ├── intern.c     (pointer → uint16 hash map)
│           └── intern.h
└── tests/
    ├── test_basic.py        (basic tracing tests)
    └── test_convert.py      (binary → JSON conversion tests)
```

## Python API

```python
from fasttracer import FastTracer

# Context manager
with FastTracer(buffer_size=256*1024*1024, output_dir="/tmp/traces"):
    my_code()

# Manual
t = FastTracer()
t.start()
my_code()
t.stop()  # final synchronous flush
```

## Offline Converter

```bash
python -m fasttracer.convert /tmp/traces/12345.ftrc -o trace.json
```

Reads binary chunks, reconstructs string tables, emits Chrome Trace JSON:
```json
{"traceEvents": [
  {"ph":"B","name":"func (file:10)","ts":1234567.890,"pid":1,"tid":1},
  {"ph":"E","name":"func (file:10)","ts":1235890.123,"pid":1,"tid":1},
  ...
]}
```

Timestamps in the JSON are CLOCK_MONOTONIC microseconds, directly comparable to
`perf script` output after trivial unit conversion.

## Merging with perf data

`perf script` outputs timestamps like:
```
python3  1234 [001] 12345.678901: cycles:  ...
```
These are CLOCK_MONOTONIC seconds. Our converter outputs the same clock base in
microseconds. A merge tool can interleave both event streams on a common timeline,
enabling views like "this Python function call coincided with 50K L3 cache misses".

## Implementation Phases

### Phase 1: Core C Extension
- fasttracer.h — all struct definitions
- intern.c/h — pointer→uint16 open-addressing hash map
- fasttracer.c — FastTracer Python type, buffer management, flush, timestamp,
  fork detection

### Phase 2: Tracing Callbacks
- PyEval_SetProfile callback for Python 3.10/3.11
- sys.monitoring callbacks for Python 3.12+
- Per-thread stack management (push func_id on call, pop on return/unwind)
- String interning in cold path

### Phase 3: Python Wrapper + Build
- setup.py with Extension configuration
- __init__.py with FastTracer class
- Context manager, start/stop API

### Phase 4: Converter
- convert.py — read .ftrc files, emit Chrome Trace JSON
- Handle multiple buffer chunks in one file
- Timestamp reconstruction: base_ts_ns + delta_us * 1000

### Phase 5: Tests
- Basic tracing: start/stop/verify output exists
- Event correctness: known call sequence → expected events
- Multi-thread tracing
- Buffer flush (fill buffer, verify fork + write)
- Converter round-trip
- Timestamp monotonicity and CLOCK_MONOTONIC correlation

## Design Decisions

| Decision | Rationale |
|----------|-----------|
| CLOCK_MONOTONIC only | Must correlate with perf; rdtsc calibration drifts |
| 8-byte fixed events | Cache-line friendly, predictable capacity, trivial indexing |
| Separate entry/exit events | Simpler than tracking duration in the tracer |
| func_id in both entry+exit | Self-contained events; no converter state machine needed |
| bit 15 of func_id = direction | Entry/exit encoded without extra field |
| Per-thread uint16 stack | Pops func_id for exit events; 512 bytes per thread |
| Sequential uint16 func_id | Collision-free, smaller than any hash |
| uint32 microsecond delta | 71min range, same resolution as Chrome Trace |
| Convert to µs at record time | One subtract + one divide, no post-hoc calibration |
| getpid() check for fork detect | ~1ns, no global state, no pthread_atfork issues |
| fork() for flush | Zero-copy (COW), no GIL, no Python in child |
| MAP_ANONYMOUS not MAP_FILE | Simpler; child does explicit write() |
| atomic_fetch_add for offset | Lock-free multi-thread slot allocation |
| _exit(0) in child | Never run Python cleanup in forked child |
| PyEval_SetProfile for 3.10/3.11 | Only available API; sys.monitoring for 3.12+ |
