"""Basic tests for fasttracer."""

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
