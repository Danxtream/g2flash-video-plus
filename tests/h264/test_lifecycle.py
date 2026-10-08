"""Check cancellation, stale users and park-before-reclaim ownership.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class LifecycleTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "lifecycle-test"
        subprocess.run([
            "clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            str(ROOT / "patches/video/lifecycle.c"),
            str(ROOT / "tests/h264/lifecycle_test.c"), "-o", str(cls.exe),
        ], check=True)

    def check_case(self, name):
        subprocess.run([str(self.exe), name], check=True)

    def test_idle_two_starters_reset_and_generation_wrap(self):
        self.check_case("generations")

    def test_cancel_before_publication_and_partial_start_rollback(self):
        self.check_case("startup")

    def test_constructor_and_decode_fatal_park(self):
        self.check_case("faults")

    def test_timeout_or_termination_failure_quarantines_storage(self):
        self.check_case("quarantine")

    def test_output_and_timer_pins_block_free_and_reject_stale_callbacks(self):
        self.check_case("users")
