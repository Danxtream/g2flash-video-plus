"""Offline sender, exact reply and configured-decoder reference regressions.

SPDX-License-Identifier: GPL-3.0-only
"""
import copy
from pathlib import Path
import queue
import struct
import tempfile
import unittest
import zlib

from g2flash import CTRL
from test_receive_client import Counter, MockTransport, frame, diagnostic_bytes
from video_client import (VideoClient, status, frame_result, timing, summarize, stream_clip,
                          START, STOP, STATUS, CAPABILITIES, FRAME_READ, FRAME_ACK, VERIFY_FRAMES, NATIVE)
from reference_clip import read_nals, build_reference, make_reference, decoder_identity


def status_bytes(state=0, stream=0, token=1, consumed=0, pictures=0, options=0, high=0, acked=0):
    raw = bytearray(128)
    raw[:4] = bytes((1, 7, 0, state))
    struct.pack_into('<II', raw, 4, stream, token)
    struct.pack_into('<I', raw, 20, stream)
    struct.pack_into('<IIHHBB', raw, 32, 4096, 4086, 320, 192, 1, 2)
    raw[46:52] = bytes((1, 1, 4, 6, 4 if state else 0, 4 if state else 0))
    raw[70:72] = bytes((15, 129))
    struct.pack_into('<IIII', raw, 52, consumed, consumed, consumed, pictures)
    struct.pack_into('<IIII', raw, 80, pictures, 0, 100, options)
    struct.pack_into('<IIII', raw, 96, 16 if options & VERIFY_FRAMES and state else 0, high, acked, 0)
    return bytes(raw)


def result_row(ordinal=1, first=0, last=2, crc=0x12345678):
    return dict(ordinal=ordinal, first_nal=first, last_nal=last, geometry=320 | 192 << 16, crc=crc,
                cycles=12500000, ticks=50, clock_before=250000000, clock_after=250000000, flags=0,
                decode_tick=100, copy_tick=101, calls=last-first+1, finishing_cycles=12400000,
                finishing_ticks=49, no_output_cycles=100000, result=0, stream=1, token=1)


class PlaybackWire(MockTransport):
    """Keep actual framing/pagination mocks and substitute playback snapshots."""
    def __init__(self, *args, lose_once=False, lose_op=None, **kwargs):
        super().__init__(*args, **kwargs)
        self.options = self.acked = 0
        self.lose_once = lose_once
        self.lose_op = lose_op
        self.requests = []

    def write(self, *args):
        packet = bytes.fromhex(args[2]); body = packet[8:-2]
        assembled = bytes(body[1:]) if body[0] & 128 else bytes(self.wire) + body[1:]
        op = None
        if body[0] & 64:
            payload = assembled[5:]
            op = payload[1]
            if op != 5:
                request = struct.unpack_from('<I', payload, 4)[0]
                self.requests.append((request, payload))
                if request not in self.replays:
                    if op == START:
                        self.stream = struct.unpack_from('<I', payload, 8)[0]
                        self.options = payload[18]; self.state = 2
                    if op == STOP:
                        self.state = 0
                    if op in (FRAME_READ, FRAME_ACK):
                        stream, token, ordinal = struct.unpack_from('<III', payload, 8)
                        head = struct.pack('<BBBBIII', 1, op, 0, 0, stream, token, ordinal)
                        if op == FRAME_READ:
                            row = result_row(ordinal)
                            from video_client import RESULT_FIELDS
                            self.replays[request] = head + struct.pack('<16I', *(row[k] for k in RESULT_FIELDS))
                        else:
                            self.acked = ordinal; self.replays[request] = head
                    else:
                        self.replays[request] = diagnostic_bytes() if op == 7 else status_bytes(
                            self.state, self.stream, options=self.options)
        super().write(*args)
        if self.lose_once and op is not None and (self.lose_op is None or op == self.lose_op):
            self.lose_once = False
            while not self.notes.empty():
                self.notes.get_nowait()


