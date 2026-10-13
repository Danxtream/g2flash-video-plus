"""Development gray field framing, frozen generation and capability checks.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct
from types import SimpleNamespace
import unittest
from video_client import VideoClient, DirectPairClient, GRAY_CURVE, gray_curve_state


class GrayCurveTests(unittest.TestCase):
    def test_query_select_source_and_pending_are_explicit(self):
        client = VideoClient.__new__(VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 9)
        sent = []
        def replies(payload, length):
            sent.append((payload, length))
            return {lens: bytes((1, GRAY_CURVE, 0, lens)) + struct.pack('<5I', 1, 0, 1, 0, 0)
                    for lens in (1, 2)}
        client.raw_replies = replies
        self.assertTrue(client.gray_curve()[1]['pending'])
        self.assertEqual(struct.unpack('<BBBBII', sent[0][0]), (31, 23, 1, 0, 9, 0xffffffff))
        client.gray_curve('rescale')
        self.assertEqual(struct.unpack_from('<I', sent[1][0], 8)[0], 0)
        client.gray_curve('raw')
        self.assertEqual(struct.unpack_from('<I', sent[2][0], 8)[0], 1)
        self.assertEqual(sent[2][1], 24)
        with self.assertRaises(ValueError): client.gray_curve('gamma')

    def test_invalid_fields_source_and_reserved_are_refused(self):
        for values in ((2, 0, 1, 0, 0), (1, 0, 0, 0, 0), (1, 1, 1, 0, 0),
                       (0, 0xffffffff, 1, 0, 0), (0, 0, 0, 1, 0), (0, 0, 0, 0, 1)):
            with self.assertRaises(ValueError):
                gray_curve_state(bytes((1, 23, 0, 1)) + struct.pack('<5I', *values))
        row = gray_curve_state(bytes((1, 23, 0, 2)) + struct.pack('<5I', 0, 0xffffffff, 0, 0, 0))
        self.assertIsNone(row['active'])

    def test_direct_pair_forwards_same_selected_curve_to_each_local_link(self):
        sent = []
        pair = DirectPairClient.__new__(DirectPairClient)
        pair.clients = {lens: SimpleNamespace(gray_curve=lambda mode, lens=lens:
            (sent.append((lens, mode)) or {lens: dict(result=0, selected=1)})) for lens in (1, 2)}
        self.assertEqual(set(pair.gray_curve('raw')), {1, 2})
        self.assertEqual(sent, [(1, 'raw'), (2, 'raw')])
