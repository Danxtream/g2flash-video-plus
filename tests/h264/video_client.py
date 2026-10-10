"""Credit-based playback and optional luma/timing verification on a supplied link.

No connection or firmware-install operation. SPDX-License-Identifier: GPL-3.0-only
"""
import math
import statistics
import struct
import time

from receive_client import (ReceiveClient, control, diagnostics, diagnostic_alerts,
                            MESSAGE, VERSION, START, STOP, STATUS, CAPABILITIES,
                            DIAGNOSTICS, PAGE, nal)

FRAME_READ, FRAME_ACK, VERIFY_FRAMES, NATIVE = 8, 9, 128, 1
STATUS_BYTES, FRAME_BYTES, ACK_BYTES = 128, 80, 16
RESULT_FIELDS = ('ordinal', 'first_nal', 'last_nal', 'geometry', 'crc', 'cycles', 'ticks',
                 'clock_before', 'clock_after', 'flags', 'decode_tick', 'copy_tick', 'calls',
                 'finishing_cycles', 'finishing_ticks', 'no_output_cycles')


def status(raw):
    """Require the active decoder/display contract and bounded independent counts."""
    raw = bytes(raw)
    if (len(raw) != STATUS_BYTES or raw[:2] != bytes((VERSION, 7)) or raw[3] > 4 or
            struct.unpack_from('<IIHHBB', raw, 32) != (4096, 4086, 320, 192, 1, 2) or
            raw[46:50] != bytes((1, 1, 4, 6)) or raw[50] not in (0, 4, 6) or raw[51] > raw[50] or
            raw[70] not in (7, 15) or raw[71] != VERIFY_FRAMES | NATIVE or raw[68] > 2 or raw[69] > 7):
        raise ValueError('unsupported or inconsistent playback contract')
    result = dict(version=raw[0], stage=raw[1], result=raw[2], state=raw[3], capacity=raw[50],
                  credits=raw[51], recovery=raw[68], headers=raw[69], compact_credits=bool(raw[70] & 8))
    for name, offset in (('stream', 4), ('token', 8), ('stream_high', 20), ('error', 24),
                         ('interval', 28), ('expected', 52), ('accepted', 56), ('consumed', 60),
                         ('pictures', 64), ('presented', 80), ('copy_failures', 84), ('copy_tick', 88),
                         ('options', 92), ('result_capacity', 96), ('result_high', 100), ('acked', 104),
                         ('result_stalls', 108), ('no_output_cycles', 112), ('no_output_ticks', 116),
                         ('no_output_calls', 120), ('incomplete', 124)):
        result[name] = struct.unpack_from('<I', raw, offset)[0]
    if (result['consumed'] > result['accepted'] or (result['state'] == 2 and result['presented'] > result['pictures']) or
            result['options'] & ~(VERIFY_FRAMES | NATIVE) or result['acked'] > result['result_high'] or
            result['result_high'] > result['presented'] or result['result_capacity'] not in (0, 16) or
            result['result_high'] - result['acked'] > result['result_capacity']):
        raise ValueError('inconsistent playback counts or verification window')
    if not result['options'] & VERIFY_FRAMES and any(result[k] for k in
            ('result_capacity', 'result_high', 'acked', 'result_stalls', 'no_output_cycles', 'no_output_ticks', 'no_output_calls')):
        raise ValueError('ordinary playback unexpectedly reports verification work')
    return result


def frame_result(raw):
    """Decode one immutable presented-picture row without accepting its timing yet."""
    raw = bytes(raw)
    if len(raw) != FRAME_BYTES or raw[:2] != bytes((VERSION, FRAME_READ)) or raw[3]:
        raise ValueError('invalid frame result header')
    stream, token, ordinal = struct.unpack_from('<III', raw, 4)
    row = dict(zip(RESULT_FIELDS, struct.unpack_from('<16I', raw, 16)))
    if not raw[2] and (not stream or not token or ordinal != row['ordinal'] or not ordinal or
                      row['geometry'] != 320 | 192 << 16 or row['flags'] & ~1 or not row['calls']):
        raise ValueError('invalid frame result identity/geometry')
    return dict(result=raw[2], stream=stream, token=token, **row)


def timing(row):
    """Flag clock anomalies, retaining both scales and never waiving a Y check."""
    before, after = row['clock_before'], row['clock_after']
    reasons = []
    if row['flags'] or not before or not after:
        reasons.append('invalid clock calibration or accumulated timing')
    if before and abs(after - before) > before / 25:
        reasons.append('clock changed over 4%')
    hz = (before + after) / 2
    cycle_ms = row['cycles'] * 1000 / hz if before and after else None
    tick_ms = row['ticks']
    if cycle_ms is not None and abs(cycle_ms - tick_ms) > max(2, cycle_ms / 25):
        reasons.append('cycles and millisecond ticks disagree')
    return dict(cycle_ms=cycle_ms, tick_ms=tick_ms, clock_before_mhz=before / 1e6,
                clock_after_mhz=after / 1e6, excluded=bool(reasons), reasons=reasons)


