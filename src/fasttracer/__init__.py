from fasttracer._fasttracer import FastTracer as _FastTracer


class FastTracer:
    """High-performance binary trace recorder.

    Records Python function entry/exit events into a compact 8-byte binary
    format using CLOCK_MONOTONIC timestamps (for correlation with perf data).

    Uses double-buffered mmap'd regions and fork-based async flushing.
    """

    def __init__(self, buffer_size=256 * 1024 * 1024, output_dir="/tmp/fasttracer"):
        self._tracer = _FastTracer(buffer_size=buffer_size, output_dir=output_dir)

    def start(self):
        """Start tracing."""
        self._tracer.start()

    def stop(self):
        """Stop tracing and perform final synchronous flush."""
        self._tracer.stop()

    @property
    def output_path(self):
        """Path to the output .ftrc file."""
        return self._tracer.get_output_path()

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.stop()
