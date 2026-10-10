"""Bounded decode aggregates with fatal sanitizer and wire-format regressions.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest

from video_client import decode_totals, status, STATUS
from test_video_client import status_bytes
from test_video_link import WindowWire
from test_receive_client import Counter
from video_client import VideoClient

ROOT = Path(__file__).resolve().parents[2]


class DecodeTimingTests(unittest.TestCase):
    def test_multi_nal_buckets_wrap_and_saturation_with_fatal_sanitizers(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory)/'decode-timing'
            subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(ROOT/'tests/h264/decode_timing_test.c'), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_wire_length_feature_counts_and_empty_statistics(self):
        base = status(status_bytes())
        self.assertEqual(base['status_bytes'], 192)
        self.assertIsNone(base['decode_totals'])
        totals = status(status_bytes(extended=True))['decode_totals']
        self.assertIsNone(totals['mean_ms'])
        raw = bytearray(64); raw[0] = 1
        struct.pack_into('<HIQQII', raw, 2, 2, 2, 25000000, 100, 15000000, 60)
        struct.pack_into('<6I', raw, 32, 0, 1, 1, 0, 0, 0)
        totals = decode_totals(raw)
        self.assertEqual((totals['mean_ms'], totals['cycles_per_wall_tick_mhz'], totals['header_calls']), (50, 250, 2))
        for offset, value in ((0, 2), (1, 4), (4, 3), (32, 1)):
            corrupt = bytearray(raw); corrupt[offset] = value
            with self.assertRaises(ValueError): decode_totals(corrupt)
        raw[1] = 2; raw[4] = 255
        self.assertEqual(decode_totals(raw)['flags'], 2)
        for length in (0, 63, 65):
            with self.assertRaises(ValueError): decode_totals(bytes(length))

    def test_extended_status_pages_at_minimum_mtu_and_pair_keep_caps_unchanged(self):
        for page_bytes in (1, 5):
            client = VideoClient(WindowWire(page_bytes), 3, Counter(), ingress=1)
            totals = client.command(STATUS)
            self.assertEqual(totals['by_lens'][2]['decode_totals']['pictures'], 0)
            self.assertFalse(client.pages)


if __name__ == '__main__':
    unittest.main()
