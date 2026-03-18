import os
import subprocess
import sys
from setuptools import setup, Extension
from setuptools.command.build_ext import build_ext


class BuildExtWithConverter(build_ext):
    """Custom build_ext that also compiles the ftrc2json C binary."""

    def run(self):
        # Build the Python C extension as normal
        build_ext.run(self)

        # Build ftrc2json standalone binary
        src = os.path.join("src", "fasttracer", "modules", "ftrc2json.c")
        # Place the binary next to the Python package
        out_dir = os.path.join(self.build_lib, "fasttracer")
        os.makedirs(out_dir, exist_ok=True)
        out = os.path.join(out_dir, "ftrc2json")

        cmd = [
            "cc", "-std=c11", "-O2", "-Wall", "-Wextra",
            "-Wno-missing-field-initializers",
            "-D_GNU_SOURCE",
            "-o", out, src,
        ]
        print(f"Building ftrc2json: {' '.join(cmd)}")
        subprocess.check_call(cmd)

        # Also copy to inplace location for development
        if self.inplace:
            inplace_dir = os.path.join("src", "fasttracer")
            inplace_out = os.path.join(inplace_dir, "ftrc2json")
            subprocess.check_call(["cp", out, inplace_out])


fasttracer_ext = Extension(
    "fasttracer._fasttracer",
    sources=[
        "src/fasttracer/modules/fasttracer.c",
        "src/fasttracer/modules/intern.c",
    ],
    include_dirs=["src/fasttracer/modules"],
    extra_compile_args=["-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-missing-field-initializers"],
    define_macros=[("_POSIX_C_SOURCE", "199309L")],
)

setup(
    name="fasttracer",
    version="0.1.0",
    description="High-performance binary trace recorder for Python",
    package_dir={"": "src"},
    packages=["fasttracer"],
    ext_modules=[fasttracer_ext],
    cmdclass={"build_ext": BuildExtWithConverter},
    package_data={"fasttracer": ["ftrc2json"]},
    python_requires=">=3.10",
    entry_points={
        "console_scripts": [
            "ftrc2json-py=fasttracer.convert:main",
        ],
    },
)
