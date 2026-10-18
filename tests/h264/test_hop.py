"""Bounded hop timing, synthetic clocks only. SPDX-License-Identifier: GPL-3.0-only"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HopTests(unittest.TestCase):
    def test_both_ingresses_peer_ack_order_wrap_contention_and_retention(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / "hop-test"
            subprocess.run(["clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            str(ROOT / "tests/h264/hop_test.c"), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