def summarize(rows):
    """Report valid timing samples separately for all, IDR and P pictures."""
    result = {'frames': len(rows), 'excluded': sum(r['timing']['excluded'] for r in rows)}
    for group, selected in (('all', rows), ('IDR', [r for r in rows if r['kind'] == 'IDR']),
                            ('P', [r for r in rows if r['kind'] == 'P'])):
        accepted = [r for r in selected if not r['timing']['excluded']]
        scales = {}
        for scale in ('cycle_ms', 'tick_ms'):
            values = sorted(r['timing'][scale] for r in accepted)
            scales[scale] = None if not values else dict(count=len(values), median=statistics.median(values),
                min=values[0], max=values[-1], p90=values[max(0, math.ceil(len(values) * .9) - 1)])
        result[group] = scales
    slow = [r for r in rows if r['kind'] == 'P' and not r['timing']['excluded']]
    result['slowest_P'] = max(slow, key=lambda r: r['timing']['cycle_ms']) if slow else None
    return result


class VideoClient(ReceiveClient):
    """Reuse stock framing and exact retries, with bounded frozen result pages."""
    def raw_command(self, payload, length):
        request = struct.unpack_from('<I', payload, 4)[0]
        accepted = None
        for attempt in range(3):
            try:
                accepted = self.send(payload, (request, 0))
                break
            except TimeoutError:
                if attempt == 2:
                    raise
        count, first = self.pages[(self.lens, request, 0)]
        if not first or count != (length + len(first) - 1) // len(first):
            raise ValueError('inconsistent result page count')
        raw = bytearray(first)
        for page in range(1, count):
            message = struct.pack('<BBBBIB', MESSAGE, PAGE, VERSION, 0, request, page)
            for attempt in range(3):
                try:
                    if not self.send(message, (request, page)):
                        raise ValueError('frozen result page refused')
                    break
                except TimeoutError:
                    if attempt == 2:
                        raise
            pages, content = self.pages[(self.lens, request, page)]
            if pages != count or len(content) != min(len(first), length - len(raw)):
                raise ValueError('inconsistent result page length/count')
            raw.extend(content)
        self.pages.clear()
        if len(raw) != length or bool(accepted) != (raw[2] == 0):
            raise ValueError('transport and control result disagree')
        return bytes(raw)

    def command(self, op, stream=0, *, native=False, verify=False, token=0, ordinal=0):
        request = self.counter.reserve()
        if op in (FRAME_READ, FRAME_ACK):
            payload = struct.pack('<BBBBIIII', MESSAGE, op, VERSION, 0, request, stream, token, ordinal)
            raw = self.raw_command(payload, FRAME_BYTES if op == FRAME_READ else ACK_BYTES)
            if op == FRAME_READ:
                return frame_result(raw)
            if raw[:2] != bytes((VERSION, FRAME_ACK)) or raw[3]:
                raise ValueError('invalid result ACK header')
            astream, atoken, cursor = struct.unpack_from('<III', raw, 4)
            return dict(result=raw[2], stream=astream, token=atoken, acked=cursor)
        payload = bytearray(control(op, request, stream))
        if op == START:
            payload[18] = (NATIVE if native else 0) | (VERIFY_FRAMES if verify else 0)
        raw = self.raw_command(payload, STATUS_BYTES)
        return status(raw)

    def diagnostic(self):
        request = self.counter.reserve()
        return diagnostics(self.raw_command(control(DIAGNOSTICS, request), 128))


def verify_row(row, expected, stream, token):
    """Reject every hash, count, format or owner mismatch, even anomalous timing."""
    if row['result'] or (row['stream'], row['token']) != (stream, token):
        raise ValueError('frame result refused or belongs to another session')
    for name in ('ordinal', 'first_nal', 'last_nal', 'crc'):
        if row[name] != expected[name]:
            raise ValueError(f'frame {expected["ordinal"]}: {name} mismatch')
    if row['geometry'] != expected['width'] | expected['height'] << 16 or row['calls'] != row['last_nal'] - row['first_nal'] + 1:
        raise ValueError('frame format or contributing call count mismatch')
    if row['finishing_cycles'] > row['cycles'] or row['finishing_ticks'] > row['ticks']:
        raise ValueError('inconsistent finishing-call timing')
    result = dict(row, kind=expected['kind'], timing=timing(row))
    result['copy_after_decode_ticks'] = (row['copy_tick'] - row['decode_tick']) & 0xffffffff
    return result


