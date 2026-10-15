"""Exercise bounded production cadence telemetry without devices.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class RateTests(unittest.TestCase):
    def test_fixed_windows_causes_history_wrap_and_resident_limit(self):
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "rate-test"
            subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unknown-pragmas", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", str(ROOT / "tests/h264/rate_test.c"),
                            "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)
