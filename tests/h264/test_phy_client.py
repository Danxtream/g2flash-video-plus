"""Offline actual-PHY evidence and radio option regressions.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct
import unittest

from phy_client import phy_report, classify_phy
from receive_client import DIAGNOSTICS
from test_receive_client import Counter, MockTransport
from test_video_client import PlaybackWire, FakePlayback, FakeClock, synthetic_reference
from video_client import VideoClient, DirectPairClient, START, RADIO_OFF, stream_clip


def phy_bytes(lens=1, original=(1, 1), current=(2, 2), after=(1, 1), phase=3):
    raw = bytearray(128)
    raw[:4] = bytes((1, 7, 0, 1))
    struct.pack_into('<I', raw, 4, 100)
    raw[8:12] = bytes((1, 32, 3, lens))
    for index in range(3):
        raw[12 + index*32] = index + 1
    raw[13:16] = bytes((31, phase, 0))
    struct.pack_into('<4I', raw, 16, 7, 90, 80, 95)
    raw[32:38] = bytes((*original, *current, *after))
    return raw


def evidence(original=(1, 1), actual=(2, 2)):
    pre = phy_report(phy_bytes(current=original))
    live = phy_report(phy_bytes(original=original, current=actual))
    post = phy_report(phy_bytes(original=original, current=original, after=original, phase=8))
    return pre, live, post


class PhyWire(MockTransport):
    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.requests = []

    def write(self, *args):
        packet = bytes.fromhex(args[2]); body = packet[8:-2]
        wire = body[1:] if body[0] & 128 else bytes(self.wire) + body[1:]
        if body[0] & 64:
            payload = wire[5:]
            if payload[1] == DIAGNOSTICS:
                self.requests.append(bytes(payload))
                request = struct.unpack_from('<I', payload, 4)[0]
                self.replays.setdefault(request, bytes(phy_bytes(lens=self.lens)))
        super().write(*args)


class PhyClientTests(unittest.TestCase):
    def test_version_bounds_reserved_fields_and_confirmed_direction_values(self):
        raw = phy_bytes(); report = phy_report(raw)
        self.assertEqual(report['links'][0]['current']['tx'], 2)
        for offset, value in ((0, 2), (8, 2), (9, 31), (10, 4), (11, 3),
                              (12, 2), (14, 10), (34, 0), (38, 3), (39, 3),
                              (116, 3), (118, 4), (119, 1), (127, 1)):
            broken = raw.copy(); broken[offset] = value
            with self.subTest(offset=offset), self.assertRaises(ValueError): phy_report(broken)
        with self.assertRaises(ValueError): phy_report(raw[:-1])
        broken = raw.copy(); broken[13] = 2
        with self.assertRaises(ValueError): phy_report(broken)
        broken = raw.copy(); struct.pack_into('<I', broken, 16, 0)
        with self.assertRaises(ValueError): phy_report(broken)

    def test_unknown_disconnected_values_remain_unknown(self):
        raw = phy_bytes(); raw[13] = 0; raw[32:38] = bytes(6)
        link = phy_report(raw)['links'][0]
        self.assertFalse(link['connected']); self.assertFalse(link['current']['valid'])

    def test_actual_1m_and_2m_and_off_already_2m_are_separate_groups(self):
        for mode, original, actual, group in (('off', (1, 1), (1, 1), '1M'),
                ('video', (1, 1), (2, 2), '2M'), ('off', (2, 2), (2, 2), '2M')):
            with self.subTest(mode=mode, actual=actual):
                result = classify_phy(*evidence(original, actual), mode)
                self.assertTrue(result['eligible']); row = result['links'][0]
                self.assertEqual(row['phy_group'], group); self.assertTrue(row['restored'])
                self.assertEqual(row['already_2m'], original == (2, 2))

    def test_epoch_loss_unknown_fallback_uncertainty_and_retained_owner_exclude(self):
        for problem in ('epoch', 'disconnected', 'unknown', 'fallback', 'uncertain',
                        'owner', 'permission', 'pending', 'pair', 'preflight', 'asymmetric'):
            before, during, after = evidence()
            pre, live, post = before['links'][0], during['links'][0], after['links'][0]
            if problem == 'epoch': post['epoch'] += 1
            if problem == 'disconnected': post['connected'] = False
            if problem == 'unknown': live['current']['valid'] = False
            if problem == 'fallback': live['phase'] = 'fallback'
            if problem == 'uncertain': post['uncertain'] = True
            if problem == 'owner': post['owners'] = 1
            if problem == 'permission': post['permission'] = 3
            if problem == 'pending': post['pending'] = True
            if problem == 'pair': post['after']['rx'] = 2
            if problem == 'preflight': pre['current']['tx'] = 2
            if problem == 'asymmetric': live['current']['rx'] = 1
            with self.subTest(problem=problem):
                result = classify_phy(before, during, after, 'video')
                self.assertFalse(result['eligible']); self.assertTrue(result['links'][0]['excluded'])

    def test_end_of_input_change_or_epoch_loss_excludes_the_comparison(self):
        before, during, after = evidence()
        self.assertTrue(classify_phy(before, during, after, 'video', ending=during)['eligible'])
        end = phy_report(phy_bytes(current=(1, 1)))
        self.assertFalse(classify_phy(before, during, after, 'video', ending=end)['eligible'])
        end['links'][0]['epoch'] += 1
        self.assertFalse(classify_phy(before, during, after, 'video', ending=end)['eligible'])

    def test_absent_host_has_no_bridge_phy_and_modes_or_lenses_cannot_be_invented(self):
        before, during, after = evidence()
        for link in during['links']: link['bound'] = False
        result = classify_phy(before, during, after, 'video')
        self.assertFalse(result['eligible']); self.assertFalse(result['links'])
        self.assertIn('UART', result['note'])
        with self.assertRaises(ValueError): classify_phy(before, during, after, 'always')
        after['lens'] = 2
        with self.assertRaises(ValueError): classify_phy(before, during, after, 'off')

    def test_read_only_selector_pagination_and_direct_pair_preserve_lens_identity(self):
        clients = {}
        for lens in (1, 2):
            wire = PhyWire(lens=lens, page_bytes=14)
            client = VideoClient(wire, lens, Counter(), timeout=.2)
            self.assertEqual(client.phy()['lens'], lens)
            self.assertFalse(client.pages)
            self.assertEqual(len(wire.requests), 1)
            self.assertEqual(wire.requests[0][:4], bytes((31, 7, 1, 0)))
            self.assertEqual(wire.requests[0][8:], struct.pack('<I', 1))
            self.assertEqual(len(wire.requests[0]), 12)
            clients[lens] = client
        report = DirectPairClient(clients[1], clients[2]).phy()['by_lens']
        self.assertEqual({lens: row['lens'] for lens, row in report.items()}, {1: 1, 2: 2})

    def test_forwarded_reports_are_source_tagged_and_cross_lens_report_is_rejected(self):
        class SourceClient(VideoClient):
            def raw_replies(self, payload, length, **kwargs):
                self.sent = bytes(payload)
                return {1: bytes(phy_bytes(lens=1)), 2: bytes(phy_bytes(lens=2))}
        client = SourceClient(PhyWire(), 3, Counter(), ingress=1)
        self.assertEqual(set(client.phy()['by_lens']), {1, 2})
        self.assertEqual(client.sent[8:], struct.pack('<I', 1))
        class WrongClient(SourceClient):
            def raw_replies(self, *args, **kwargs): return {1: bytes(phy_bytes(lens=2))}
        with self.assertRaises(ValueError): WrongClient(PhyWire(), 1, Counter()).phy()

    def test_start_radio_bits_default_and_invalid_mode_are_bounded(self):
        wire = PlaybackWire(page_bytes=14); client = VideoClient(wire, 1, Counter())
        client.command(START, 1, radio='off', native=True, verify=True)
        self.assertEqual(wire.requests[-1][1][18], 0x83)
        client.command(START, 2, radio='video')
        self.assertEqual(wire.requests[-1][1][18], 0)
        old = len(wire.requests)
        with self.assertRaises(ValueError): client.command(START, 3, radio='always')
        self.assertEqual(len(wire.requests), old)

    def test_off_requires_capability_before_start_and_video_keeps_old_contract(self):
        nals, reference = synthetic_reference(2)
        client = FakePlayback(reference); clock = FakeClock()
        with self.assertRaises(ValueError):
            stream_clip(client, nals, reference, radio='off', clock=clock.now, sleep=clock.sleep)
        self.assertFalse(client.state); self.assertFalse(client.sent)
        with self.assertRaises(ValueError): stream_clip(client, nals, reference, radio='always')
        self.assertFalse(client.sent)
        samples = []
        run = stream_clip(client, nals, reference, verify=False, radio='video',
                          on_finished=lambda row: samples.append((client.state, row['presented'])),
                          clock=clock.now, sleep=clock.sleep)
        self.assertTrue(run['valid'])
        self.assertEqual(samples, [(2, 2)])
