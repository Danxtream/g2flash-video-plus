"""Validate owner-bound playback controls without connections.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct
import unittest
from video_client import playback_state, PLAYBACK, VERSION


class PlaybackControlTests(unittest.TestCase):
    def test_position_is_presented_media_and_pending_is_explicit(self):
        raw = bytes((VERSION, PLAYBACK, 0, 2)) + struct.pack('<8I', 4, 9, 1, 1, 16, 937, 2000, 1)
        row = playback_state(raw)
        self.assertEqual(row['position_ms'], 937)
        self.assertEqual(row['shown'], 16)
        self.assertTrue(row['pending'])

    def test_bad_state_and_position_refused(self):
        for values in ((4, 9, 2, 1, 16, 937, 2000, 0), (4, 9, 1, 1, 16, 2001, 2000, 0)):
            with self.assertRaises(ValueError):
                playback_state(bytes((VERSION, PLAYBACK, 0, 2)) + struct.pack('<8I', *values))
