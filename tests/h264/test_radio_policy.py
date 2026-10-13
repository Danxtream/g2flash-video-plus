"""PC policy is a future boundary selection, not an actual PHY claim.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct
from types import SimpleNamespace
import unittest
from video_client import VideoClient, radio_policy_state, RADIO_POLICY


class PolicyTests(unittest.TestCase):
    def test_query_and_field_select_bind_source_without_live_switch(self):
        client = VideoClient.__new__(VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 9)
        sent = []
        def replies(payload, length):
            sent.append((payload, length))
            return {lens: bytes((1, RADIO_POLICY, 0, lens)) + struct.pack('<5I', 1, 0, 1, lens == 2, 0)
                    for lens in (1, 2)}
        client.raw_replies = replies
        self.assertTrue(client.radio_policy()[2]['wire_fed'])
        self.assertEqual(struct.unpack('<BBBBII', sent[0][0]), (31, 21, 1, 0, 9, 0xffffffff))
        self.assertTrue(client.radio_policy('video')[1]['pending'])
        self.assertEqual(struct.unpack_from('<I', sent[1][0], 8)[0], 1)
        client.radio_policy('off')
        self.assertEqual(struct.unpack_from('<I', sent[2][0], 8)[0], 0)
        with self.assertRaises(ValueError): client.radio_policy('always')

    def test_idle_active_and_pending_combinations_are_explicit(self):
        row = radio_policy_state(bytes((1, 21, 0, 1)) + struct.pack('<5I', 1, 0xffffffff, 0, 0, 0))
        self.assertIsNone(row['active'])
        for values in ((1, 0, 0, 0, 0), (2, 1, 0, 0, 0), (1, 1, 1, 0, 0)):
            with self.assertRaises(ValueError): radio_policy_state(bytes((1, 21, 0, 1)) + struct.pack('<5I', *values))
