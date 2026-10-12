"""Verify exact rational deadlines and wrap refusal. SPDX-License-Identifier: GPL-3.0-only"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PacingTests(unittest.TestCase):
    def test_rational_deadlines_and_finite_generations(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "pacing-test"
            subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unknown-pragmas", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", str(ROOT / "tests/h264/pacing_test.c"),
                            str(ROOT / "patches/video/pacing.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
