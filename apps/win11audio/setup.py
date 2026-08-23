from setuptools import setup
from pybind11.setup_helpers import Pybind11Extension, build_ext
import sys

# Define C++ extension module
ext_modules = [
    Pybind11Extension(
        "dsp_engine",
        ["dsp_engine.cpp"],
        cxx_std=17,
        extra_compile_args=["-O3"] if sys.platform != "win32" else ["/O2"],
    ),
]

setup(
    name="win11audio-dsp",
    version="0.1.0",
    description="Windows 11 C++ DSP audio processing pipeline with Python bindings",
    author="Stefan Skoog",
    ext_modules=ext_modules,
    cmdclass={"build_ext": build_ext},
    python_requires=">=3.10",
)
