"""Check the stock refresh adapter without a display or device connection.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class HandoffTests(unittest.TestCase):
    def test_refresh_queue_copies_exact_records_and_refuses_unknown_shapes(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'handoff-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unused-function', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all',
                            str(ROOT / 'tests/h264/handoff_test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