def stream_clip(client, nals, reference, *, verify=True, native=False, progress=lambda text: None,
                renew=lambda: None, on_ready=lambda state: None, pace_fps=0, timeout=600,
                clock=time.monotonic, sleep=time.sleep):
    """Bound credits and EOF drainage; a reconnect/error ends this entire run.

    The caller performs ordinary session/lease cleanup and diagnostics around
    each run. This helper never creates a connection or retries a P continuation
    after connection loss. Timing flags exclude samples, not correctness checks.
    """
    if (len(nals) != reference['nal_count'] or list(map(len, nals)) != reference['nal_sizes'] or
            reference['frame_count'] != len(reference['frames']) or not reference['frame_count'] or
            any(not data or len(data) > 4086 for data in nals)):
        raise ValueError('reference/NAL/count/bound mismatch')
    capabilities = client.command(CAPABILITIES)
    if capabilities['state'] != 0 or capabilities['result']:
        raise ValueError('playback requires a clean idle owner')
    stream = capabilities['stream_high'] + 1
    state = client.command(START, stream, native=native, verify=verify)
    if state['result']:
        raise ValueError(f'START refused: {state["result"]}')
    deadline = clock() + timeout
    while state['state'] != 2:
        if state['error'] or state['state'] == 4 or clock() > deadline:
            raise RuntimeError('decoder did not become READY')
        renew(); sleep(.02); state = client.command(STATUS)
    token = state['token']
    on_ready(state)
    progress(f'READY stream={stream} token={token}; verification={verify}; native={native}')
    index, rows, last_progress, started = 0, [], clock(), clock()
    finishing = {r['last_nal']: r['ordinal'] for r in reference['frames']}
    refusal_count, credit_waits = 0, 0
    while True:
        if clock() > deadline:
            raise TimeoutError('bounded upload/result drain deadline expired')
        renew()
        state = client.command(STATUS)
        if (state['result'] or state['error'] or state['state'] != 2 or
                (state['stream'], state['token']) != (stream, token) or
                state['consumed'] > index or state['pictures'] > reference['frame_count'] or
                state['presented'] > reference['frame_count']):
            raise RuntimeError('decoder/session/count failed during playback')
        if verify:
            while len(rows) < state['result_high']:
                if len(rows) >= reference['frame_count']:
                    raise ValueError('extra completed frame result')
                ordinal = len(rows) + 1
                row = client.command(FRAME_READ, stream, token=token, ordinal=ordinal)
                checked = verify_row(row, reference['frames'][len(rows)], stream, token)
                rows.append(checked)
                progress(f'frame {ordinal}/{reference["frame_count"]}: Y CRC matched; '
                         f'cycles={checked["timing"]["cycle_ms"]} ms ticks={row["ticks"]} ms '
                         f'excluded={checked["timing"]["excluded"]}')
            if len(rows) > state['acked']:
                ack = client.command(FRAME_ACK, stream, token=token, ordinal=len(rows))
                if ack['result'] or (ack['stream'], ack['token'], ack['acked']) != (stream, token, len(rows)):
                    raise ValueError('result acknowledgement mismatch')
        if index == len(nals) and state['consumed'] == index and state['presented'] == reference['frame_count']:
            if not verify or len(rows) == reference['frame_count']:
                break
        credits = state['credits']
        if not credits:
            credit_waits += 1
        sent = 0
        while credits and index < len(nals):
            if pace_fps and index in finishing:
                target = started + (finishing[index] - 1) / pace_fps
                if clock() < target:
                    break
            if not client.send_nal(stream, index, nals[index]):
                refusal_count += 1
                if refusal_count > 16:
                    raise RuntimeError('repeated input refusal; stop instead of resetting video references')
                break
            index += 1; credits -= 1; sent += 1
        if clock() - last_progress >= 2:
            progress(f'upload {index}/{len(nals)} NALs; consumed={state["consumed"]}; '
                     f'copied={state["presented"]}; verified={len(rows)}')
            last_progress = clock()
        if not sent:
            sleep(.005)
    final = dict(state)
    if verify:
        final = client.command(STATUS)
        if final['acked'] != reference['frame_count'] or final['result_high'] != reference['frame_count']:
            raise ValueError('EOF did not drain every result acknowledgement')
    stopped = client.command(STOP, stream)
    if stopped['result']:
        raise RuntimeError('STOP refused')
    while stopped['state'] != 0:
        if clock() > deadline or stopped['error'] or stopped['state'] == 4:
            raise RuntimeError('STOP did not safely reclaim ownership')
        sleep(.02); stopped = client.command(STATUS)
    if stopped['error'] or stopped['incomplete']:
        raise RuntimeError('verification or cleanup remained incomplete')
    return dict(valid=True, stream=stream, token=token, rows=rows, summary=summarize(rows) if verify else None,
                status=final, elapsed_seconds=clock() - started, credit_waits=credit_waits,
                input_refusals=refusal_count, verified=verify)
