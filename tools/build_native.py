#!/usr/bin/env python3
"""Portable local build for machines with a C++17 compiler (CMake is optional)."""
import os
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
out = root / ".local/bin"
out.mkdir(parents=True, exist_ok=True)
compiler = os.environ.get("CXX", "g++")
flags = ["-std=c++17", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(root / "native/include")]
subprocess.run(
    [
        compiler,
        *flags,
        str(root / "native/src/macho.cpp"),
        str(root / "native/src/main.cpp"),
        "-o",
        str(out / "radek-macho"),
    ],
    check=True,
)
subprocess.run(
    [compiler, *flags, str(root / "native/tests/runtime.cpp"), "-o", str(out / "runtime-tests")], check=True
)
subprocess.run([str(out / "runtime-tests")], check=True)
print("Native analyzer and experimental runtime tests passed")