class FakePlayback:
    """Model consumption and window credits independently of completed frames."""
    def __init__(self, reference, fail=None):
        self.reference, self.fail = reference, fail
        self.state = self.options = self.index = self.consumed = self.high = self.acked = 0
        self.pending = []; self.sent = []; self.reads = self.acks = self.stops = 0
        self.refused = False
        self.lens = self.targets = 1
        self.status_reads = self.credit_reads = 0
    def report(self):
        pictures = sum(r['last_nal'] < self.consumed for r in self.reference['frames'])
        report = status(status_bytes(self.state, 1, consumed=self.consumed, pictures=pictures,
                                    options=self.options, high=self.high if self.state else 0,
                                    acked=self.acked if self.state else 0))
        report['credits'] = 4 - len(self.pending)
        report['accepted'] = self.index
        return report
    def command(self, op, stream=0, **kwargs):
        if op == CAPABILITIES:
            result = self.report(); result['stream_high'] = 0; return result
        if op == START:
            self.state = 2; self.options = (VERIFY_FRAMES if kwargs['verify'] else 0) | (NATIVE if kwargs['native'] else 0)
        if op == STATUS:
            self.status_reads += 1
        if op == FRAME_READ:
            self.reads += 1
            expected = self.reference['frames'][kwargs['ordinal'] - 1]
            row = result_row(expected['ordinal'], expected['first_nal'], expected['last_nal'], expected['crc'])
            if self.fail in ('crc', 'token', 'stream', 'ordinal', 'first_nal', 'last_nal', 'calls', 'geometry'):
                row[self.fail] += 1
            if self.fail == 'clock':
                row['clock_after'] = 300000000
            return row
        if op == FRAME_ACK:
            self.acks += 1; self.acked = kwargs['ordinal']
            return dict(result=0, stream=1, token=2 if self.fail == 'ack' else 1, acked=self.acked)
        if op == STOP:
            self.stops += 1; self.state = 0
            if self.fail == 'cleanup':
                result = self.report(); result['error'] = 15; return result
        return self.report()
    def credits(self, stream, tokens):
        assert stream == 1 and tokens == {1: 1}
        self.credit_reads += 1
        if self.pending:
            if self.fail == 'disconnect':
                raise ConnectionError('link lost')
            if self.fail != 'deadline' and self.high - self.acked < 16:
                self.consumed = self.pending.pop(0) + 1
                if self.options & VERIFY_FRAMES:
                    self.high = sum(r['last_nal'] < self.consumed for r in self.reference['frames'])
        return dict(expected=self.consumed, credits=4-len(self.pending), capacity=4,
                    state=self.state, result=0, error=0)
    def send_window(self, payloads):
        return [{1: self.send_nal(*struct.unpack_from('<II', p, 2), p[10:])} for p in payloads]
    def send_nal(self, stream, sequence, data):
        if self.fail == 'refusal' and not self.refused:
            self.refused = True; return False
        if self.fail == 'always-refuse':
            return False
        if sequence != self.index or len(self.pending) == 4:
            return False
        self.sent.append(sequence); self.pending.append(sequence); self.index += 1; return True


class FakeClock:
    def __init__(self): self.value = 0
    def now(self): self.value += .001; return self.value
    def sleep(self, seconds): self.value += seconds


def synthetic_reference(count=32):
    nals = [b'\x67', b'\x68'] + [b'\x65' if not n else b'\x41' for n in range(count)]
    frames = [dict(ordinal=n + 1, first_nal=0 if not n else n + 2, last_nal=n + 2,
                   crc=0x12345678, width=320, height=192, kind='P' if n else 'IDR') for n in range(count)]
    return nals, dict(nal_count=len(nals), nal_sizes=list(map(len, nals)), frame_count=count, frames=frames)


