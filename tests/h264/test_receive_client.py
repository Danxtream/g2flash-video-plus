"""Mock protocol tests with no device connection.

SPDX-License-Identifier: GPL-3.0-only
"""
import json
from pathlib import Path
import queue
import struct
import tempfile
import unittest

from g2flash import CTRL, crc16
from receive_client import (CAPABILITIES, START, STOP, RESET, STATUS, PAGE,
                            ReceiveClient, RequestCounter, control, nal,
                            packets, parse_page, snapshot)


def frame(body):
    return bytes([0xaa, 0x12, 0, len(body) + 2, 1, 1, 0xf0, 0]) + body + crc16(body)


def status_bytes(stream=0, state=0, capacity=0, accepted=0, error=0):
    raw = bytearray(80)
    raw[:4] = bytes([1, 4, error, state])
    struct.pack_into('<I', raw, 4, stream)
    struct.pack_into('<I', raw, 20, stream)
    struct.pack_into('<IIHHBB', raw, 32, 4096, 4086, 320, 192, 1, 2)
    raw[46:52] = bytes([1, 1, 4, 6, capacity, capacity - accepted])
    struct.pack_into('<I', raw, 56, accepted)
    return bytes(raw)


class Counter:
    def __init__(self):
        self.value = 0

    def reserve(self):
        self.value += 1
        return self.value


class MockTransport:
    """Assemble production packet framing and answer frozen snapshot pages."""
    def __init__(self, lens=1, page_bytes=1, ack_first=False):
        self.notes = queue.Queue()
        self.lens, self.page_bytes, self.ack_first = lens, page_bytes, ack_first
        self.wire, self.replays, self.slots = bytearray(), {}, {}
        self.stream = self.state = self.records = 0

    def write(self, service, characteristic, text, kind):
        assert (service, characteristic, kind) == (CTRL[0], CTRL[1], 1)
        packet = bytes.fromhex(text)
        assert packet[:2] == b'\xaa\x21' and packet[6] == 0xf0
        body = packet[8:-2]
        assert crc16(body) == packet[-2:]
        if body[0] & 0x80:
            self.wire.clear()
            self.sequence = packet[2]
        self.wire.extend(body[1:])
        if not body[0] & 0x40:
            return
        assert self.wire[0] == self.lens | 8
        length = struct.unpack_from('<H', self.wire, 1)[0]
        payload = bytes(self.wire[5:])
        assert len(payload) == length and crc16(payload) == self.wire[3:5]
        self.records += 1
        replies, accepted = [], True
        if payload[0] == 31 and payload[1] != 6:
            op, request = payload[1], struct.unpack_from('<I', payload, 4)[0]
            page = payload[8] if op == PAGE else 0
            if op != PAGE and request not in self.replays:
                if op == START:
                    self.stream = struct.unpack_from('<I', payload, 8)[0]
                    self.slots.clear()
                    self.state = 2
                elif op in (STOP, RESET):
                    self.state = 0
                    self.slots.clear()
                self.replays[request] = status_bytes(self.stream, self.state,
                    4 if self.state else 0, len(self.slots))
            raw = self.replays[request]
            pages = (len(raw) + self.page_bytes - 1) // self.page_bytes
            content = raw[page * self.page_bytes:(page + 1) * self.page_bytes]
            replies.append(frame(struct.pack('<BBIBB', 31, self.lens, request, page, pages) + content))
        elif payload[0] == 31:
            stream, sequence = struct.unpack_from('<II', payload, 2)
            data = payload[10:]
            if self.state != 2 or stream != self.stream or sequence >= 4:
                accepted = False
            elif sequence in self.slots:
                accepted = self.slots[sequence] == data
            elif len(self.slots) == 4:
                accepted = False
            else:
                self.slots[sequence] = data
        ack = frame(struct.pack('<BBHBHH', 1 if accepted else 3, self.sequence, 0,
                    self.lens, len(payload), int.from_bytes(crc16(payload), 'little')))
        if self.ack_first:
            replies.insert(0, ack)
        else:
            replies.append(ack)
        for reply in replies:
            self.notes.put((CTRL[2], reply))


