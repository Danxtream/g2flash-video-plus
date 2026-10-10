"""Exact paired ACK, bounded pipeline and asymmetric progress regressions.

SPDX-License-Identifier: GPL-3.0-only
"""
import queue
import struct
import unittest

from g2flash import CTRL, crc16
from test_receive_client import Counter, frame
from test_video_client import FakePlayback, FakeClock, synthetic_reference, status_bytes
from receive_client import PAGE, nal
from video_client import (VideoClient, DirectPairClient, paired_report, credit_snapshot,
                          stream_clip, CAPABILITIES, START, STOP, STATUS, CREDITS)
from video_link import VideoLink


class WindowWire:
    """Fragment records independently of packets, as the stock transport does.

    ACK loss/reorder affects notifications, never the recipient's admission.
    Each source carries its own explicit three-success history.
    """
    def __init__(self, page_bytes=6, loss=None):
        self.notes = queue.Queue()
        self.page_bytes, self.loss = page_bytes, loss
        self.wire = bytearray()
        self.replays, self.records, self.payloads = {}, [], []
        self.history = {1: [], 2: []}
        self.states = {1: 0, 2: 0}
        self.attempts = self.ends = 0
        self.held = []

    def write(self, service, characteristic, text, kind):
        assert (service, characteristic, kind) == (CTRL[0], CTRL[1], 1)
        packet = bytes.fromhex(text)
        body = packet[8:-2]
        assert crc16(body) == packet[-2:]
        if body[0] & 128:
            assert not self.wire
            self.sequence, self.ordinal = packet[2], 0
            self.attempts += 1
        self.wire.extend(body[1:])
        while len(self.wire) >= 5:
            size = struct.unpack_from('<H', self.wire, 1)[0]
            if len(self.wire) < 5 + size:
                break
            flags, payload = self.wire[0], bytes(self.wire[5:5+size])
            assert crc16(payload) == self.wire[3:5]
            del self.wire[:5+size]
            self.payloads.append(payload)
            for lens in (1, 2):
                if not flags & lens:
                    continue
                accepted = not (self.loss == 'nack' and lens == 2 and self.ordinal == 1)
                self.records.append((self.attempts, self.ordinal, lens, payload))
                if payload[0] == 31 and payload[1] != 6:
                    op, request = payload[1], struct.unpack_from('<I', payload, 4)[0]
                    key = (lens, request)
                    page = payload[8] if op == PAGE else 0
                    if op != PAGE and key not in self.replays:
                        if op == START: self.states[lens] = 2
                        if op == STOP: self.states[lens] = 0
                        if op == CREDITS:
                            self.replays[key] = struct.pack('<IBB', lens, 32+4, 0)
                        else:
                            self.replays[key] = status_bytes(self.states[lens], 1, token=10+lens)
                    raw = self.replays[key]
                    count = (len(raw)+self.page_bytes-1)//self.page_bytes
                    self.notes.put((CTRL[2], frame(struct.pack('<BBIBB', 31, lens, request, page, count)
                                   + raw[page*self.page_bytes:(page+1)*self.page_bytes])))
                checksum = int.from_bytes(crc16(payload), 'little')
                entry = (self.sequence, self.ordinal, size, checksum)
                ack = struct.pack('<BBHBHH', 1 if accepted else 3, self.sequence, self.ordinal,
                                  lens, size, checksum)
                if accepted:
                    ack += b''.join(struct.pack('<BHHH', *old) for old in reversed(self.history[lens][-3:]))
                    self.history[lens].append(entry)
                if self.loss == 'peer' and lens == 2:
                    continue
                if self.loss == 'retry' and lens == 2 and self.attempts == 1:
                    continue
                if self.loss == 'history' and self.ordinal < 3:
                    continue
                self.held.append((CTRL[2], frame(ack)))
            self.ordinal += 1
        if body[0] & 64:
            assert not self.wire
            self.ends += 1
            # Reordered and duplicate notifications must not grant extra slots.
            for item in reversed(self.held):
                self.notes.put(item); self.notes.put(item)
            self.held.clear()


class PairedPlayback:
    targets = 3
    def __init__(self, reference, *, ingress=1, fail=None):
        self.lens, self.reference, self.fail = ingress, reference, fail
        self.sides = {side: FakePlayback(reference) for side in (1, 2)}
        self.windows = []
        self.turn = 0
    def command(self, op, stream=0, *, lens=None, **kwargs):
        if lens is not None:
            return self.sides[lens].command(op, stream, **kwargs)
        result = {side: client.command(op, stream, **kwargs) for side, client in self.sides.items()}
        if op == START and self.fail == 'start':
            result[2]['result'] = 3
        if op == START and self.fail == 'lost-start':
            raise TimeoutError('peer START reply lost after local acceptance')
        return paired_report(result)
    def credits(self, stream, tokens):
        self.turn += 1
        reports = {}
        for side, client in self.sides.items():
            # LEFT consumes every poll, RIGHT only on alternating polls.
            if side == 1 or self.turn % 2:
                client.credits(stream, {1: 1})
            report = client.report()
            report.update(expected=client.consumed, capacity=6 if side == 1 else 4)
            reports[side] = report
        return paired_report(reports)
    def send_window(self, payloads):
        for side, client in self.sides.items():
            capacity = 6 if side == 1 else 4
            self.assert_bound(client, capacity, payloads)
        self.windows.append(len(payloads))
        return [{side: client.send_nal(*struct.unpack_from('<II', p, 2), p[10:])
                 for side, client in self.sides.items()} for p in payloads]
    @staticmethod
    def assert_bound(client, capacity, payloads):
        assert len(payloads) <= 4-len(client.pending)
        assert client.index + len(payloads) <= client.consumed+capacity