class VideoClientTests(unittest.TestCase):
    def test_full_and_minimum_mtu_all_status_result_and_diagnostic_pages(self):
        for size in (1, 22):
            for lens in (1, 2):
                client = VideoClient(PlaybackWire(lens=lens, page_bytes=size), lens, Counter())
                self.assertEqual(client.command(CAPABILITIES)['stage'], 7)
                self.assertEqual(client.command(START, 1, verify=True, native=True)['options'], 129)
                self.assertEqual(client.command(FRAME_READ, 1, token=1, ordinal=1)['crc'], 0x12345678)
                self.assertEqual(client.command(FRAME_ACK, 1, token=1, ordinal=1)['acked'], 1)
                self.assertEqual(client.diagnostic()['worker']['guards_ok'], 1)

    def test_lost_reply_retries_identical_durable_request_not_new_operation(self):
        link = PlaybackWire(page_bytes=22, lose_once=True)
        client = VideoClient(link, 1, Counter(), timeout=.001)
        self.assertEqual(client.command(CAPABILITIES)['stage'], 7)
        self.assertEqual(link.requests[0], link.requests[1])

    def test_lost_page_and_ack_use_frozen_replay_without_new_acknowledgement(self):
        for operation in (5, FRAME_ACK):
            link = PlaybackWire(page_bytes=22, lose_once=True, lose_op=operation)
            client = VideoClient(link, 1, Counter(), timeout=.001)
            self.assertEqual(client.command(CAPABILITIES)['stage'], 7)
            self.assertEqual(client.command(START, 1, verify=True)['options'], VERIFY_FRAMES)
            self.assertEqual(client.command(FRAME_ACK, 1, token=1, ordinal=1)['acked'], 1)
            self.assertEqual(link.acked, 1)
            self.assertFalse(link.lose_once)

    def test_idle_retains_last_copy_count_without_a_live_decoder_queue(self):
        raw = bytearray(status_bytes())
        struct.pack_into('<I', raw, 80, 32)
        self.assertEqual(status(raw)['presented'], 32)

    def test_status_refuses_changed_format_counts_and_disabled_mode_evidence(self):
        for offset, value in ((1, 6), (46, 0), (50, 5), (70, 1), (71, 128), (100, 1), (104, 1)):
            raw = bytearray(status_bytes()); raw[offset] = value
            with self.assertRaises(ValueError): status(raw)

    def test_credit_stream_separates_nals_frames_eof_drain_without_status_per_nal(self):
        for failure in (None, 'clock'):
            nals, reference = synthetic_reference(64)
            client = FakePlayback(reference, failure); clock = FakeClock()
            result = stream_clip(client, nals, reference, clock=clock.now, sleep=clock.sleep)
            self.assertTrue(result['valid']); self.assertEqual(len(result['rows']), 64)
            self.assertEqual(client.sent, list(range(66))); self.assertEqual(client.acked, 64)
            self.assertEqual(client.stops, 1)
            self.assertEqual(result['summary']['excluded'], 64 if failure == 'clock' else 0)
            self.assertLess(client.status_reads, 5)
            self.assertGreater(client.credit_reads, 16)

    def test_off_playback_never_reads_or_acknowledges_results(self):
        nals, reference = synthetic_reference(64)
        client = FakePlayback(reference); clock = FakeClock()
        result = stream_clip(client, nals, reference, verify=False, clock=clock.now, sleep=clock.sleep)
        self.assertTrue(result['valid']); self.assertEqual((client.reads, client.acks), (0, 0))
        self.assertIsNone(result['summary'])

    def test_every_correctness_mismatch_or_connection_loss_voids_run(self):
        nals, reference = synthetic_reference()
        for failure in ('crc', 'token', 'stream', 'ordinal', 'first_nal', 'last_nal', 'calls',
                        'geometry', 'ack', 'disconnect', 'refusal', 'always-refuse', 'cleanup', 'deadline'):
            client = FakePlayback(reference, failure); clock = FakeClock()
            with self.subTest(failure=failure), self.assertRaises((ValueError, RuntimeError, TimeoutError, ConnectionError)):
                stream_clip(client, nals, reference, timeout=.3 if failure == 'deadline' else 600,
                            clock=clock.now, sleep=clock.sleep)

    def test_timing_keeps_crc_checks_but_excludes_clock_and_tick_anomalies(self):
        row = result_row()
        self.assertEqual(timing(row)['cycle_ms'], 50); self.assertFalse(timing(row)['excluded'])
        for field, value in (('clock_before', 0), ('clock_after', 300000000), ('ticks', 5), ('flags', 1)):
            bad = dict(row); bad[field] = value
            self.assertTrue(timing(bad)['excluded'])
        rows = [dict(row, kind='P', timing=timing(row)), dict(row, kind='IDR', timing=timing(row))]
        summary = summarize(rows)
        self.assertEqual(summary['P']['cycle_ms']['p90'], 50)
        self.assertEqual(summary['slowest_P']['ordinal'], 1)

    def test_annex_b_preserves_all_non_output_inputs_and_refuses_bound(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'clip.h264'
            values = [b'\x67ab', b'\x68cd', b'\x06ef', b'\x65ghi', b'\x09\x80', b'\x41j']
            path.write_bytes(b''.join(b'\0\0\0\1' + value for value in values))
            self.assertEqual(read_nals(path)[1], values)
            path.write_bytes(b'\0\0\1\x65' + b'x' * 4086)
            with self.assertRaises(ValueError): read_nals(path)


class ReferenceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(); cls.addClassCleanup(cls.directory.cleanup)
        cls.exe = build_reference(cls.directory.name)

    def test_configured_reference_crc_count_and_unsupported_fixture_refusal(self):
        # Use the production synthetic stream generator, with only its upload
        # sink replaced by an Annex-B writer. No committed video asset required.
        root = Path(__file__).resolve().parents[2]
        directory = Path(self.directory.name)
        driver = directory / 'emit.cpp'
        driver.write_text('''
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include "g2_h264.h"
extern "C" int worker_enqueue_stream();
extern "C" void worker_finish_call() {}
extern "C" g2_h264_result g2_h264_decode(void *, const uint8_t *, uint32_t) { std::abort(); }
extern "C" g2_h264_result g2_h264_frame(const void *, g2_h264_frame_info *) { std::abort(); }
extern "C" int worker_upload_nal(const uint8_t *p, uint32_t n, uint32_t) {
    const uint8_t prefix[] = {0,0,0,1};
    std::fwrite(prefix,1,4,stdout); return std::fwrite(p,1,n,stdout)==n;
}
int main() { return worker_enqueue_stream() ? 0 : 1; }
''', encoding='utf-8')
        import subprocess
        # The syntax-only generator must never enter its unqueued decode path.
        # Abort stubs enforce that without introducing a second reference.
        emit = directory / 'emit'
        subprocess.run(['clang++', '-std=c++23', '-O2', '-fsanitize=address,undefined',
                        '-fno-sanitize-recover=all', '-I' + str(root / 'patches/h264'),
                        str(driver), str(root / 'tests/h264/worker_stream.cpp'), '-o', str(emit)], check=True)
        clip = directory / 'synthetic.h264'
        clip.write_bytes(subprocess.check_output([str(emit)]))
        reference = make_reference(clip, self.exe)
        self.assertEqual((reference['nal_count'], reference['frame_count']), (34, 32))
        self.assertEqual({r['crc'] for r in reference['frames']}, {zlib.crc32(bytes([128]) * 320 * 192)})
        self.assertEqual(reference['decoder_identity'], decoder_identity()[0])
        fixture = root / 'third-party/sub0h264/tests/fixtures/flat_black_baseline_640x480.h264'
        if fixture.is_file():
            with self.assertRaises(RuntimeError): make_reference(fixture, self.exe)


if __name__ == '__main__':
    unittest.main()
