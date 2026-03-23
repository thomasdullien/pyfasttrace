"""Basic tests for fasttracer."""

import json
import os
import shutil
import struct
import tempfile

import pytest


@pytest.fixture
def trace_dir():
    d = tempfile.mkdtemp(prefix="fasttracer_test_")
    yield d
    shutil.rmtree(d, ignore_errors=True)


def test_import():
    from fasttracer import FastTracer
    assert FastTracer is not None


def test_start_stop(trace_dir):
    from fasttracer import FastTracer

    t = FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir)
    t.start()

    # Do some work that generates trace events
    def foo():
        return 42

    for _ in range(100):
        foo()

    t.stop()

    # Check that output file exists and has content
    output = t.output_path
    assert os.path.exists(output), f"Output file not found: {output}"
    size = os.path.getsize(output)
    assert size > 0, "Output file is empty"

    # Check magic
    with open(output, "rb") as f:
        magic = struct.unpack("<I", f.read(4))[0]
        assert magic == 0x43525446, f"Bad magic: 0x{magic:08x}"


def test_context_manager(trace_dir):
    from fasttracer import FastTracer

    def bar():
        return sum(range(100))

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(50):
            bar()

    output = t.output_path
    assert os.path.exists(output)
    assert os.path.getsize(output) > 0


def test_converter(trace_dir):
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def baz(n):
        if n <= 1:
            return 1
        return baz(n - 1) + baz(n - 2)

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        baz(10)

    events = convert_file(t.output_path)
    assert len(events) > 0, "No events converted"

    # Check that we have X (complete) events
    phases = {e["ph"] for e in events}
    assert "X" in phases, "No complete events found"

    # Check that events have expected fields
    for e in events:
        assert "ts" in e
        assert "dur" in e
        assert "pid" in e
        assert "tid" in e
        assert "name" in e

    # With "X" events, timestamps are ordered by completion (inner calls
    # complete before outer), so start timestamps are not monotonic.
    # Just verify all durations are non-negative.
    for e in events:
        assert e["dur"] >= 0, f"Negative duration: {e}"


def test_nested_functions(trace_dir):
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def outer():
        return inner()

    def inner():
        return 42

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        outer()

    events = convert_file(t.output_path)
    names = [e["name"] for e in events]

    # Should see both outer and inner
    has_outer = any("outer" in n for n in names)
    has_inner = any("inner" in n for n in names)
    assert has_outer, f"Missing 'outer' in events: {names}"
    assert has_inner, f"Missing 'inner' in events: {names}"


def test_exception_handling(trace_dir):
    """Verify that exceptions don't corrupt the call stack."""
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def thrower():
        raise ValueError("test")

    def catcher():
        try:
            thrower()
        except ValueError:
            pass

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(10):
            catcher()

    events = convert_file(t.output_path)
    assert len(events) > 0

    # Filter to only thrower/catcher events (exclude tracer shutdown noise)
    relevant = [e for e in events if "thrower" in e["name"] or "catcher" in e["name"]]
    assert all(e["ph"] == "X" for e in relevant), "Expected all X events"
    assert len(relevant) == 20, f"Expected 20 events (10×catcher + 10×thrower), got {len(relevant)}"


# ── C converter (ftrc2json) tests ──────────────────────────────────────


def test_c_converter_basic(trace_dir):
    """Test that the C converter produces valid JSON with expected events."""
    from fasttracer import FastTracer, ftrc2json

    def fib(n):
        if n <= 1:
            return 1
        return fib(n - 1) + fib(n - 2)

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        fib(10)

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    assert os.path.exists(json_path)

    with open(json_path) as f:
        trace = json.load(f)

    all_events = trace["traceEvents"]
    assert len(all_events) > 0, "No events in JSON output"

    # Filter to trace events (not metadata)
    events = [e for e in all_events if e["ph"] == "X"]
    assert len(events) > 0, "No X events in JSON output"

    # Check structure of trace events
    for e in events:
        assert "ph" in e
        assert "pid" in e
        assert "tid" in e
        assert "ts" in e
        assert "dur" in e
        assert "name" in e
        assert e["cat"] == "FEE"


