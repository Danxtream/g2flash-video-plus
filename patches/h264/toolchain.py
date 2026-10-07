"""Locate ARM headers for the decoder flags used for the glasses speed tests.

SPDX-License-Identifier: GPL-3.0-only
Run from the repository root: python3 patches/h264/toolchain.py.
Explicit include overrides are local arguments, never committed configuration.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
MULTILIB = "thumb/v7e-m+fp/softfp"
TARGET_FLAGS = [
    "--target=thumbv7em-none-eabi", "-mthumb", "-mfpu=fpv4-sp-d16",
    "-mfloat-abi=softfp", "-ffp-contract=off", "-fno-jump-tables",
    "-fomit-frame-pointer", "-fno-builtin", "-mno-unaligned-access",
    "-fno-unwind-tables", "-fno-asynchronous-unwind-tables", "-Wall", "-Wextra",
]
CXX_FLAGS = [
    "-std=c++23", "-DNDEBUG", "-fPIC", "-fvisibility=hidden",
    "-fvisibility-inlines-hidden", "-fno-exceptions", "-fno-rtti",
    "-fno-threadsafe-statics", "-fstack-usage",
]


def include_search_paths(diagnostics):
    """Extract GCC's actual system include search list, excluding diagnostics."""
    start = "#include <...> search starts here:"
    end = "End of search list."
    if start not in diagnostics or end not in diagnostics.split(start, 1)[1]:
        raise RuntimeError("ARM GCC did not report its include search list")
    body = diagnostics.split(start, 1)[1].split(end, 1)[0]
    return [Path(line.strip()).resolve() for line in body.splitlines() if line.strip()]


def validate_headers(cxx_root, c_root):
    """Require the softfp multilib and C/C++ headers used for the glasses speed tests."""
    cxx_root, c_root = Path(cxx_root).resolve(), Path(c_root).resolve()
    multilib = cxx_root / "arm-none-eabi" / MULTILIB
    for header in (cxx_root / "vector", multilib / "bits/c++config.h",
                   c_root / "stdlib.h", c_root / "stdio.h"):
        if not header.is_file():
            raise RuntimeError(f"missing ARM toolchain header: {header}")
    return [cxx_root, multilib, c_root]


def discover_headers(gxx="arm-none-eabi-g++", cxx_root=None, c_root=None):
    """Discover installed GCC headers or validate an explicit local pair."""
    if cxx_root is not None or c_root is not None:
        if cxx_root is None or c_root is None:
            raise ValueError("both C++ and C include roots must be supplied")
        return validate_headers(cxx_root, c_root)
    result = subprocess.run(
        [gxx, "-mcpu=cortex-m4", "-mfpu=fpv4-sp-d16", "-mfloat-abi=softfp",
         "-E", "-x", "c++", "-v", "-"],
        input="", capture_output=True, text=True, check=True,
        env={**os.environ, "LC_ALL": "C"},
    )
    paths = include_search_paths(result.stderr)
    cxx = next((p for p in paths if (p / "vector").is_file()), None)
    c = next((p for p in paths if (p / "stdlib.h").is_file()
              and (p / "stdio.h").is_file()), None)
    if cxx is None or c is None:
        raise RuntimeError("install ARM GCC's newlib C and C++ development headers")
    headers = validate_headers(cxx, c)
    if headers[1] not in paths:
        raise RuntimeError("ARM GCC selected a different C++ multilib")
    return headers


def compiler_flags(headers, optimization="-O2"):
    """Keep the decoder flags from the glasses speed tests, without C++ -ffreestanding."""
    if optimization not in ("-O2", "-Oz"):
        raise ValueError("decoder optimization must be -O2 or -Oz")
    flags = [*TARGET_FLAGS, optimization, *CXX_FLAGS]
    for path in headers:
        flags += ["-isystem", str(path)]
    component = ROOT / "third-party/sub0h264/components/sub0h264"
    flags += ["-I" + str(component / "include"), "-I" + str(component / "src")]
    return flags


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arm-gxx", default="arm-none-eabi-g++")
    parser.add_argument("--cxx-include-root", type=Path)
    parser.add_argument("--c-include-root", type=Path)
    args = parser.parse_args()
    headers = discover_headers(args.arm_gxx, args.cxx_include_root, args.c_include_root)
    print(json.dumps({"headers": [str(p) for p in headers],
                      "cxx_flags": compiler_flags(headers)}, indent=2))


if __name__ == "__main__":
    main()
