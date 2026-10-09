"""Exercise dequantization overflow regressions with fatal sanitizers.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DequantizationTests(unittest.TestCase):
    def test_modular_products_and_bounded_sibling_paths(self):
        tests = ROOT / "third-party/sub0h264/tests"
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "dequant-test"
            # Upstream doctest includes obsolete ciso646; keep its warning visible.
            subprocess.run(["clang++", "-std=c++23", "-O2", "-g", "-DNDEBUG",
                            "-Wall", "-Wextra", "-Werror", "-Wno-error=#warnings",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-I" + str(tests / "vendor"), str(tests / "test_main.cpp"),
                            str(tests / "test_dequant_wrap.cpp"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=60)