class ReceiveClientTests(unittest.TestCase):
    def test_page_crc_source_request_and_bounds(self):
        good = frame(struct.pack('<BBIBB', 31, 2, 77, 0, 80) + b'\x01')
        self.assertEqual(parse_page(good), (2, 77, 0, 80, b'\x01'))
        corrupt = bytearray(good)
        corrupt[-1] ^= 1
        with self.assertRaises(ValueError):
            parse_page(corrupt)
        for body in (struct.pack('<BBIBB', 31, 3, 77, 0, 80) + b'\x01',
                     struct.pack('<BBIBB', 31, 2, 77, 80, 80) + b'\x01'):
            with self.assertRaises(ValueError):
                parse_page(frame(body))

    def test_minimum_and_larger_mtu_pagination_and_ack_order(self):
        for page_bytes in (1, 22):
            for first in (False, True):
                transport = MockTransport(page_bytes=page_bytes, ack_first=first)
                client = ReceiveClient(transport, 1, Counter())
                self.assertEqual(client.command(CAPABILITIES)['state'], 0)
                self.assertEqual(client.command(START, 1)['capacity'], 4)
                self.assertEqual(client.command(STATUS)['stage'], 4)
                self.assertEqual(client.command(STOP, 1)['state'], 0)
                self.assertFalse(client.pages)

    def test_reorder_duplicate_full_refusal_reset_and_multiple_sessions(self):
        transport = MockTransport()
        client = ReceiveClient(transport, 1, Counter())
        headers = (b'\x67', b'\x68', b'\x65', b'\x41')
        for stream in (1, 2):
            self.assertEqual(client.command(START, stream)['state'], 2)
            for sequence in (2, 0, 1, 3):
                self.assertTrue(client.send_nal(stream, sequence, headers[sequence]))
            self.assertTrue(client.send_nal(stream, 0, headers[0]))
            self.assertFalse(client.send_nal(stream, 4, b'\x41'))
            report = client.command(STATUS)
            self.assertEqual((report['accepted'], report['consumed'], report['pictures'], report['credits']),
                             (4, 0, 0, 0))
            self.assertEqual(client.command(RESET, stream)['state'], 0)

    def test_nack_resets_transport_without_changing_video_sequence(self):
        transport = MockTransport()
        client = ReceiveClient(transport, 1, Counter())
        client.command(START, 1)
        self.assertFalse(client.send_nal(1, 4, b'\x41'))
        self.assertTrue(client.send_nal(1, 0, b'\x67'))
        self.assertTrue(client.send_nal(1, 0, b'\x67'))
        self.assertEqual(len(transport.slots), 1)

    def test_record_bound_and_packet_sequence_wrap(self):
        payload = nal(1, 0, b'\x67' + b'x' * 4085)
        split = packets(payload, 255, 1)
        self.assertEqual((split[0][2], split[1][2]), (255, 0))
        self.assertLessEqual(max(map(len, split)), 20)
        with self.assertRaises(ValueError):
            nal(1, 0, b'x' * 4087)
        with self.assertRaises(ValueError):
            nal(1, 0xffffffff, b'\x67')
        with self.assertRaises(ValueError):
            packets(payload, 0, 3)

    def test_snapshot_refuses_playback_claims_and_changed_contract(self):
        self.assertEqual(snapshot(status_bytes())['pictures'], 0)
        for offset, value in ((1, 5), (46, 0), (50, 5), (60, 1), (64, 1), (68, 3), (70, 1)):
            raw = bytearray(status_bytes())
            raw[offset] = value
            with self.assertRaises(ValueError):
                snapshot(raw)

    def test_request_reservation_survives_restart_and_refuses_wrap(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'requests.json'
            first = RequestCounter(path).reserve()
            second = RequestCounter(path).reserve()
            self.assertGreater(second, first)
            self.assertEqual(json.loads(path.read_text())['request'], second)
            self.assertGreater(RequestCounter(path).reserve(second + 9), second + 9)
            with self.assertRaises(ValueError):
                RequestCounter(path).reserve(0xfffffffe)
            self.assertFalse(path.with_suffix('.json.lock').exists())

    def test_exact_ack_and_source_are_required(self):
        class WrongSource(MockTransport):
            def write(self, *args):
                super().write(*args)
                while not self.notes.empty():
                    characteristic, response = self.notes.get_nowait()
                    body = bytearray(response[8:-2])
                    body[1 if body[0] == 31 else 4] = 2
                    self.notes.put((characteristic, frame(body)))
                    break
        client = ReceiveClient(WrongSource(), 1, Counter(), timeout=.01)
        with self.assertRaises(TimeoutError):
            client.command(CAPABILITIES)
