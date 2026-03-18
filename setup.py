from setuptools import setup, Extension

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
    python_requires=">=3.10",
    entry_points={
        "console_scripts": [
            "ftrc2json=fasttracer.convert:main",
        ],
    },
)
