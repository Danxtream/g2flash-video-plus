"""Check allocation-free accounting with synthetic heaps. SPDX-License-Identifier: GPL-3.0-only"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HeapMeterTests(unittest.TestCase):
    def test_minima_ledgers_external_delta_contention_and_generation(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'heap-meter'
            subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(ROOT / 'tests/h264/heap_meter_test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
