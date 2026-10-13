"""Validate owner-bound playback controls without connections.

SPDX-License-Identifier: GPL-3.0-only
"""
import copy
import struct
import unittest
from video_client import playback_state, seek_restoration_ready, PLAYBACK, VERSION


class PlaybackControlTests(unittest.TestCase):
    def test_seek_waits_for_every_old_owner_including_a_non_ingress_peer(self):
        pair = dict(valid=True, tx=1, rx=1)
        row = dict(connection=1, connected=True, bound=True, epoch=9, uncertain=False,
                   status=0, pending=False, owners=1, permission=7, phase='active',
                   before=pair, current=dict(valid=True, tx=2, rx=2), after=dict(valid=False, tx=0, rx=0))
        before = {lens: dict(links=[copy.deepcopy(row)]) for lens in (1, 2)}
        after = copy.deepcopy(before)
        for sample in after.values():
            sample['links'][0].update(owners=0, permission=0, phase='restored', current=pair, after=pair)
        self.assertTrue(seek_restoration_ready(before, after))
        after[2]['links'][0].update(phase='restore-read', pending=True)
        self.assertFalse(seek_restoration_ready(before, after))
        after[2]['links'][0].update(phase='restored', pending=False, after=dict(valid=True, tx=2, rx=2))
        with self.assertRaises(RuntimeError): seek_restoration_ready(before, after)
        after[2]['links'][0].update(after=pair, epoch=10)
        with self.assertRaises(RuntimeError): seek_restoration_ready(before, after)
        before[2]['links'][0].update(owners=0, permission=0)
        after[2]['links'][0].update(epoch=9, current=dict(valid=False, tx=0, rx=0))
        with self.assertRaises(RuntimeError): seek_restoration_ready(before, after)

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
