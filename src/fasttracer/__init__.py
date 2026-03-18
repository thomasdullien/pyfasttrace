import os
import subprocess

from fasttracer._fasttracer import FastTracer as _FastTracer

# Path to the C converter binary, installed alongside the package
_FTRC2JSON_PATH = os.path.join(os.path.dirname(__file__), "ftrc2json")


def ftrc2json(input_paths, output_path):
    """Convert .ftrc file(s) to Chrome Trace JSON using the C converter.

    Args:
        input_paths: str or list of str — paths to .ftrc files
        output_path: str — path for the output .json file
    """
    if isinstance(input_paths, str):
        input_paths = [input_paths]

    cmd = [_FTRC2JSON_PATH, "-o", output_path] + input_paths
    subprocess.check_call(cmd)


class FastTracer:
    """High-performance binary trace recorder.

    Records Python function entry/exit events into a compact 8-byte binary
    format using CLOCK_MONOTONIC timestamps (for correlation with perf data).

    Uses double-buffered mmap'd regions and fork-based async flushing.
    """

    def __init__(self, buffer_size=256 * 1024 * 1024, output_dir="/tmp/fasttracer",
                 rollover_size=0):
        self._tracer = _FastTracer(buffer_size=buffer_size, output_dir=output_dir,
                                    rollover_size=rollover_size)
        self._output_dir = output_dir

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