class VideoLinkTests(unittest.TestCase):
    def test_four_records_fragment_and_wrap_with_both_sources_and_history(self):
        for ingress in (1, 2):
            for mtu in (23, 247):
                wire = WindowWire(loss='history')
                client = VideoLink(wire, 3, Counter(), mtu, mtu-3, .002, ingress=ingress)
                client.sequence = 250
                payloads = [nal(1, n, b'\x41'+bytes([n])*700) for n in range(4)]
                self.assertEqual(client.send_window(payloads), [{1: True, 2: True}]*4)
                self.assertEqual((wire.ends, wire.attempts), (1, 1))
                self.assertEqual(client.metrics['records'], 4)

    def test_loss_retries_exact_nals_and_never_accepts_only_ingress(self):
        payloads = [nal(1, n, b'\x41') for n in range(4)]
        wire = WindowWire(loss='retry')
        client = VideoLink(wire, 3, Counter(), timeout=.001, ingress=2)
        self.assertEqual(client.send_window(payloads), [{1: True, 2: True}]*4)
        self.assertEqual(client.metrics['retries'], 1)
        self.assertEqual(wire.payloads[:4], wire.payloads[4:])
        wire = WindowWire(loss='peer')
        with self.assertRaises(TimeoutError):
            VideoLink(wire, 3, Counter(), timeout=.001, ingress=1).send_window(payloads)
        self.assertEqual(wire.attempts, 3)

    def test_peer_nack_after_local_success_is_not_repaired_by_success_history(self):
        wire = WindowWire(loss='nack')
        result = VideoLink(wire, 3, Counter(), ingress=1).send_window(
            [nal(1, n, b'\x41') for n in range(4)])
        self.assertEqual(result, [{1: True, 2: True}, {1: True, 2: False},
                                  {1: True, 2: True}, {1: True, 2: True}])

    def test_paired_frozen_pages_and_credits_keep_distinct_owner_tokens(self):
        for page_bytes in (1, 6):
            for ingress in (1, 2):
                client = VideoClient(WindowWire(page_bytes), 3, Counter(), ingress=ingress)
                start = client.command(START, 1)
                self.assertEqual(start['tokens'], {1: 11, 2: 12})
                credits = client.credits(1, start['tokens'])
                self.assertEqual(credits['expected'], 1)
                self.assertEqual(credits['by_lens'][2]['expected'], 2)
                self.assertFalse(client.pages)

    def test_asymmetric_paired_consumption_bounds_each_lens_without_status_per_nal(self):
        nals, reference = synthetic_reference(64)
        for ingress in (1, 2):
            client = PairedPlayback(reference, ingress=ingress)
            clock = FakeClock()
            result = stream_clip(client, nals, reference, verify=False, clock=clock.now, sleep=clock.sleep)
            self.assertTrue(result['valid'])
            for side in client.sides.values():
                self.assertEqual(side.sent, list(range(66)))
                self.assertEqual((side.reads, side.acks), (0, 0))
                self.assertLess(side.status_reads, 5)
            self.assertEqual(max(client.windows), 4)

    def test_partial_or_lost_paired_start_retires_both_lenses(self):
        nals, reference = synthetic_reference()
        for fail in ('start', 'lost-start'):
            client = PairedPlayback(reference, fail=fail)
            with self.assertRaises((ValueError, TimeoutError)):
                stream_clip(client, nals, reference)
            self.assertEqual([side.stops for side in client.sides.values()], [1, 1])

    def test_direct_fallback_retains_both_bounds_and_never_reuses_bridge_route(self):
        class Side(FakePlayback):
            def __init__(self, reference, side):
                super().__init__(reference); self.lens = self.targets = side
            def credits(self, stream, tokens):
                return super().credits(stream, {1: tokens[self.lens]})
            def send_window(self, payloads):
                return [{self.lens: next(iter(row.values()))} for row in super().send_window(payloads)]
        nals, reference = synthetic_reference()
        pair = DirectPairClient(Side(reference, 1), Side(reference, 2))
        clock = FakeClock()
        result = stream_clip(pair, nals, reference, verify=False, clock=clock.now, sleep=clock.sleep)
        self.assertTrue(result['valid'])
        self.assertEqual(pair.clients[1].sent, pair.clients[2].sent)

    def test_credit_parser_refuses_impossible_or_unadvertised_grants(self):
        for packed, error in ((39, 0), (36, 2), (4, 0), (80, 0)):
            with self.assertRaises(ValueError):
                credit_snapshot(struct.pack('<IBB', 0, packed, error))
        with self.assertRaises(ValueError):
            VideoLink(WindowWire(), 3, Counter())
        with self.assertRaises(ValueError):
            VideoLink(WindowWire(), 1, Counter()).send_window([b'x']*5)


if __name__ == '__main__':
    unittest.main()
