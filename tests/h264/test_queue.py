"""Check owned receive snapshots and fake-consumer lifetimes.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class QueueTests(unittest.TestCase):
    def test_snapshot_bounds_backpressure_lifetimes_and_lens_isolation(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "queue-test"
            subprocess.run([
                "clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                str(ROOT / "tests/h264/queue_test.c"), "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