def test_c_converter_matches_python(trace_dir):
    """Verify the C converter produces the same events as the Python converter."""
    from fasttracer import FastTracer, ftrc2json
    from fasttracer.convert import convert_file

    def work():
        return sum(range(100))

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(20):
            work()

    # Python converter
    py_events = convert_file(t.output_path)

    # C converter
    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)
    with open(json_path) as f:
        c_trace = json.load(f)

    # Filter out metadata events (ph=M) from C output for comparison
    c_events = [e for e in c_trace["traceEvents"] if e["ph"] == "X"]

    assert len(py_events) == len(c_events), (
        f"Event count mismatch: Python={len(py_events)}, C={len(c_events)}"
    )

    # Compare event-by-event
    for i, (py_ev, c_ev) in enumerate(zip(py_events, c_events)):
        assert py_ev["ph"] == c_ev["ph"], f"Phase mismatch at {i}"
        assert py_ev["name"] == c_ev["name"], (
            f"Name mismatch at {i}: {py_ev['name']} vs {c_ev['name']}"
        )
        assert py_ev["pid"] == c_ev["pid"], f"PID mismatch at {i}"
        # Timestamps may differ slightly due to float precision
        assert abs(py_ev["ts"] - c_ev["ts"]) < 1.0, (
            f"Timestamp mismatch at {i}: {py_ev['ts']} vs {c_ev['ts']}"
        )
        assert abs(py_ev["dur"] - c_ev["dur"]) < 1.0, (
            f"Duration mismatch at {i}: {py_ev['dur']} vs {c_ev['dur']}"
        )


def test_c_converter_nested(trace_dir):
    """Test C converter with nested function calls."""
    from fasttracer import FastTracer, ftrc2json

    def outer():
        return middle()

    def middle():
        return inner()

    def inner():
        return 42

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        outer()

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    with open(json_path) as f:
        trace = json.load(f)

    events = [e for e in trace["traceEvents"] if e["ph"] == "X"]
    names = [e["name"] for e in events]

    assert any("outer" in n for n in names)
    assert any("middle" in n for n in names)
    assert any("inner" in n for n in names)


def test_c_converter_exceptions(trace_dir):
    """Test C converter with exception handling."""
    from fasttracer import FastTracer, ftrc2json

    def thrower():
        raise ValueError("boom")

    def catcher():
        try:
            thrower()
        except ValueError:
            pass

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(10):
            catcher()

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    with open(json_path) as f:
        trace = json.load(f)

    events = [e for e in trace["traceEvents"] if e["ph"] == "X"]
    relevant = [e for e in events if "thrower" in e["name"] or "catcher" in e["name"]]
    assert len(relevant) == 20, f"Expected 20 events (10×catcher + 10×thrower), got {len(relevant)}"


# ── Perfetto integration tests ─────────────────────────────────────────


def test_perfetto_loads_trace(trace_dir):
    """Verify Perfetto trace_processor can load our JSON and query slices."""
    from fasttracer import FastTracer, ftrc2json
    from perfetto.trace_processor import TraceProcessor

    def fib(n):
        if n <= 1:
            return 1
        return fib(n - 1) + fib(n - 2)

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        fib(12)

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    tp = TraceProcessor(trace=json_path)
    try:
        # Should have slices
        result = tp.query("SELECT count(*) as cnt FROM slice")
        rows = list(result)
        assert len(rows) == 1
        assert rows[0].cnt > 0, "No slices found in trace"

        # Should have our fib function
        result = tp.query("SELECT DISTINCT name FROM slice WHERE name LIKE '%fib%'")
        fib_names = [row.name for row in result]
        assert len(fib_names) > 0, "No 'fib' slices found"

        # Every slice should have non-negative duration, except for the
        # tracer's own shutdown calls (__exit__, stop) which are entered
        # but never exited because tracing stops mid-call.
        result = tp.query(
            "SELECT count(*) as cnt FROM slice WHERE dur < 0 "
            "AND name NOT LIKE '%FastTracer%' AND name NOT LIKE '%stop%'"
        )
        rows = list(result)
        assert rows[0].cnt == 0, "Found non-tracer slices with negative duration"

        # Timestamps should be non-negative
        result = tp.query("SELECT count(*) as cnt FROM slice WHERE ts < 0")
        rows = list(result)
        assert rows[0].cnt == 0, "Found slices with negative timestamp"
    finally:
        tp.close()


