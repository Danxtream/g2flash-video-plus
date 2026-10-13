"""Validate capture evidence without device access. SPDX-License-Identifier: GPL-3.0-only"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from video_client import shown_report, paired_offset

ROOT = Path(__file__).resolve().parents[2]


class ShownTests(unittest.TestCase):
    def report(self, lens, copy, trigger, uncertainty):
        values = (9, lens, 17, 17, 1000, copy, trigger, 500, 3, 17, 32, 16, 0, 0, uncertainty)
        return shown_report(bytes((1, 15, 0, lens)) + struct.pack('<15I', *values))

    def test_offset_uses_local_deltas_with_explicit_unknown_wire_uncertainty(self):
        left = self.report(1, 4, 0xfffffffe, 0xffffffff)
        right = self.report(2, 5010, 5000, 0xffffffff)
        self.assertEqual(left['trigger_to_copy_ms'], 6)
        estimate = paired_offset(left, right)
        self.assertEqual(estimate['offset_estimate_ms'], -4)
        self.assertIsNone(estimate['uncertainty_ms'])
        self.assertFalse(estimate['qualified'])
        with self.assertRaises(ValueError):
            paired_offset(right, left)
        right['ordinal'] += 1
        with self.assertRaises(ValueError):
            paired_offset(left, right)

    def test_ring_overwrites_without_ack_wait_and_refuses_exhaustion(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'shown-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unknown-pragmas', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', str(ROOT / 'tests/h264/shown_test.c'),
                            str(ROOT / 'patches/video/shown.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
