"""Completed-picture FIFO ownership under fatal sanitizers.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class DecodedQueueTests(unittest.TestCase):
    def test_owned_pixels_fifo_pins_cancel_and_exhaustion(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'decoded-queue-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unknown-pragmas', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', str(ROOT / 'tests/h264/decoded_queue_test.c'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