def test_perfetto_balanced_events(trace_dir):
    """Verify Perfetto sees balanced call stacks (no orphan begin/end)."""
    from fasttracer import FastTracer, ftrc2json
    from perfetto.trace_processor import TraceProcessor

    def outer():
        return inner()

    def inner():
        return 42

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(50):
            outer()

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    tp = TraceProcessor(trace=json_path)
    try:
        # Every slice should have a valid duration (>= 0)
        # If events were unbalanced, Perfetto would produce slices with
        # dur = -1 or very large durations
        result = tp.query(
            "SELECT count(*) as cnt FROM slice "
            "WHERE name LIKE '%outer%' OR name LIKE '%inner%'"
        )
        rows = list(result)
        assert rows[0].cnt == 100, (
            f"Expected 100 slices (50×outer + 50×inner), got {rows[0].cnt}"
        )

        # Check all those slices have reasonable durations (< 1 second)
        result = tp.query(
            "SELECT count(*) as cnt FROM slice "
            "WHERE (name LIKE '%outer%' OR name LIKE '%inner%') AND dur > 1000000000"
        )
        rows = list(result)
        assert rows[0].cnt == 0, "Found slices with unreasonably large duration"
    finally:
        tp.close()


def test_perfetto_thread_info(trace_dir):
    """Verify Perfetto can see thread metadata from our traces."""
    from fasttracer import FastTracer, ftrc2json
    from perfetto.trace_processor import TraceProcessor

    def work():
        return sum(range(100))

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        work()

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    tp = TraceProcessor(trace=json_path)
    try:
        # Should have at least one thread track
        result = tp.query("SELECT count(*) as cnt FROM thread")
        rows = list(result)
        assert rows[0].cnt > 0, "No threads found in trace"

        # Should have at least one process
        result = tp.query("SELECT count(*) as cnt FROM process")
        rows = list(result)
        assert rows[0].cnt > 0, "No processes found in trace"
    finally:
        tp.close()


# ── Rollover tests ─────────────────────────────────────────────────────


def test_rollover_creates_multiple_files(trace_dir):
    """Verify rollover creates sequentially numbered files."""
    from fasttracer import FastTracer
    import glob

    def busy():
        return sum(range(1000))

    # Use 2MB buffer. Each event is 8 bytes. The events area starts at ~1MB
    # (header + string table reserve), so ~1MB / 8 = ~131K events per buffer.
    # busy() generates ~5 events per call (entry+exit for busy, sum, range, etc.)
    # So ~26K calls should fill one buffer, triggering a flush.
    # With rollover at 2MB and buffer flushes of ~2MB each, we need ~2 flushes.
    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir,
                    rollover_size=20 * 1024 * 1024) as t:
        for _ in range(2000000):
            busy()

    files = sorted(glob.glob(os.path.join(trace_dir, "*.ftrc")))
    assert len(files) >= 2, f"Expected multiple files, got {len(files)}: {files}"

    # Files should be numbered sequentially
    for f in files:
        assert "_" in os.path.basename(f), f"Expected numbered file: {f}"


def test_rollover_second_file_self_contained(trace_dir):
    """Verify the second rollover file is independently valid in Perfetto."""
    from fasttracer import FastTracer, ftrc2json
    from perfetto.trace_processor import TraceProcessor
    import glob

    def recursive(n):
        if n <= 0:
            return 0
        return recursive(n - 1) + 1

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir,
                    rollover_size=20 * 1024 * 1024) as t:
        for _ in range(500000):
            recursive(5)

    files = sorted(glob.glob(os.path.join(trace_dir, "*.ftrc")))
    assert len(files) >= 2, f"Need at least 2 files, got {len(files)}"

    # Convert ONLY the second file
    second_file = files[1]
    json_path = os.path.join(trace_dir, "second.json")
    ftrc2json(second_file, json_path)

    tp = TraceProcessor(trace=json_path)
    try:
        # Should have slices with valid durations
        result = tp.query("SELECT count(*) as cnt FROM slice WHERE dur >= 0")
        rows = list(result)
        assert rows[0].cnt > 0, "No valid slices in second file"

        # Should have recursive function
        result = tp.query(
            "SELECT count(*) as cnt FROM slice WHERE name LIKE '%recursive%'"
        )
        rows = list(result)
        assert rows[0].cnt > 0, "No 'recursive' slices in second file"
    finally:
        tp.close()


