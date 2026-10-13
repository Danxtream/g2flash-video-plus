"""Explicit owned-Y8 negotiation and generation boundaries; no device access.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import video_client
from playback_controller import PlaybackController
from test_video_client import FakePlayback, synthetic_reference, status_bytes


class DitherTests(unittest.TestCase):
    def test_capability_flag_is_explicit_and_old_images_remain_off_only(self):
        client = video_client.VideoClient.__new__(video_client.VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 1)
        for flags in (1, 3):
            raw = bytes((1, 13, 0, 255, 2, 16, 255, flags)) + struct.pack('<II', 77824, 8192)
            client.raw_replies = lambda *args, **kwargs: {1: raw, 2: raw}
            self.assertEqual(client.playback_capabilities()[2]['dither'], flags == 3)

    def test_start_defaults_off_and_on_requires_owned_queue_before_sending(self):
        client = video_client.VideoClient.__new__(video_client.VideoClient)
        client.counter = SimpleNamespace(reserve=lambda: 1)
        sent = []
        client.raw_replies = lambda payload, length: (sent.append(payload) or {1: b''})
        with patch.object(video_client, 'status', return_value={}), \
                patch.object(video_client, 'paired_report', side_effect=lambda reports: reports):
            client.command(video_client.START, 1, queued=True)
            self.assertEqual(sent[-1][18], video_client.QUEUED)
            client.command(video_client.START, 2, queued=True, dither=True)
            self.assertEqual(sent[-1][18], video_client.QUEUED | video_client.DITHER)
            with self.assertRaises(ValueError): client.command(video_client.START, 3, dither=True)
            self.assertEqual(len(sent), 2)

    def test_unadvertised_peer_refuses_dither_before_timeline_or_start(self):
        nals, reference = synthetic_reference(1)
        client = FakePlayback(reference)
        client.playback_capabilities = lambda: {1: dict(dither=True), 2: dict(dither=False)}
        client.configure_timeline = lambda *args: self.fail('unadvertised mode configured')
        with self.assertRaisesRegex(ValueError, 'every recipient'):
            video_client.stream_clip(client, nals, reference, queued=True, dither=True)
        self.assertEqual(client.state, 0)

    def test_status_advertises_dither_without_accepting_unqueued_on_mode(self):
        raw = bytearray(status_bytes(options=video_client.QUEUED | video_client.DITHER))
        raw[71] = 191
        self.assertEqual(video_client.status(raw)['options'], video_client.QUEUED | video_client.DITHER)
        struct.pack_into('<I', raw, 92, video_client.DITHER)
        with self.assertRaises(ValueError): video_client.status(raw)

    def test_menu_seek_keeps_dither_in_the_next_generation(self):
        client = SimpleNamespace(lens=1, playback_capabilities=lambda: {1: dict(menu=True)})
        controller = PlaybackController(client, [b'x'], {})
        with patch('playback_controller.stream_clip', side_effect=video_client.VideoSeekRequested(1000, True)), \
                patch('playback_controller.seek_clip', return_value={'valid': True}) as seek:
            self.assertTrue(controller.run(dither=True)['valid'])
        self.assertTrue(seek.call_args.kwargs['dither'])
        self.assertTrue(seek.call_args.kwargs['paused'])
