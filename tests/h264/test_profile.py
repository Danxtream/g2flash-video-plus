"""Advertised bounded-profile capability and legacy reply compatibility.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct
from types import SimpleNamespace
import unittest
from video_client import VideoClient, PLAYBACK_CAPABILITIES


class ProfileCapabilityTests(unittest.TestCase):
    def client(self, bits):
        client = VideoClient.__new__(VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 1)
        client.raw_replies = lambda payload, length: {
            1: bytes((1, PLAYBACK_CAPABILITIES, 0, 255, 2, 16, 255, bits)) + struct.pack('<II', 77824, 8192)}
        return client

    def test_new_and_old_capabilities_are_explicit(self):
        for bits in range(1, 32, 2):
            self.assertEqual(self.client(bits).playback_capabilities()[1]['profiling'], bool(bits & 16))

    def test_unknown_or_missing_seek_schema_is_rejected(self):
        for bits in (0, 16, 32, 33, 255):
            with self.assertRaises(ValueError):
                self.client(bits).playback_capabilities()
