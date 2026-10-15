"""Rescale-only negotiation and retired development-field checks.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct
from types import SimpleNamespace
import unittest
from video_client import VideoClient, DirectPairClient, PLAYBACK_CAPABILITIES


class GrayCurveTests(unittest.TestCase):
    def client(self, advanced):
        client = VideoClient.__new__(VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 9)
        client.raw_replies = lambda payload, length: {
            lens: bytes((1, PLAYBACK_CAPABILITIES, 0, 255, 2, 7, 255, advanced)) +
                struct.pack('<II', 77824, 8192) for lens in (1, 2)
        }
        return client

    def test_retired_gray_field_has_no_client_api(self):
        self.assertFalse(hasattr(VideoClient, 'gray_curve'))
        self.assertFalse(hasattr(DirectPairClient, 'gray_curve'))

    def test_rescale_only_capabilities_preserve_dither_profile_and_both_lenses(self):
        rows = self.client(23).playback_capabilities()
        self.assertEqual(set(rows), {1, 2})
        for row in rows.values():
            self.assertNotIn('gray_curve', row)
            self.assertTrue(row['dither'] and row['profiling'] and row['trigger_timing'])

    def test_legacy_capability_bit_does_not_restore_retired_curve_api(self):
        for row in self.client(31).playback_capabilities().values():
            self.assertNotIn('gray_curve', row)
            self.assertTrue(row['profiling'])
        self.assertFalse(hasattr(VideoClient, 'gray_curve'))
