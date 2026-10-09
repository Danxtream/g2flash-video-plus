"""Exercise receive-only video controls through a supplied transport.

This test helper has no connection or firmware-install operation.
SPDX-License-Identifier: GPL-3.0-only
"""
import json
import os
from pathlib import Path
import queue
import secrets
import struct
import time

from g2flash import CTRL, crc16
from send_message_probe import make_packet, parse_acks

MESSAGE, VERSION = 31, 1
CAPABILITIES, START, STOP, RESET, STATUS, PAGE, NAL = range(7)
STATUS_BYTES, REPLY_HEADER_BYTES, RECORD_BYTES = 80, 8, 4096


def control(op, request, stream=0):
    """Encode a bounded request; START advertises receive-only fixed geometry."""
    if op not in range(5) or not 0 < request < 0xffffffff:
        raise ValueError('invalid control operation or exhausted request id')
    head = struct.pack('<BBBBI', MESSAGE, op, VERSION, 0, request)
    if op == START:
        if not 0 < stream <= 0xffffffff:
            raise ValueError('invalid stream id')
        return head + struct.pack('<IHHBBBBI', stream, 320, 192, 1, 2, 0, 0, 63)
    if op in (STOP, RESET):
        return head + struct.pack('<I', stream)
    return head


def nal(stream, sequence, data):
    """Sequence a NAL, independently of pictures and transport record ordinals."""
    if not 0 < stream <= 0xffffffff or not 0 <= sequence < 0xffffffff:
        raise ValueError('invalid NAL stream or exhausted sequence')
    data = bytes(data)
    if not data or len(data) > RECORD_BYTES - 10:
        raise ValueError('NAL exceeds the negotiated record limit')
    return struct.pack('<BBII', MESSAGE, NAL, stream, sequence) + data


def packets(payload, sequence, lens, mtu=23, max_write=20):
    """Use upstream reset/context-reset and CRC framing without compression."""
    if lens not in (1, 2) or not 23 <= mtu <= 517:
        raise ValueError('select one lens and a valid MTU')
    payload = bytes(payload)
    if not payload or len(payload) > RECORD_BYTES:
        raise ValueError('invalid bounded record')
    capacity = min(252, mtu - 14, max_write - 11)
    if capacity < 1:
        raise ValueError('write limit cannot hold one stream byte')
    wire = bytes([lens | 8]) + struct.pack('<H', len(payload)) + crc16(payload) + payload
    return [make_packet(wire[i:i + capacity], (sequence + j) & 255, lens,
                        reset=i == 0, end=i + capacity >= len(wire))
            for j, i in enumerate(range(0, len(wire), capacity))]


def parse_page(frame):
    """Return a CRC-checked source/request/page tuple, or ignore other services."""
    frame = bytes(frame)
    if len(frame) < 10 or frame[:2] != b'\xaa\x12' or frame[4:8] != b'\x01\x01\xf0\0':
        return None
    body = frame[8:-2]
    if not body or body[0] != MESSAGE:
        return None
    if (frame[3] != len(frame) - 8 or not 9 <= len(body) <= 30 or
            crc16(body) != frame[-2:] or body[1] not in (1, 2) or
            not 0 < body[7] <= 128 or body[6] >= body[7]):
        raise ValueError('invalid video reply envelope')
    return body[1], struct.unpack_from('<I', body, 2)[0], body[6], body[7], body[8:]


def snapshot(raw):
    """Validate receive-only advertised bounds before selecting any stream."""
    raw = bytes(raw)
    if (len(raw) != STATUS_BYTES or raw[0] != VERSION or raw[1] != 4 or
            raw[46:50] != bytes([1, 1, 4, 6]) or
            struct.unpack_from('<II', raw, 32) != (4096, 4086) or
            struct.unpack_from('<HHBB', raw, 40) != (320, 192, 1, 2) or
            raw[50] not in (0, 4, 6) or raw[51] > raw[50] or raw[68] > 2 or
            raw[69] > 7 or raw[70:72] != b'\0\0' or
            struct.unpack_from('<II', raw, 60) != (0, 0)):
        raise ValueError('unsupported or inconsistent receive-only capabilities')
    fields = {'version': raw[0], 'stage': raw[1], 'result': raw[2], 'state': raw[3],
              'capacity': raw[50], 'credits': raw[51], 'recovery': raw[68], 'headers': raw[69]}
    for key, offset in (('stream', 4), ('generation', 8), ('left_requests', 12),
                        ('right_requests', 16), ('stream_high', 20), ('error', 24),
                        ('interval', 28), ('expected', 52), ('accepted', 56),
                        ('consumed', 60), ('pictures', 64), ('gap_deadline', 72), ('missing', 76)):
        fields[key] = struct.unpack_from('<I', raw, offset)[0]
    return fields


