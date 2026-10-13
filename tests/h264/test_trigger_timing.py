"""Task-owned nominal trigger evidence; no device access.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest

from video_client import VideoClient, shown_report, trigger_timing_report

ROOT = Path(__file__).resolve().parents[2]


class TriggerTimingTests(unittest.TestCase):
    def report(self):
        values = (9, 10, 32, 1937, 20, 8, 6, 10, 16, 0xfffffff8, 16,
                  1, 32, 0, 4, 16, 0, 1, 400, 1, 73200, 73200, 77824)
        return bytearray(bytes((1, 22, 0, 2)) + struct.pack('<23I', *values))

    def test_nominal_offset_is_signed_and_not_clock_or_optically_qualified(self):
        row = trigger_timing_report(self.report())
        self.assertEqual((row['offset_lower_ms'], row['offset_upper_ms']), (-8, 16))
        self.assertEqual(row['offset_estimate_ms'], 4)
        self.assertEqual(row['nominal_uncertainty_ms'], 12)
        self.assertFalse(row['qualified'])
        for offset, value in ((80, 2), (72, 3), (76, 8193), (88, 77825), (92, 0), (48, 2), (44, 0xfffffff7)):
            raw = self.report(); struct.pack_into('<I', raw, offset, value)
            with self.assertRaises(ValueError): trigger_timing_report(raw)

    def test_extended_shown_captures_and_old_report_shape_are_both_supported(self):
        values = (9, 1, 1, 1, 0, 110, 100, 90, 0, 1, 1, 0, 0, 0, 0)
        old = bytes((1, 15, 0, 1)) + struct.pack('<15I', *values)
        self.assertIsNone(shown_report(old)['submit_tick'])
        new = shown_report(old + struct.pack('<II', 101, 104))
        self.assertEqual((new['claim_tick'], new['submit_tick']), (101, 104))

    def test_owner_bound_request_and_capability_selected_report_length(self):
        client = VideoClient.__new__(VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 7)
        client.targets = 2
        sent = []
        def replies(payload, length, **kwargs):
            sent.append((payload, length, kwargs))
            if payload[1] == 13:
                return {2: bytes((1, 13, 0, 255, 2, 16, 255, 7)) + struct.pack('<II', 77824, 8192)}
            return {2: self.report()}
        client.raw_replies = replies
        self.assertTrue(client.playback_capabilities()[2]['trigger_timing'])
        self.assertEqual(client.shown_bytes[2], 72)
        self.assertTrue(client.trigger_timing(9, {2: 10})[2]['valid'])
        self.assertEqual(struct.unpack('<BBBBIIII', sent[-1][0]), (31, 22, 1, 0, 7, 9, 0, 10))
        with self.assertRaises(ValueError): client.trigger_timing(9, {})
        with self.assertRaises(ValueError): client.trigger_timing(8, {2: 10})

    def test_nominal_bounds_wrap_retries_anomalies_and_saturation(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'trigger-timing-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                str(ROOT / 'tests/h264/trigger_timing_test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
