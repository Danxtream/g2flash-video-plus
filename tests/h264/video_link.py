"""Bounded record windows with exact per-lens ACK and frozen-page collection.

Uses a supplied, already-authenticated link; no connection or install operation.
SPDX-License-Identifier: GPL-3.0-only
"""
import queue
import struct
import time

from g2flash import CTRL, crc16
from receive_client import ReceiveClient, parse_page
from send_message_probe import make_packet, parse_acks

ACK_WINDOW = 4  # Current success plus the three explicit history entries.


def sources(targets):
    """Return only the lenses explicitly selected by the transport bit mask."""
    if targets not in (1, 2, 3):
        raise ValueError('invalid target lenses')
    return tuple(lens for lens in (1, 2) if targets & lens)


class VideoLink(ReceiveClient):
    """One serialized writer; notifications arrive asynchronously on the link.

    Every window is fenced before a query changes the transport context. A
    paired success requires each lens's exact ACK, including forwarded returns.
    """
    def __init__(self, transport, lens, counter, mtu=23, max_write=20, timeout=10,
                 *, ingress=None):
        sources(lens)
        ingress = lens if lens != 3 else ingress
        if ingress not in (1, 2) or not lens & ingress:
            raise ValueError('paired delivery requires an explicit ingress lens')
        super().__init__(transport, ingress, counter, mtu, max_write, timeout)
        self.targets = lens
        self.metrics = dict(packets=0, records=0, payload_bytes=0, retries=0,
                            ack_wait_seconds=0, active_write_seconds=0, queries=[])
        self.last_results = {}

    def _receive_pending(self, pending, page, targets, timeout):
        characteristic, frame = self.transport.notes.get(timeout=timeout)
        if characteristic.lower() != CTRL[2]:
            return
        parsed = parse_page(frame)
        if parsed and page is not None and parsed[0] in sources(targets) and parsed[1:3] == page:
            key, value = parsed[:3], parsed[3:]
            if key in self.pages and self.pages[key] != value:
                raise ValueError('conflicting frozen reply page')
            self.pages[key] = value
        entries = parse_acks(frame)
        if not entries:
            return
        if frame[8] == 3:
            current = entries[0]
            for expected in pending:
                if expected[:3] == current[:3]:
                    pending[expected] = False
        else:
            for entry in entries:
                if entry in pending and pending[entry] is None:
                    pending[entry] = True

    def send_window(self, payloads, *, targets=None, page=None):
        """Write up to four records before waiting; reserve credits at the caller.

        Retries preserve payload bytes/NAL sequences. A refusal ends the window;
        missing notifications get at most two exact retries, never new grants.
        """
        targets = self.targets if targets is None else targets
        if targets & self.targets != targets:
            raise ValueError('window selects an unowned recipient')
        lenses = sources(targets)
        payloads = tuple(bytes(p) for p in payloads)
        if not 1 <= len(payloads) <= ACK_WINDOW or any(not p or len(p) > 4096 for p in payloads):
            raise ValueError('record window exceeds transport or input bounds')
        capacity = min(252, self.mtu - 14, self.max_write - 11)
        if capacity < 1:
            raise ValueError('write limit cannot hold a stream byte')
        for attempt in range(3):
            sequence = self.sequence
            pending, wire = {}, bytearray()
            for ordinal, payload in enumerate(payloads):
                checksum = int.from_bytes(crc16(payload), 'little')
                for lens in lenses:
                    pending[(sequence, ordinal, lens, len(payload), checksum)] = None
                wire += bytes([targets | (8 if not ordinal else 0)])
                wire += struct.pack('<HH', len(payload), checksum) + payload
            packets = [make_packet(wire[offset:offset + capacity], (sequence + index) & 255,
                                  targets, reset=offset == 0, end=offset + capacity >= len(wire))
                       for index, offset in enumerate(range(0, len(wire), capacity))]
            self.sequence = (sequence + len(packets) + 17) & 255
            started = time.monotonic()
            for packet in packets:
                self.transport.write(CTRL[0], CTRL[1], packet.hex(), 1)
                self.metrics['packets'] += 1
                # Notifications are queued concurrently by the existing link.
                # Drain without waiting while continuing the serialized writer.
                while not self.transport.notes.empty():
                    try:
                        self._receive_pending(pending, page, targets, 0)
                    except queue.Empty:
                        break
            self.metrics['records'] += len(payloads)
            self.metrics['payload_bytes'] += sum(map(len, payloads))
            self.metrics['active_write_seconds'] += time.monotonic() - started
            deadline = time.monotonic() + self.timeout
            wait_started = time.monotonic()
            try:
                while (any(value is None for value in pending.values()) or
                       (page is not None and any((lens, *page) not in self.pages for lens in lenses))):
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise TimeoutError('missing exact lens ACK or frozen reply page')
                    self._receive_pending(pending, page, targets, remaining)
            except queue.Empty as error:
                if attempt == 2:
                    raise TimeoutError('paired record notification deadline expired') from error
            except TimeoutError:
                if attempt == 2:
                    raise
            else:
                result = [{lens: pending[(sequence, ordinal, lens, len(payload),
                           int.from_bytes(crc16(payload), 'little'))] for lens in lenses}
                          for ordinal, payload in enumerate(payloads)]
                self.last_results = result[-1]
                if page is not None:
                    self.metrics['queries'].append(dict(targets=targets, request=page[0],
                        page=page[1], seconds=time.monotonic() - started))
                return result
            finally:
                self.metrics['ack_wait_seconds'] += time.monotonic() - wait_started
            self.metrics['retries'] += 1
        raise AssertionError('bounded retry exhausted without a result')

    def send(self, payload, page=None, *, targets=None):
        """Fence one record against every selected lens's explicit result."""
        return all(self.send_window([payload], targets=targets, page=page)[0].values())
