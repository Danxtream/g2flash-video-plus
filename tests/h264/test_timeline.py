"""Picture metadata ownership and supplied-link protocol; no device access.

SPDX-License-Identifier: GPL-3.0-only
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
import struct
from types import SimpleNamespace
from unittest.mock import patch

import video_client

ROOT = Path(__file__).resolve().parents[2]


class TimelineTests(unittest.TestCase):
    def client(self):
        client = video_client.VideoClient.__new__(video_client.VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 7)
        self.sent = []
        def replies(payload, length):
            self.sent.append((payload, length))
            return {1: b'', 2: b''}
        client.raw_replies = replies
        return client

    def test_pc_timeline_and_picture_records_are_distinct_bounded_controls(self):
        client = self.client()
        with patch.object(video_client, 'status', lambda raw: dict(state=0, result=0)):
            client.configure_timeline(9, 2000, 500)
            client.frame_time(9, 1, 4, 500)
        self.assertEqual(struct.unpack('<BBBBIIIIHH', self.sent[0][0]),
                         (31, 11, 1, 0, 7, 9, 2000, 500, 16, 1))
        self.assertEqual(struct.unpack('<BBBBIIIII', self.sent[1][0]),
                         (31, 12, 1, 0, 7, 9, 1, 4, 500))
        self.assertEqual([len(payload) for payload, _ in self.sent], [24, 24])

    def test_invalid_pc_records_never_send(self):
        client = self.client()
        for args in ((0, 2000, 0), (1, 0, 0), (1, 2000, 2001)):
            with self.assertRaises(ValueError):
                client.configure_timeline(*args)
        for args in ((0, 1, 0, 0), (1, 0, 0, 0), (1, 1, -1, 0), (1, 1, 0, -1)):
            with self.assertRaises(ValueError):
                client.frame_time(*args)
        self.assertFalse(self.sent)

    def test_compact_metadata_has_independent_credits_and_pair_capability(self):
        client = self.client()
        captured = []
        def replies(payload, length, **kwargs):
            captured.append((payload, length, kwargs))
            if payload[1] == video_client.PLAYBACK_CAPABILITIES:
                raw = bytes((1, 13, 0, 7, 2, 16, 0, 0)) + struct.pack('<II', 77824, 8192)
            else:
                raw = struct.pack('<IBB', 3, 2, 0)
            return {1: raw, 2: raw}
        client.raw_replies = replies
        self.assertTrue(client.playback_capabilities()[2]['paired'])
        report = client.frame_time_fast(9, 3, 11, 125)
        self.assertEqual(report['result'], 0)
        self.assertEqual(report['by_lens'][1], dict(accepted=3, credits=2, result=0))
        self.assertEqual(struct.unpack('<BBBBIIIII', captured[1][0]), (31, 14, 1, 0, 7, 9, 3, 11, 125))
        self.assertEqual(captured[1][1:], (6, dict(result_offset=5)))

    def test_bounded_picture_metadata_under_fatal_sanitizers(self):
        with tempfile.TemporaryDirectory() as directory:
            exe = Path(directory) / 'timeline-test'
            subprocess.run(['clang', '-std=c11', '-O2', '-g', '-Wall', '-Wextra', '-Werror',
                            '-Wno-unknown-pragmas', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=all', str(ROOT / 'tests/h264/timeline_test.c'),
                            '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True, timeout=30)