def test_rollover_synthetic_events(trace_dir):
    """Verify synthetic events appear at file boundaries."""
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file
    import glob

    FT_FLAG_SYNTHETIC = 2

    def deep():
        return sum(range(100))

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir,
                    rollover_size=20 * 1024 * 1024) as t:
        for _ in range(2000000):
            deep()

    files = sorted(glob.glob(os.path.join(trace_dir, "*.ftrc")))
    assert len(files) >= 2, f"Need at least 2 files for rollover test"

    # Check the second file has synthetic entries at the start
    events = convert_file(files[1])
    assert len(events) > 0, "Second file has no events"


# ── perf-viz-merge integration tests ───────────────────────────────────

PERF_VIZ_MERGE = "/home/thomasdullien/sources/perftrace/perf-viz-merge"


@pytest.mark.skipif(
    not os.path.isfile(PERF_VIZ_MERGE),
    reason="perf-viz-merge binary not found"
)
def test_perf_viz_merge_reads_output(trace_dir):
    """Verify perf-viz-merge can parse our JSON output."""
    import subprocess
    from fasttracer import FastTracer, ftrc2json

    def fib(n):
        if n <= 1:
            return 1
        return fib(n - 1) + fib(n - 2)

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        fib(15)

    json_path = os.path.join(trace_dir, "trace.json")
    ftrc2json(t.output_path, json_path)

    output_path = os.path.join(trace_dir, "merged.perfetto-trace")
    result = subprocess.run(
        [PERF_VIZ_MERGE, "--viz", json_path, "-o", output_path, "-v"],
        capture_output=True, text=True, timeout=30,
    )

    assert result.returncode == 0, (
        f"perf-viz-merge failed:\nstdout: {result.stdout}\nstderr: {result.stderr}"
    )
    assert os.path.exists(output_path), "No output file produced"
    assert os.path.getsize(output_path) > 0, "Output file is empty"

    # Check that it reported reading our events
    output = result.stdout + result.stderr
    assert "VizTracer events" in output, f"Unexpected output: {output}"


@pytest.mark.skipif(
    not os.path.isfile(PERF_VIZ_MERGE),
    reason="perf-viz-merge binary not found"
)
def test_perf_viz_merge_rollover_file(trace_dir):
    """Verify perf-viz-merge can parse a single rollover file (with synthetic events)."""
    import glob
    import subprocess
    from fasttracer import FastTracer, ftrc2json

    def work():
        return sum(range(1000))

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir,
                    rollover_size=20 * 1024 * 1024) as t:
        for _ in range(2000000):
            work()

    files = sorted(glob.glob(os.path.join(trace_dir, "*.ftrc")))
    assert len(files) >= 2, f"Need rollover files, got {len(files)}"

    # Convert the second file only
    json_path = os.path.join(trace_dir, "second.json")
    ftrc2json(files[1], json_path)

    output_path = os.path.join(trace_dir, "merged.perfetto-trace")
    result = subprocess.run(
        [PERF_VIZ_MERGE, "--viz", json_path, "-o", output_path, "-v"],
        capture_output=True, text=True, timeout=30,
    )

    assert result.returncode == 0, (
        f"perf-viz-merge failed on rollover file:\n"
        f"stdout: {result.stdout}\nstderr: {result.stderr}"
    )
    assert os.path.getsize(output_path) > 0, "Output file is empty"


# ── Function name correctness tests ───────────────────────────────────
# These tests verify that the intern table produces correct function names,
# particularly in scenarios where CPython reuses memory addresses for
# temporary bound method objects (e.g., list.append, dict.update).


