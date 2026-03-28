# pyfasttrace (fasttracer)

Low-overhead Python function tracer using a C extension with binary
.ftrc output format. Double-buffered with async writer thread.

## Coding style

- C11 for the C extension (`src/fasttracer/modules/`)
- **Never allow data races in concurrent code.** The tracer runs in a
  multi-threaded Python process. All shared mutable state (buffers,
  write offsets, thread maps) must use proper synchronization — atomics
  with correct memory ordering, CAS for coordination, no "allowing
  small races." If you believe a race is acceptable, flag it explicitly
  for review and do not proceed without confirmation.

## Build

```bash
python3 -m venv .venv
.venv/bin/pip install -e .
.venv/bin/python -m pytest tests/ -v
```

## Key files

- `src/fasttracer/modules/fasttracer.c` — tracer core (profiler callback, double buffer, writer thread)
- `src/fasttracer/modules/fasttracer.h` — binary format (BufferHeader, BinaryEvent)
- `src/fasttracer/modules/libftrc.c` — reader library (used by perf-viz-merge)
- `src/fasttracer/modules/libftrc.h` — reader API
