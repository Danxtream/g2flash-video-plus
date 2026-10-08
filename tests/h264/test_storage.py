"""Check request-sized cached ownership with failure injection and sanitizers.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class StorageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temp.cleanup)
        cls.exe = Path(cls.temp.name) / "storage-test"
        subprocess.run([
            "clang", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            str(ROOT / "patches/video/storage.c"),
            str(ROOT / "tests/h264/storage_test.c"), "-o", str(cls.exe),
        ], check=True)

    def check_case(self, name):
        subprocess.run([str(self.exe), name], check=True)

    def test_batch_sizes_tags_alignment_and_unused_reservations(self):
        self.check_case("batch")

    def test_every_partial_batch_allocation_rolls_back(self):
        self.check_case("rollback")

    def test_fragmentation_all_heap_reserves_and_allocation_race(self):
        self.check_case("reserves")

    def test_arithmetic_ledger_and_pointer_bounds(self):
        self.check_case("bounds")

    def test_old_and_new_capacity_count_together(self):
        self.check_case("growth")