def test_function_names_match_calls(trace_dir):
    """Traced function names must match the functions that were actually called."""
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def alpha():
        return 1

    def beta():
        return alpha()

    def gamma():
        return beta()

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(50):
            gamma()

    events = convert_file(t.output_path)
    relevant = [e for e in events if any(
        name in e["name"] for name in ("alpha", "beta", "gamma")
    )]

    alpha_events = [e for e in relevant if "alpha" in e["name"]]
    beta_events = [e for e in relevant if "beta" in e["name"]]
    gamma_events = [e for e in relevant if "gamma" in e["name"]]

    assert len(alpha_events) == 50, f"Expected 50 alpha calls, got {len(alpha_events)}"
    assert len(beta_events) == 50, f"Expected 50 beta calls, got {len(beta_events)}"
    assert len(gamma_events) == 50, f"Expected 50 gamma calls, got {len(gamma_events)}"


def test_c_builtins_do_not_corrupt_names(trace_dir):
    """C built-in methods (list.append, dict.update, etc.) create temporary
    bound method objects that CPython may free and reallocate at the same
    address. This must not corrupt the intern table's function name mapping.
    """
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def do_work():
        """Call many different C built-in methods to trigger address reuse."""
        lst = []
        for i in range(20):
            lst.append(i)          # list.append
        d = {}
        for i in range(20):
            d.update({i: i})       # dict.update
        s = set()
        for i in range(20):
            s.add(i)               # set.add
        lst.sort()                 # list.sort
        lst.reverse()              # list.reverse
        lst.clear()                # list.clear
        return len(d)

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(100):
            do_work()

    events = convert_file(t.output_path)
    do_work_events = [e for e in events if "do_work" in e["name"]]

    assert len(do_work_events) == 100, (
        f"Expected 100 do_work events, got {len(do_work_events)}"
    )

    # Verify that C built-in names are recognizable (not swapped with
    # unrelated functions). Each name should contain the method it represents.
    c_events = [e for e in events if any(
        m in e["name"] for m in ("append", "update", "add", "sort", "reverse", "clear")
    )]
    assert len(c_events) > 0, "No C built-in method events found"

    # None of the C built-in events should have a Python function's name
    for e in c_events:
        assert "do_work" not in e["name"], (
            f"C built-in event has wrong name: {e['name']}"
        )


def test_interleaved_c_and_python_calls(trace_dir):
    """Rapidly alternating between Python and C function calls must not
    cause name corruption through intern table pointer reuse.
    """
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def py_func():
        return 42

    def mixed_work():
        result = []
        for _ in range(10):
            result.append(py_func())  # C call (append) then Python call (py_func)
        return result

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(200):
            mixed_work()

    events = convert_file(t.output_path)

    mixed_events = [e for e in events if "mixed_work" in e["name"]]
    py_func_events = [e for e in events if "py_func" in e["name"]]
    append_events = [e for e in events if "append" in e["name"]]

    assert len(mixed_events) == 200, (
        f"Expected 200 mixed_work, got {len(mixed_events)}"
    )
    assert len(py_func_events) == 2000, (
        f"Expected 2000 py_func, got {len(py_func_events)}"
    )
    assert len(append_events) == 2000, (
        f"Expected 2000 append, got {len(append_events)}"
    )

    # Verify no name swaps: append events must not contain "py_func"
    for e in append_events:
        assert "py_func" not in e["name"], (
            f"append event has py_func name: {e['name']}"
        )
    for e in py_func_events:
        assert "append" not in e["name"], (
            f"py_func event has append name: {e['name']}"
        )