class RequestCounter:
    """Durably reserve requests before send, including across lenses/restarts."""
    def __init__(self, path):
        self.path = Path(path)

    def reserve(self, high=0):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        lock = self.path.with_suffix(self.path.suffix + '.lock')
        descriptor = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600)
        try:
            current = json.loads(self.path.read_text())['request'] if self.path.exists() else 0
            if type(current) is not int or type(high) is not int or not 0 <= current < 0xffffffff or not 0 <= high < 0xffffffff:
                raise ValueError('invalid persisted request counter')
            value = max(current, high, int(time.time())) + 1
            if value >= 0xffffffff:
                raise ValueError('request counter exhausted; do not wrap or reset it')
            temporary = self.path.with_suffix(self.path.suffix + '.pending')
            with temporary.open('w', encoding='utf-8') as file:
                json.dump({'request': value}, file)
                file.flush()
                os.fsync(file.fileno())
            os.replace(temporary, self.path)
            return value
        finally:
            os.close(descriptor)
            lock.unlink()


class ReceiveClient:
    """One authenticated connection; every retry resets only transport state."""
    def __init__(self, transport, lens, counter, mtu=23, max_write=20, timeout=10):
        if lens not in (1, 2):
            raise ValueError('direct one-lens test required')
        self.transport, self.lens, self.counter = transport, lens, counter
        self.mtu, self.max_write, self.timeout = mtu, max_write, timeout
        self.sequence = secrets.randbelow(256)
        self.pages = {}
        self.pending_page = None

    def receive(self, timeout):
        characteristic, frame = self.transport.notes.get(timeout=timeout)
        if characteristic.lower() != CTRL[2]:
            return None
        page = parse_page(frame)
        if page and page[:3] == self.pending_page:
            key = page[:3]
            value = page[3:]
            if key in self.pages and self.pages[key] != value:
                raise ValueError('conflicting reply page')
            self.pages[key] = value
        return frame

    def send(self, payload, page=None):
        sequence = self.sequence
        self.sequence = (sequence + 17) & 255
        expected = (sequence, 0, self.lens, len(payload), int.from_bytes(crc16(payload), 'little'))
        for packet in packets(payload, sequence, self.lens, self.mtu, self.max_write):
            self.transport.write(CTRL[0], CTRL[1], packet.hex(), 1)
        deadline, accepted = time.monotonic() + self.timeout, None
        key = (self.lens, *page) if page is not None else None
        self.pending_page = key
        while accepted is None or (key is not None and key not in self.pages):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('receive command missing exact ACK or reply page')
            try:
                frame = self.receive(remaining)
            except queue.Empty as error:
                raise TimeoutError('receive command notification timed out') from error
            entries = parse_acks(frame) if frame else None
            if entries and frame[8] == 3 and any(row[:3] == expected[:3] for row in entries):
                accepted = False
            elif entries and expected in entries:
                accepted = True
        return accepted

    def command(self, op, stream=0):
        request = self.counter.reserve()
        payload = control(op, request, stream)
        accepted = self.send(payload, (request, 0))
        count, first = self.pages[(self.lens, request, 0)]
        if not first or count != (STATUS_BYTES + len(first) - 1) // len(first):
            raise ValueError('inconsistent bounded snapshot page count')
        raw = bytearray(first)
        for page in range(1, count):
            message = struct.pack('<BBBBIB', MESSAGE, PAGE, VERSION, 0, request, page)
            if not self.send(message, (request, page)):
                raise ValueError('cached snapshot page refused')
            pages, content = self.pages[(self.lens, request, page)]
            if pages != count or len(content) != min(len(first), STATUS_BYTES - len(raw)):
                raise ValueError('inconsistent snapshot page length/count')
            raw.extend(content)
        result = snapshot(raw)
        if accepted != (result['result'] == 0):
            raise ValueError('transport result disagrees with video control result')
        self.pages.clear()
        return result

    def send_nal(self, stream, sequence, data):
        return self.send(nal(stream, sequence, data))
