"""Exercise RIGHT-selected two-lens FIFO triggers. SPDX-License-Identifier: GPL-3.0-only"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SyncTests(unittest.TestCase):
    def test_two_lens_readiness_completion_replays_and_exhaustion(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "sync-test"
            subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-Wno-unknown-pragmas", "-fsanitize=address,undefined",
                            "-fno-sanitize-recover=all", str(ROOT / "tests/h264/sync_test.c"),
                            str(ROOT / "patches/video/sync.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