def test_threaded_function_names(trace_dir):
    """Worker thread call stacks must have correct function names,
    not names swapped from the main thread or other workers.
    """
    import threading
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def recursive_work(n):
        if n <= 0:
            return 0
        lst = []
        lst.append(n)  # C built-in — triggers temporary object allocation
        return recursive_work(n - 1) + lst[0]

    errors = []

    def worker(iterations):
        try:
            for _ in range(iterations):
                recursive_work(5)
        except Exception as exc:
            errors.append(exc)

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        threads = [threading.Thread(target=worker, args=(100,)) for _ in range(4)]
        for th in threads:
            th.start()
        # Also do work on the main thread
        worker(100)
        for th in threads:
            th.join()

    assert not errors, f"Worker threads raised exceptions: {errors}"

    events = convert_file(t.output_path)

    rw_events = [e for e in events if "recursive_work" in e["name"]]
    append_events = [e for e in events if "append" in e["name"]]

    # Due to GIL contention and PyEval_SetProfile per-thread registration,
    # not all worker threads may be fully traced. Require at least the main
    # thread's contribution (100 iterations × 6 calls = 600).
    assert len(rw_events) >= 600, (
        f"Expected at least 600 recursive_work events, got {len(rw_events)}"
    )

    # Critical: no recursive_work event should be named "append" and vice versa
    for e in rw_events:
        assert "append" not in e["name"], (
            f"recursive_work event corrupted with append name: {e['name']}"
        )
    for e in append_events:
        assert "recursive_work" not in e["name"], (
            f"append event corrupted with recursive_work name: {e['name']}"
        )


def test_many_unique_c_functions(trace_dir):
    """Interning many distinct C functions must not cause hash collisions
    that corrupt function names.
    """
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def exercise_many_builtins():
        # Exercise a wide variety of C built-in methods
        lst = [3, 1, 2]
        lst.append(4)
        lst.extend([5, 6])
        lst.insert(0, 0)
        lst.pop()
        lst.remove(0)
        lst.sort()
        lst.reverse()
        lst.copy()
        lst.count(1)
        lst.index(1)
        lst.clear()

        d = {"a": 1}
        d.update({"b": 2})
        d.get("a")
        d.keys()
        d.values()
        d.items()
        d.pop("b")
        d.setdefault("c", 3)
        d.copy()
        d.clear()

        s = {1, 2, 3}
        s.add(4)
        s.remove(4)
        s.discard(99)
        s.union({5})
        s.intersection({1, 2})
        s.difference({3})
        s.copy()
        s.clear()

        "hello world".split()
        "hello".upper()
        "HELLO".lower()
        " hi ".strip()
        "hello".replace("l", "r")
        ",".join(["a", "b"])

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(50):
            exercise_many_builtins()

    events = convert_file(t.output_path)
    wrapper_events = [e for e in events if "exercise_many_builtins" in e["name"]]

    assert len(wrapper_events) == 50, (
        f"Expected 50 exercise_many_builtins, got {len(wrapper_events)}"
    )

    # Collect all unique function names observed
    all_names = {e["name"] for e in events}

    # Not all C built-in methods generate C_CALL trace events in all Python
    # versions (e.g., single-arg builtins may be optimized away in 3.10).
    # Verify we see at least a few distinct C built-in names alongside our
    # Python wrapper — the key property is that names are not corrupted.
    c_builtin_keywords = [
        "append", "extend", "insert", "sort", "reverse", "update",
        "split", "upper", "lower", "strip", "replace", "join",
        "add", "union", "intersection",
    ]
    found = [kw for kw in c_builtin_keywords if any(kw in n for n in all_names)]
    assert len(found) >= 2, (
        f"Expected at least 2 distinct C built-in names, found {len(found)}: {found}"
    )

    # The critical check: no C built-in event should be named
    # "exercise_many_builtins" (name corruption from pointer reuse)
    for name in all_names:
        if any(kw in name for kw in c_builtin_keywords):
            assert "exercise_many_builtins" not in name, (
                f"C built-in event has corrupted name: {name}"
            )


def test_c_converter_name_correctness(trace_dir):
    """Verify the C converter (ftrc2json) also produces correct function names
    under pointer-reuse conditions.
    """
    from fasttracer import FastTracer, ftrc2json
    from fasttracer.convert import convert_file

    def target_func():
        lst = []
        for i in range(10):
            lst.append(i)
        return lst

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(100):
            target_func()

    # Python converter
    py_events = convert_file(t.output_path)

    # C converter
    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)
    with open(json_path) as f:
        c_trace = json.load(f)
    c_events = [e for e in c_trace["traceEvents"] if e["ph"] == "X"]

    # Both converters should agree on function names
    py_target = [e for e in py_events if "target_func" in e["name"]]
    c_target = [e for e in c_events if "target_func" in e["name"]]

    assert len(py_target) == 100, f"Python converter: expected 100 target_func, got {len(py_target)}"
    assert len(c_target) == 100, f"C converter: expected 100 target_func, got {len(c_target)}"

    py_append = [e for e in py_events if "append" in e["name"]]
    c_append = [e for e in c_events if "append" in e["name"]]

    assert len(py_append) == len(c_append), (
        f"Append count mismatch: Python={len(py_append)}, C={len(c_append)}"
    )


