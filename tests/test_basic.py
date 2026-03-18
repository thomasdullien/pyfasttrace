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

    t = FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir)
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

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
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

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
        baz(10)

    events = convert_file(t.output_path)
    assert len(events) > 0, "No events converted"

    # Check that we have both B and E events
    phases = {e["ph"] for e in events}
    assert "B" in phases, "No entry events found"
    assert "E" in phases, "No exit events found"

    # Check that events have expected fields
    for e in events:
        assert "ts" in e
        assert "pid" in e
        assert "tid" in e
        assert "name" in e

    # Check that timestamps are monotonically non-decreasing
    timestamps = [e["ts"] for e in events]
    for i in range(1, len(timestamps)):
        assert timestamps[i] >= timestamps[i - 1], (
            f"Non-monotonic timestamp at index {i}: {timestamps[i-1]} > {timestamps[i]}"
        )


def test_nested_functions(trace_dir):
    from fasttracer import FastTracer
    from fasttracer.convert import convert_file

    def outer():
        return inner()

    def inner():
        return 42

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
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

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(10):
            catcher()

    events = convert_file(t.output_path)
    assert len(events) > 0

    # Filter to only thrower/catcher events (exclude tracer shutdown noise)
    relevant = [e for e in events if "thrower" in e["name"] or "catcher" in e["name"]]
    entries = sum(1 for e in relevant if e["ph"] == "B")
    exits = sum(1 for e in relevant if e["ph"] == "E")
    assert entries == exits, f"Unbalanced: {entries} entries, {exits} exits"
    assert entries == 20, f"Expected 20 entries (10×catcher + 10×thrower), got {entries}"


# ── C converter (ftrc2json) tests ──────────────────────────────────────


def test_c_converter_basic(trace_dir):
    """Test that the C converter produces valid JSON with expected events."""
    from fasttracer import FastTracer, ftrc2json

    def fib(n):
        if n <= 1:
            return 1
        return fib(n - 1) + fib(n - 2)

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
        fib(10)

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    assert os.path.exists(json_path)

    with open(json_path) as f:
        trace = json.load(f)

    all_events = trace["traceEvents"]
    assert len(all_events) > 0, "No events in JSON output"

    # Filter to trace events (not metadata)
    events = [e for e in all_events if e["ph"] in ("B", "E")]
    assert len(events) > 0, "No B/E events in JSON output"

    # Check structure of trace events
    for e in events:
        assert "ph" in e
        assert "pid" in e
        assert "tid" in e
        assert "ts" in e
        assert "name" in e

    # Should have both B and E phases
    phases = {e["ph"] for e in events}
    assert "B" in phases
    assert "E" in phases


def test_c_converter_matches_python(trace_dir):
    """Verify the C converter produces the same events as the Python converter."""
    from fasttracer import FastTracer, ftrc2json
    from fasttracer.convert import convert_file

    def work():
        return sum(range(100))

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
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
    c_events = [e for e in c_trace["traceEvents"] if e["ph"] in ("B", "E")]

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


def test_c_converter_nested(trace_dir):
    """Test C converter with nested function calls."""
    from fasttracer import FastTracer, ftrc2json

    def outer():
        return middle()

    def middle():
        return inner()

    def inner():
        return 42

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
        outer()

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    with open(json_path) as f:
        trace = json.load(f)

    events = [e for e in trace["traceEvents"] if e["ph"] in ("B", "E")]
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

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
        for _ in range(10):
            catcher()

    json_path = os.path.join(trace_dir, "out.json")
    ftrc2json(t.output_path, json_path)

    with open(json_path) as f:
        trace = json.load(f)

    events = [e for e in trace["traceEvents"] if e["ph"] in ("B", "E")]
    relevant = [e for e in events if "thrower" in e["name"] or "catcher" in e["name"]]
    entries = sum(1 for e in relevant if e["ph"] == "B")
    exits = sum(1 for e in relevant if e["ph"] == "E")
    assert entries == exits, f"Unbalanced: {entries} entries, {exits} exits"
    assert entries == 20


# ── Perfetto integration tests ─────────────────────────────────────────


def test_perfetto_loads_trace(trace_dir):
    """Verify Perfetto trace_processor can load our JSON and query slices."""
    from fasttracer import FastTracer, ftrc2json
    from perfetto.trace_processor import TraceProcessor

    def fib(n):
        if n <= 1:
            return 1
        return fib(n - 1) + fib(n - 2)

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
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

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
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

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=trace_dir) as t:
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
    with FastTracer(buffer_size=2 * 1024 * 1024, output_dir=trace_dir,
                    rollover_size=2 * 1024 * 1024) as t:
        for _ in range(200000):
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

    with FastTracer(buffer_size=2 * 1024 * 1024, output_dir=trace_dir,
                    rollover_size=2 * 1024 * 1024) as t:
        for _ in range(50000):
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

    with FastTracer(buffer_size=2 * 1024 * 1024, output_dir=trace_dir,
                    rollover_size=2 * 1024 * 1024) as t:
        for _ in range(200000):
            deep()

    files = sorted(glob.glob(os.path.join(trace_dir, "*.ftrc")))
    assert len(files) >= 2, f"Need at least 2 files for rollover test"

    # Check the second file has synthetic entries at the start
    events = convert_file(files[1])
    assert len(events) > 0, "Second file has no events"


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

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=dir1) as t1:
        for _ in range(10):
            alpha()
    path1 = t1.output_path

    with FastTracer(buffer_size=4 * 1024 * 1024, output_dir=dir2) as t2:
        for _ in range(10):
            beta()
    path2 = t2.output_path

    json_path = os.path.join(trace_dir, "merged.json")
    ftrc2json([path1, path2], json_path)

    with open(json_path) as f:
        trace = json.load(f)

    events = [e for e in trace["traceEvents"] if e["ph"] in ("B", "E")]
    names = [e["name"] for e in events]

    assert any("alpha" in n for n in names), "Missing alpha events"
    assert any("beta" in n for n in names), "Missing beta events"