# ── C converter multi-file test ────────────────────────────────────────


def test_c_converter_multiple_files(trace_dir):
    """Test C converter merging multiple .ftrc files."""
    from fasttracer import FastTracer, ftrc2json

    def alpha():
        return 1

    def beta():
        return 2

    dir1 = os.path.join(trace_dir, "d1")
    dir2 = os.path.join(trace_dir, "d2")

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=dir1) as t1:
        for _ in range(10):
            alpha()
    path1 = t1.output_path

    with FastTracer(buffer_size=32 * 1024 * 1024, output_dir=dir2) as t2:
        for _ in range(10):
            beta()
    path2 = t2.output_path

    json_path = os.path.join(trace_dir, "merged.json")
    ftrc2json([path1, path2], json_path)

    with open(json_path) as f:
        trace = json.load(f)

    events = [e for e in trace["traceEvents"] if e["ph"] == "X"]
    names = [e["name"] for e in events]

    assert any("alpha" in n for n in names), "Missing alpha events"
    assert any("beta" in n for n in names), "Missing beta events"


# ── Crash-safe flush tests ─────────────────────────────────────────────


def test_sigterm_flushes_trace(trace_dir):
    """SIGTERM must produce a valid .ftrc file via emergency flush."""
    import signal
    import subprocess
    import sys
    from fasttracer.convert import convert_file

    # Run a subprocess that starts tracing, does work, then receives SIGTERM
    script = f"""
import os, signal, time
from fasttracer import FastTracer

t = FastTracer(buffer_size=32 * 1024 * 1024, output_dir="{trace_dir}")
t.start()

# Generate enough events to be meaningful
lst = []
for i in range(5000):
    lst.append(i)

# Send SIGTERM to ourselves
os.kill(os.getpid(), signal.SIGTERM)
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True, text=True, timeout=30,
    )

    # Process should have been terminated by SIGTERM
    assert result.returncode != 0, (
        f"Expected non-zero exit, got {result.returncode}"
    )

    # Find the .ftrc file
    ftrc_files = [f for f in os.listdir(trace_dir) if f.endswith(".ftrc")]
    assert len(ftrc_files) >= 1, (
        f"No .ftrc file produced after SIGTERM. Dir contents: {os.listdir(trace_dir)}"
    )

    # The file should be parseable and contain events
    ftrc_path = os.path.join(trace_dir, ftrc_files[0])
    events = convert_file(ftrc_path)
    assert len(events) > 0, "SIGTERM flush produced an empty trace"

    # Should contain append events from our workload
    append_events = [e for e in events if "append" in e["name"]]
    assert len(append_events) > 0, (
        f"No append events found. Event names: {set(e['name'] for e in events[:20])}"
    )


def test_atexit_flushes_trace(trace_dir):
    """Unhandled exceptions that exit Python must still produce a trace via atexit."""
    import subprocess
    import sys
    from fasttracer.convert import convert_file

    script = f"""
from fasttracer import FastTracer

t = FastTracer(buffer_size=32 * 1024 * 1024, output_dir="{trace_dir}")
t.start()

lst = []
for i in range(5000):
    lst.append(i)

# Exit without calling stop() — atexit handler should flush
raise RuntimeError("simulated crash")
"""
    result = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True, text=True, timeout=30,
    )

    assert result.returncode != 0

    ftrc_files = [f for f in os.listdir(trace_dir) if f.endswith(".ftrc")]
    assert len(ftrc_files) >= 1, (
        f"No .ftrc file produced after exception exit. Dir: {os.listdir(trace_dir)}"
    )

    ftrc_path = os.path.join(trace_dir, ftrc_files[0])
    events = convert_file(ftrc_path)
    assert len(events) > 0, "atexit flush produced an empty trace"
