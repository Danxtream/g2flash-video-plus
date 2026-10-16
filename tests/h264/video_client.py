"""Credit-based playback and optional luma/timing verification on a supplied link.

No connection or firmware-install operation. SPDX-License-Identifier: GPL-3.0-only
"""
import math
import statistics
import struct
import time

from video_link import VideoLink, ACK_WINDOW, sources
from phy_client import PHY_DIAGNOSTICS, RADIO_REFUSAL_DIAGNOSTICS, phy_report, radio_refusal_report
from receive_client import (control, diagnostics, diagnostic_alerts,
                            MESSAGE, VERSION, START, STOP, RESET, STATUS, CAPABILITIES,
                            DIAGNOSTICS, PAGE, nal, page_request)

FRAME_READ, FRAME_ACK, CREDITS, VERIFY_FRAMES, NATIVE = 8, 9, 10, 128, 1
TIMELINE, FRAME_TIME = 11, 12
PLAYBACK_CAPABILITIES, QUEUED = 13, 4
PAIRED = 8
START_PAUSED = 32
DITHER = 16
FRAME_TIME_FAST = 14
SHOWN = 15
PAUSE, RESUME, PLAYBACK = 16, 17, 18
MENU, SCALE = 19, 20
RADIO_POLICY = 21
TRIGGER_TIMING = 22
MENU_ACTIONS = {'open': 0, 'close': 1, 'next': 2, 'previous': 3, 'apply': 4, 'query': 5}
RADIO_OFF = 2
BASE_STATUS_BYTES, STATUS_BYTES, FRAME_BYTES, ACK_BYTES = 128, 192, 80, 16
RESULT_FIELDS = ('ordinal', 'first_nal', 'last_nal', 'geometry', 'crc', 'cycles', 'ticks',
                 'clock_before', 'clock_after', 'flags', 'decode_tick', 'copy_tick', 'calls',
                 'finishing_cycles', 'finishing_ticks', 'no_output_cycles')
POWER_STEPS = ('none', 'local startup refused', 'local UI not awake', 'local driver not initialized',
               'local active app is not dashboard', 'bridge send refused', 'peer acknowledgement missing',
               'peer not ready', 'peer refused', 'owner cancelled or expired', 'peer acknowledgement expired')


class VideoSeekRequested(Exception):
    """Transfer still-live ownership to the serialized PC seek controller."""
    def __init__(self, target_ms, paused):
        super().__init__('menu requested a new media generation')
        self.target_ms, self.paused = target_ms, paused


def status(raw):
    """Require the active decoder/display contract and bounded independent counts."""
    raw = bytes(raw)
    if (len(raw) not in (BASE_STATUS_BYTES, STATUS_BYTES) or raw[:2] != bytes((VERSION, 7)) or raw[3] > 4 or
            struct.unpack_from('<IIHHBB', raw, 32) != (4096, 4086, 320, 192, 1, 2) or
            raw[46:50] != bytes((1, 1, 4, 6)) or raw[50] not in (0, 4, 6) or raw[51] > raw[50] or
            raw[70] != 63 or raw[71] not in (VERIFY_FRAMES | NATIVE, VERIFY_FRAMES | NATIVE | RADIO_OFF,
                                           VERIFY_FRAMES | NATIVE | RADIO_OFF | QUEUED,
                                           VERIFY_FRAMES | NATIVE | RADIO_OFF | QUEUED | PAIRED,
                                           VERIFY_FRAMES | NATIVE | RADIO_OFF | QUEUED | PAIRED | START_PAUSED,
                                           VERIFY_FRAMES | NATIVE | RADIO_OFF | QUEUED | PAIRED | START_PAUSED | DITHER) or raw[68] > 2 or raw[69] >> 3 >= len(POWER_STEPS)):
        raise ValueError('unsupported or inconsistent playback contract')
    result = dict(version=raw[0], stage=raw[1], result=raw[2], state=raw[3], capacity=raw[50],
                  credits=raw[51], recovery=raw[68], headers=raw[69] & 7, compact_credits=bool(raw[70] & 8),
                  power_step=raw[69] >> 3, power_step_name=POWER_STEPS[raw[69] >> 3], radio_supported=bool(raw[71] & RADIO_OFF))
    for name, offset in (('stream', 4), ('token', 8), ('stream_high', 20), ('error', 24),
                         ('interval', 28), ('expected', 52), ('accepted', 56), ('consumed', 60),
                         ('pictures', 64), ('presented', 80), ('copy_failures', 84), ('copy_tick', 88),
                         ('options', 92), ('result_capacity', 96), ('result_high', 100), ('acked', 104),
                         ('result_stalls', 108), ('no_output_cycles', 112), ('no_output_ticks', 116),
                         ('no_output_calls', 120), ('incomplete', 124)):
        result[name] = struct.unpack_from('<I', raw, offset)[0]
    if result['options'] & DITHER and not result['options'] & QUEUED:
        raise ValueError('dither status lacks owned queued playback')
    if (result['consumed'] > result['accepted'] or (result['state'] == 2 and result['presented'] > result['pictures']) or
            result['options'] & ~raw[71] or result['acked'] > result['result_high'] or
            result['result_high'] > result['presented'] or result['result_capacity'] not in (0, 16) or
            result['result_high'] - result['acked'] > result['result_capacity']):
        raise ValueError('inconsistent playback counts or verification window')
    if not result['options'] & VERIFY_FRAMES and any(result[k] for k in
            ('result_capacity', 'result_high', 'acked', 'result_stalls', 'no_output_cycles', 'no_output_ticks', 'no_output_calls')):
        raise ValueError('ordinary playback unexpectedly reports verification work')
    result['status_bytes'] = BASE_STATUS_BYTES + (raw[70] >> 5)*64
    result['decode_totals'] = decode_totals(raw[BASE_STATUS_BYTES:]) if len(raw) == STATUS_BYTES else None
    return result


def decode_totals(raw):
    """Decode bounded wall/cycle aggregates, without inventing percentiles."""
    if len(raw) != 64 or raw[0] != 1 or raw[1] & ~3:
        raise ValueError('unsupported decode aggregate format')
    flags, headers, pictures, cycles, ticks, maximum_cycles, maximum_ticks = struct.unpack_from('<BHIQQII', raw, 1)
    buckets = struct.unpack_from('<6I', raw, 32)
    if not flags & 2 and sum(buckets) != pictures:
        raise ValueError('inconsistent decode aggregate buckets')
    header_cycles, header_ticks = struct.unpack_from('<II', raw, 56)
    return dict(flags=flags, pictures=pictures, cycles=cycles, ticks=ticks,
                max_cycles=maximum_cycles, max_ticks=maximum_ticks, buckets=buckets,
                header_calls=headers, header_cycles=header_cycles, header_ticks=header_ticks,
                mean_ms=ticks/pictures if pictures else None,
                cycles_per_wall_tick_mhz=cycles/ticks/1000 if ticks and not flags else None,
                note='Decode brackets include preemption; bucket counts do not establish median or P90.')


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


def credit_snapshot(raw):
    """Decode only the advertised six-byte owner-bound credit format."""
    if len(raw) != 6:
        raise ValueError('invalid compact credit length')
    packed, error = raw[4:6]
    state = packed >> 4 & 7
    capacity = (6 if packed & 8 else 4) if state == 2 else 0
    credits = packed & 7
    if state > 4 or credits > capacity or error and credits:
        raise ValueError('invalid compact credit state/count')
    return dict(expected=struct.unpack_from('<I', raw)[0], credits=credits,
                capacity=capacity, state=state, result=error, error=error, gap=bool(packed & 128))


def paired_report(reports):
    """Retain each recipient's state while exposing conservative paired bounds."""
    if len(reports) == 1:
        return next(iter(reports.values()))
    values = tuple(reports.values())
    result = dict(values[0], by_lens=reports)
    for name in ('credits', 'capacity', 'expected', 'consumed', 'pictures', 'presented',
                 'result_high', 'acked', 'accepted'):
        if all(name in value for value in values):
            result[name] = min(value[name] for value in values)
    for name in ('result', 'error', 'stream_high', 'incomplete'):
        if all(name in value for value in values):
            result[name] = max(value[name] for value in values)
    states = {value['state'] for value in values}
    result['state'] = states.pop() if len(states) == 1 else 4 if 4 in states else 1
    if all('token' in value for value in values):
        result['tokens'] = {lens: value['token'] for lens, value in reports.items()}
    return result


def playback_state(raw):
    """Decode owner-bound media position, not input or decoded sequence."""
    if len(raw) != 36 or raw[0] != VERSION or raw[1] not in (PAUSE, RESUME, PLAYBACK, MENU, SCALE):
        raise ValueError('invalid playback state header')
    names = ('stream', 'token', 'paused', 'requested_pause', 'shown', 'position_ms', 'duration_ms', 'pending')
    report = dict(zip(names, struct.unpack_from('<8I', raw, 4)), result=raw[2], lens=raw[3])
    if not report['result'] and (report['lens'] not in (1, 2) or not report['stream'] or not report['token'] or
            any(report[n] > 1 for n in ('paused', 'requested_pause', 'pending')) or
            report['position_ms'] > report['duration_ms']):
        raise ValueError('invalid playback state values')
    return report


def menu_state(raw):
    """Decode the common menu view and PC-owned pending seek request."""
    if len(raw) != 48 or raw[1] not in (MENU, SCALE):
        raise ValueError('invalid menu state header')
    report = playback_state(raw[:36])
    view, serial, target = struct.unpack_from('<3I', raw, 36)
    if not report['result'] and (view & ~63 or (view & 15) > 9 or
            (view & 15 and not view & 1) or target > report['duration_ms']):
        raise ValueError('invalid menu view or seek target')
    return dict(report, menu_open=bool(view & 1), selection=(view >> 1) & 7,
                native_selected=bool(view & 16), native_active=bool(view & 32),
                seek_serial=serial, seek_target_ms=target)


def radio_policy_state(raw):
    """Read selected/latched policy without inferring the negotiated PHY."""
    if len(raw) != 24 or raw[:2] != bytes((VERSION, RADIO_POLICY)) or raw[3] not in (1, 2):
        raise ValueError('invalid radio policy reply')
    selected, active, pending, wire, reserved = struct.unpack_from('<5I', raw, 4)
    if not raw[2] and (selected > 1 or active not in (0, 1, 0xffffffff) or pending > 1 or wire > 1 or reserved or
                      bool(pending) != (active != 0xffffffff and active != selected)):
        raise ValueError('invalid selected or latched radio policy')
    return dict(result=raw[2], lens=raw[3], selected=selected, active=None if active == 0xffffffff else active,
                pending=bool(pending), wire_fed=bool(wire))

class VideoClient(VideoLink):
    def playback_capabilities(self):
        """Query explicit queued support without enabling playback."""
        request = self.counter.reserve()
        payload = struct.pack('<BBBBI', MESSAGE, PLAYBACK_CAPABILITIES, VERSION, 0, request)
        replies = self.raw_replies(payload, 16)
        for raw in replies.values():
            if (raw[:3] != bytes((VERSION, PLAYBACK_CAPABILITIES, 0)) or raw[3] not in (1, 3, 7, 15, 31, 63, 127, 255) or
                    (raw[4] & 63) != 2 or (raw[4] & 64 and not raw[4] & 128) or raw[5] != 7 or
                    (raw[3] & 32 and (raw[6] != 255 or raw[7] not in tuple(range(1, 256, 2)))) or
                    (not raw[3] & 32 and raw[6:8] != bytes((0, 0)))):
                raise ValueError('unsupported queued playback capability')
        self.shown_bytes = {lens: 72 if raw[7] & 4 else 64 for lens, raw in replies.items()}
        return {lens: dict(geometry=bool(raw[4] & 128), timed_nal=bool(raw[7] & 128), multi_slice=bool(raw[7] & 8), receive_evidence=bool(raw[7] & 32), profiling=bool(raw[7] & 16), trigger_timing=bool(raw[7] & 4), dither=bool(raw[7] & 2), policy=bool(raw[3] & 128), menu=bool(raw[3] & 64), seek=bool(raw[3] & 32), max_preroll=raw[6], controls=bool(raw[3] & 16), shown=bool(raw[3] & 8), paired=bool(raw[3] & 2), compact_metadata=bool(raw[3] & 4), depth=raw[4] & 63, rates=(10, 15, 20), rate_mask=raw[5], a4_bytes=struct.unpack_from('<I', raw, 8)[0],
                           stack_bytes=struct.unpack_from('<I', raw, 12)[0]) for lens, raw in replies.items()}

    def configure_timeline(self, stream, duration_ms, start_pts_ms=0, fps=15):
        """Prepare optional picture metadata without enabling queued presentation."""
        if fps not in (10, 15, 20) or not 0 < stream <= 0xffffffff or not 0 < duration_ms <= 0xffffffff or not 0 <= start_pts_ms <= duration_ms:
            raise ValueError('invalid timeline stream, duration or position')
        request = self.counter.reserve()
        payload = struct.pack('<BBBBIIIIHH', MESSAGE, TIMELINE, VERSION, 0,
                              request, stream, duration_ms, start_pts_ms, fps, 1)
        return paired_report({lens: status(raw) for lens, raw in
                              self.raw_replies(payload, BASE_STATUS_BYTES).items()})

    def frame_time(self, stream, ordinal, nal_sequence, pts_ms):
        """Associate a completed picture with its terminating slice, not every NAL."""
        if (not 0 < stream <= 0xffffffff or not 0 < ordinal <= 0xffffffff or
                not 0 <= nal_sequence <= 0xffffffff or not 0 <= pts_ms <= 0xffffffff):
            raise ValueError('invalid completed-picture identity')
        request = self.counter.reserve()
        payload = struct.pack('<BBBBIIIII', MESSAGE, FRAME_TIME, VERSION, 0,
                              request, stream, ordinal, nal_sequence, pts_ms)
        return paired_report({lens: status(raw) for lens, raw in
                              self.raw_replies(payload, BASE_STATUS_BYTES).items()})

    def frame_time_fast(self, stream, ordinal, nal_sequence, pts_ms):
        """Use advertised six-byte metadata replies rather than a paged STATUS."""
        if (not 0 < stream <= 0xffffffff or not 0 < ordinal <= 0xffffffff or
                not 0 <= nal_sequence <= 0xffffffff or not 0 <= pts_ms <= 0xffffffff):
            raise ValueError('invalid completed-picture identity')
        request = self.counter.reserve()
        payload = struct.pack('<BBBBIIIII', MESSAGE, FRAME_TIME_FAST, VERSION, 0,
                              request, stream, ordinal, nal_sequence, pts_ms)
        replies = self.raw_replies(payload, 6, result_offset=5)
        reports = {lens: dict(accepted=struct.unpack_from('<I', raw)[0], credits=raw[4],
                              result=raw[5]) for lens, raw in replies.items()}
        if any(r['credits'] > 6 for r in reports.values()):
            raise ValueError('invalid metadata credits')
        return dict(result=max(r['result'] for r in reports.values()), by_lens=reports)

    def shown(self, stream, token, cursor=0, *, lens=None):
        """Read one captured per-lens copy report; no playback acknowledgement."""
        target = self.targets if lens is None else lens
        if target not in (1, 2) or not self.targets & target or not stream or not token or not 0 <= cursor <= 0xffffffff:
            raise ValueError('select a valid report owner and cursor')
        request = self.counter.reserve()
        payload = struct.pack('<BBBBIIII', MESSAGE, SHOWN, VERSION, 0, request, stream, token, cursor)
        length = getattr(self, 'shown_bytes', {}).get(target, 64)
        return shown_report(self.raw_replies(payload, length, targets=target)[target])

    def trigger_timing(self, stream, tokens):
        """Read nominal wire/copy bounds and task-owned resource observations."""
        if not 0 < stream <= 0xffffffff or any(not 0 < tokens.get(side, 0) <= 0xffffffff for side in sources(self.targets)):
            raise ValueError('trigger timing requires every active owner token')
        payload = struct.pack('<BBBBIIII', MESSAGE, TRIGGER_TIMING, VERSION, 0,
                              self.counter.reserve(), stream, tokens.get(1, 0), tokens.get(2, 0))
        replies = {lens: trigger_timing_report(raw) for lens, raw in self.raw_replies(payload, 96).items()}
        if any(not r['result'] and (r['lens'] != lens or r['stream'] != stream or r['token'] != tokens[lens])
               for lens, r in replies.items()):
            raise ValueError('trigger timing owner differs from its request')
        return replies

    def playback(self, stream, tokens, operation=PLAYBACK):
        """Publish pause/resume intent or read actual position on both owners."""
        if operation not in (PAUSE, RESUME, PLAYBACK) or not 0 < stream <= 0xffffffff:
            raise ValueError('invalid playback operation or stream')
        payload = struct.pack('<BBBBIIII', MESSAGE, operation, VERSION, 0,
                              self.counter.reserve(), stream, tokens.get(1, 0), tokens.get(2, 0))
        return {lens: playback_state(raw) for lens, raw in self.raw_replies(payload, 36).items()}

    def menu(self, stream, tokens, action='query'):
        """Publish a menu action; a busy intent slot requires a fresh retry."""
        if action not in MENU_ACTIONS:
            raise ValueError('invalid menu action')
        return self._menu_control(stream, tokens, MENU, MENU_ACTIONS[action])

    def scale(self, stream, tokens, native):
        """Select native/2x for the next common picture trigger."""
        if not isinstance(native, bool):
            raise ValueError('native must be a boolean')
        return self._menu_control(stream, tokens, SCALE, 1 if native else 2)

    def _menu_control(self, stream, tokens, operation, value):
        if not 0 < stream <= 0xffffffff:
            raise ValueError('invalid menu stream')
        payload = struct.pack('<BBBBIIIII', MESSAGE, operation, VERSION, 0, self.counter.reserve(),
                              stream, tokens.get(1, 0), tokens.get(2, 0), value)
        replies = {lens: menu_state(raw) for lens, raw in self.raw_replies(payload, 48).items()}
        if any(not row['result'] and row['lens'] != lens for lens, row in replies.items()):
            raise ValueError('menu response source differs from its lens')
        return replies

    def radio_policy(self, mode=None):
        """Select Off/Video-only at next START/seek, or query without a change."""
        if mode not in (None, 'off', 'video'):
            raise ValueError('radio policy must be off or video')
        payload = struct.pack('<BBBBII', MESSAGE, RADIO_POLICY, VERSION, 0, self.counter.reserve(),
                              0xffffffff if mode is None else int(mode == 'video'))
        replies = {lens: radio_policy_state(raw) for lens, raw in self.raw_replies(payload, 24).items()}
        if any(row['lens'] != lens for lens, row in replies.items()):
            raise ValueError('radio policy source differs from its lens')
        return replies

    """Forwarded paired delivery and bounded frozen replies on a supplied link."""
    def raw_replies(self, payload, length, *, targets=None, result_offset=2):
        started = time.monotonic()
        targets = self.targets if targets is None else targets
        request = struct.unpack_from('<I', payload, 4)[0]
        self.send(payload, (request, 0), targets=targets)
        accepted = self.last_results.copy()
        replies = {}
        for lens in sources(targets):
            count, first = self.pages[(lens, request, 0)]
            if not first or count != (length + len(first) - 1) // len(first):
                raise ValueError('inconsistent frozen result page count')
            raw = bytearray(first)
            for page in range(1, count):
                message = page_request(request, page)
                if not self.send(message, (request, page), targets=lens):
                    raise ValueError('frozen result page refused')
                pages, content = self.pages[(lens, request, page)]
                if pages != count or len(content) != min(len(first), length - len(raw)):
                    raise ValueError('inconsistent result page length/count')
                raw.extend(content)
            if len(raw) != length or accepted[lens] != (raw[result_offset] == 0):
                raise ValueError('transport and control result disagree')
            replies[lens] = bytes(raw)
        self.pages.clear()
        self.metrics.setdefault('commands', []).append(dict(op=payload[1], targets=targets,
            bytes=length, seconds=time.monotonic()-started))
        return replies

    def raw_command(self, payload, length):
        """Compatibility for single-lens callers requiring one raw reply."""
        if self.targets == 3:
            raise ValueError('use source-tagged replies for paired commands')
        return next(iter(self.raw_replies(payload, length).values()))

    def command(self, op, stream=0, *, native=False, verify=False, queued=False, paired=False, paused=False, dither=False, preroll=0, radio="video", token=0, ordinal=0, lens=None, fps=15):
        if op == START and fps not in (10, 15, 20):
            raise ValueError('re-convert video at 10, 15 or 20 fps')
        if radio not in ("off", "video"):
            raise ValueError("radio mode must be off or video")
        if paired and not queued:
            raise ValueError('paired presentation requires queued playback')
        if dither and not queued:
            raise ValueError('dithering requires owned queued Y8 playback')
        if not 0 <= preroll <= 255 or ((preroll or paused) and not queued):
            raise ValueError('seek prefix and paused preview require queued playback')
        request = self.counter.reserve()
        if op in (FRAME_READ, FRAME_ACK):
            target = self.targets if lens is None else lens
            if target not in (1, 2) or not self.targets & target:
                raise ValueError('select the result owner lens')
            payload = struct.pack('<BBBBIIII', MESSAGE, op, VERSION, 0, request, stream, token, ordinal)
            raw = self.raw_replies(payload, FRAME_BYTES if op == FRAME_READ else ACK_BYTES,
                                   targets=target)[target]
            if op == FRAME_READ:
                return frame_result(raw)
            if raw[:2] != bytes((VERSION, FRAME_ACK)) or raw[3]:
                raise ValueError('invalid result ACK header')
            astream, atoken, cursor = struct.unpack_from('<III', raw, 4)
            return dict(result=raw[2], stream=astream, token=atoken, acked=cursor)
        payload = bytearray(control(op, request, stream))
        if op == START:
            payload[18] = (NATIVE if native else 0) | (VERIFY_FRAMES if verify else 0) | (RADIO_OFF if radio == "off" else 0) | (QUEUED if queued else 0) | (PAIRED if paired else 0) | (START_PAUSED if paused else 0) | (DITHER if dither else 0)
            payload[19] = preroll
            struct.pack_into('<I', payload, 20, (1000 + fps - 1) // fps)
        return paired_report({source: status(raw) for source, raw in
                              self.raw_replies(payload, STATUS_BYTES if op == STATUS else BASE_STATUS_BYTES).items()})

    def credits(self, stream, tokens):
        """Query both owners once; neither reply alone grants paired input slots."""
        request = self.counter.reserve()
        payload = struct.pack('<BBBBIIII', MESSAGE, CREDITS, VERSION, 0, request, stream,
                              tokens.get(1, 0), tokens.get(2, 0))
        reports = {lens: credit_snapshot(raw) for lens, raw in
                   self.raw_replies(payload, 6, result_offset=5).items()}
        return paired_report(reports)

    def phy(self):
        """Read frozen per-lens PHY evidence; never request a radio setting."""
        request = self.counter.reserve()
        payload = control(DIAGNOSTICS, request) + struct.pack('<I', PHY_DIAGNOSTICS)
        replies = {lens: phy_report(raw) for lens, raw in self.raw_replies(payload, 128).items()}
        if any(lens != row['lens'] for lens, row in replies.items()):
            raise ValueError('PHY report source differs from its lens identity')
        return next(iter(replies.values())) if len(replies) == 1 else dict(by_lens=replies)

    def radio_diagnostics(self):
        """Read retained submission/refusal evidence only; never refresh PHY."""
        request = self.counter.reserve()
        payload = control(DIAGNOSTICS, request) + struct.pack('<I', RADIO_REFUSAL_DIAGNOSTICS)
        replies = {lens: radio_refusal_report(raw) for lens, raw in self.raw_replies(payload, 128).items()}
        if any(lens != row['lens'] for lens, row in replies.items()):
            raise ValueError('radio refusal source differs from its lens identity')
        return next(iter(replies.values())) if len(replies) == 1 else dict(by_lens=replies)

    def diagnostic(self):
        request = self.counter.reserve()
        replies = {lens: diagnostics(raw) for lens, raw in
                   self.raw_replies(control(DIAGNOSTICS, request), 128).items()}
        return next(iter(replies.values())) if len(replies) == 1 else dict(by_lens=replies)


class DirectPairClient:
    """Explicit fallback using two independent links and the same paired bounds."""
    targets = 3
    lens = 1

    def __init__(self, left, right):
        if (left.targets, right.targets) != (1, 2):
            raise ValueError('fallback requires independent left and right clients')
        self.clients = {1: left, 2: right}

    def command(self, op, stream=0, *, lens=None, **kwargs):
        if op == START and kwargs.get('queued'):
            kwargs['paired'] = True
        if lens is not None:
            return self.clients[lens].command(op, stream, **kwargs)
        return paired_report({side: client.command(op, stream, **kwargs)
                              for side, client in self.clients.items()})

    def credits(self, stream, tokens):
        return paired_report({side: client.credits(stream, tokens)
                              for side, client in self.clients.items()})

    def playback_capabilities(self):
        return {side: client.playback_capabilities()[side] for side, client in self.clients.items()}

    def configure_timeline(self, stream, duration_ms, start_pts_ms=0, fps=15):
        return paired_report({side: client.configure_timeline(stream, duration_ms, start_pts_ms, fps)
                              for side, client in self.clients.items()})

    def frame_time_fast(self, stream, ordinal, nal_sequence, pts_ms):
        reports = {side: client.frame_time_fast(stream, ordinal, nal_sequence, pts_ms)['by_lens'][side]
                   for side, client in self.clients.items()}
        return dict(result=max(r['result'] for r in reports.values()), by_lens=reports)

    def shown(self, stream, token, cursor=0, *, lens):
        return self.clients[lens].shown(stream, token, cursor)

    def playback(self, stream, tokens, operation=PLAYBACK):
        return {side: client.playback(stream, tokens, operation)[side]
                for side, client in self.clients.items()}

    def menu(self, stream, tokens, action='query'):
        return {side: client.menu(stream, tokens, action)[side] for side, client in self.clients.items()}

    def trigger_timing(self, stream, tokens):
        return {side: client.trigger_timing(stream, tokens)[side] for side, client in self.clients.items()}

    def scale(self, stream, tokens, native):
        return {side: client.scale(stream, tokens, native)[side] for side, client in self.clients.items()}

    def radio_policy(self, mode=None):
        return {side: client.radio_policy(mode)[side] for side, client in self.clients.items()}


    def send_window(self, payloads):
        results = {side: client.send_window(payloads) for side, client in self.clients.items()}
        return [{side: results[side][index][side] for side in self.clients}
                for index in range(len(payloads))]

    def phy(self):
        """Keep independent host-link epochs and samples for both lenses."""
        return dict(by_lens={side: client.phy() for side, client in self.clients.items()})

    def radio_diagnostics(self):
        """Keep each lens's refusal and pool evidence separate."""
        return dict(by_lens={side: client.radio_diagnostics() for side, client in self.clients.items()})

    def diagnostic(self):
        return dict(by_lens={side: client.diagnostic() for side, client in self.clients.items()})

    def send(self, payload):
        """Apply explicit ordinary cleanup to each independent connection."""
        return all(row[side] for row in self.send_window([payload]) for side in row)

    @property
    def metrics(self):
        """Retain transport measurements per link rather than merging clocks."""
        return {side: client.metrics for side, client in self.clients.items()}


def shown_report(raw):
    """Validate source-tagged capture evidence, including loss/uncertainty."""
    if len(raw) not in (64, 72) or raw[:2] != bytes((VERSION, SHOWN)) or (not raw[2] and raw[3] not in (1, 2)):
        raise ValueError('invalid shown-frame header')
    names = ('stream', 'token', 'serial', 'ordinal', 'pts', 'copy_tick', 'trigger_tick', 'queued_tick',
             'flags', 'first', 'high', 'lost', 'holds', 'reply_tick', 'uncertainty_ms')
    result = dict(zip(names, struct.unpack_from('<15I', raw, 4)), result=raw[2], lens=raw[3])
    result['claim_tick'], result['submit_tick'] = struct.unpack_from('<II', raw, 64) if len(raw) == 72 else (None, None)
    if not raw[2] and (not result['stream'] or not result['token'] or not result['ordinal'] or
                      not 0 < result['first'] <= result['serial'] <= result['high'] or
                      result['lost'] != result['first'] - 1 or result['flags'] & ~63):
        raise ValueError('inconsistent shown-frame identity or ring')
    result['trigger_to_copy_ms'] = (result['copy_tick'] - result['trigger_tick']) & 0xffffffff
    result['uncertainty_ms'] = None if result['uncertainty_ms'] == 0xffffffff else result['uncertainty_ms']
    return result


def trigger_timing_report(raw):
    """Validate nominal tick intervals without asserting optical/clock qualification."""
    if len(raw) != 96 or raw[:2] != bytes((VERSION, TRIGGER_TIMING)) or (not raw[2] and raw[3] not in (1, 2)):
        raise ValueError('invalid trigger timing header')
    names = ('stream', 'token', 'ordinal', 'pts', 'round_trip_ms', 'peer_total_ms', 'peer_copy_ms',
             'local_copy_ms', 'wire_bound_ms', 'offset_lower_ms', 'offset_upper_ms', 'valid', 'samples',
             'anomalies', 'minimum_bound_ms', 'maximum_bound_ms', 'holds', 'occupancy', 'stack_used',
             'guards_ok', 'memory_used', 'memory_peak', 'memory_cap')
    result = dict(zip(names, struct.unpack_from('<23I', raw, 4)), result=raw[2], lens=raw[3])
    result['offset_lower_ms'], result['offset_upper_ms'] = struct.unpack_from('<ii', raw, 40)
    if not raw[2] and (not result['stream'] or not result['token'] or result['valid'] not in (0, 1) or
            result['guards_ok'] not in (0, 1) or result['occupancy'] > 2 or result['stack_used'] > 8192 or
            result['memory_cap'] not in (77824, 139264) or
            not result['memory_used'] <= result['memory_peak'] <= result['memory_cap']):
        raise ValueError('inconsistent trigger timing resources or owner')
    if not raw[2] and result['valid'] and (result['lens'] != 2 or not result['ordinal'] or not result['samples'] or
            result['round_trip_ms'] > 2000 or result['peer_total_ms'] > 2000 or
            result['peer_copy_ms'] > result['peer_total_ms'] or result['local_copy_ms'] > 2000 or
            not result['minimum_bound_ms'] <= result['wire_bound_ms'] <= result['maximum_bound_ms'] or
            result['offset_lower_ms'] > result['offset_upper_ms']):
        raise ValueError('inconsistent nominal trigger interval')
    result['qualified'] = False
    result['note'] = 'Nominal local tick bounds only; drift and optical onset are unqualified.'
    if result['valid'] and not result['result']:
        result['offset_estimate_ms'] = (result['offset_lower_ms'] + result['offset_upper_ms']) / 2
        result['nominal_uncertainty_ms'] = (result['offset_upper_ms'] - result['offset_lower_ms']) / 2
    else:
        result['offset_estimate_ms'] = result['nominal_uncertainty_ms'] = None
    return result


def paired_offset(left, right):
    """Compare deltas from the common trigger, never absolute lens clocks."""
    if any(r['result'] for r in (left, right)) or (left['lens'], right['lens']) != (1, 2) or any(
            left[k] != right[k] for k in ('stream', 'ordinal', 'pts')):
        raise ValueError('unmatched shown-frame pair')
    # No wire delay is measured; offset 56 now carries the reply tick.
    estimate = left['trigger_to_copy_ms'] - right['trigger_to_copy_ms']
    uncertainty = None if any(r['uncertainty_ms'] is None for r in (left, right)) else sum(
        r['uncertainty_ms'] for r in (left, right))
    return dict(offset_estimate_ms=estimate, uncertainty_ms=uncertainty,
                qualified=uncertainty is not None,
                note='Software copy captures; loaded wire delay and optical onset require hardware evidence.')


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


def presentation_rate(samples):
    """Measure copy-completion intervals, stating the count span and limitations.

    A lone endpoint cannot establish a rate. Device ticks wrap modulo 2**32;
    accept only forward spans shorter than half that range. Reply latency is
    outside this interval; this does not measure optical left/right sync.
    """
    shown = [row for row in samples if row['presented'] > 0]
    if len(shown) < 2 or shown[-1]['presented'] == shown[0]['presented']:
        return dict(fps=None, intervals=0, seconds=None, reason='fewer than two distinct copy endpoints')
    if any(b['presented'] < a['presented'] for a, b in zip(shown, shown[1:])):
        return dict(fps=None, intervals=0, seconds=None, reason='presented counter regressed')
    first, last = shown[0], shown[-1]
    ticks = (last['copy_tick'] - first['copy_tick']) & 0xffffffff
    count = last['presented'] - first['presented']
    if not 0 < ticks < 0x80000000:
        return dict(fps=None, intervals=count, seconds=None, reason='invalid copy tick span')
    return dict(fps=1000*count/ticks, intervals=count, seconds=ticks/1000,
                first_presented=first['presented'], last_presented=last['presented'],
                first_copy_tick=first['copy_tick'], last_copy_tick=last['copy_tick'],
                reason='observed copy intervals; excludes startup and reply delay')


def seek_restoration_ready(before, after):
    """Require all bound-link health and every old owner's original PHY pair."""
    pending = False
    for lens, baseline in before.items():
        current = after[lens]
        for old in baseline['links']:
            if not old['bound']:
                continue
            row = current['links'][old['connection'] - 1]
            if (not row['bound'] or not row['connected'] or row['epoch'] != old['epoch'] or
                    row['uncertain'] or row['status'] or not row['current']['valid']):
                raise RuntimeError('seek host link changed or restoration is uncertain')
            if row['pending'] or row['owners'] or row['permission'] or row['phase'] in ('restore-wait', 'restore-set', 'restore-read'):
                pending = True
                continue
            if old['owners'] or old['permission']:
                original, restored = old['before'], row['after']
                if (not original['valid'] or not restored['valid'] or not row['current']['valid'] or
                        (original['tx'], original['rx']) != (restored['tx'], restored['rx']) or
                        (original['tx'], original['rx']) != (row['current']['tx'], row['current']['rx'])):
                    raise RuntimeError('seek did not restore an old owner original PHY pair')
    return not pending


def seek_clip(client, nals, reference, target_ms, *, paused=False, renew=lambda: None,
              on_control=lambda client, stream, tokens: None, **kwargs):
    """Retire old owners, decode hidden preroll and confirm target on every lens.

    The serialized on_control callback may issue PC commands while the upload
    pump is active. Paused previews remain paused until an explicit resume;
    neither a helper thread nor an independent transport writer is created.
    """
    from seek_clip import prepare_seek
    selected, prepared = prepare_seek(nals, reference, target_ms)
    state = client.command(STATUS)
    if state['state'] != 0:
        sample = client.phy()
        before = sample.get('by_lens', {client.lens: sample})
        retired = client.command(RESET, state['stream'])
        deadline = time.monotonic() + 30
        while retired['state'] != 0:
            if retired['result'] or retired['state'] == 4 or time.monotonic() > deadline:
                raise RuntimeError('seek could not safely retire every old owner')
            renew(); time.sleep(.02); retired = client.command(STATUS)
        if retired['result'] or retired['error']:
            raise RuntimeError('seek retirement reported a safety failure')
        while True:
            sample = client.phy()
            if seek_restoration_ready(before, sample.get('by_lens', {client.lens: sample})):
                break
            if time.monotonic() > deadline:
                raise RuntimeError('seek original PHY restoration did not finish')
            renew(); time.sleep(.05)
    reached = {}
    def control_tick(active, stream, tokens):
        for lens, token in tokens.items():
            if lens in reached:
                continue
            row = active.shown(stream, token, 0, lens=lens)
            if row['result'] == 3:
                continue
            if row['result'] or row['ordinal'] != prepared['preroll'] + 1 or row['pts'] != prepared['start_pts_ms']:
                raise RuntimeError('seek target copy could not be confirmed')
            reached[lens] = row
        on_control(active, stream, tokens)
    result = stream_clip(client, selected, prepared, queued=True, paused=paused,
                         renew=renew, on_control=control_tick, **kwargs)
    if set(reached) != set(sources(client.targets)):
        raise RuntimeError('seek target did not complete on every lens')
    return dict(result, seek_target=prepared['start_pts_ms'], target_shown=reached)


def stream_clip(client, nals, reference, *, verify=True, native=False, queued=False, paused=False, dither=False, radio="video", progress=lambda text: None,
                renew=lambda: None, on_ready=lambda state: None, on_finished=lambda state: None, pace_fps=0, timeout=600,
                on_control=lambda client, stream, tokens: None, clock=time.monotonic, sleep=time.sleep):
    """Use owner-bound credit windows; a reconnect or refusal voids the run.

    Full status is read at startup, infrequently during input, and at EOF. Frame
    verification is optional and independent of receive credit acknowledgements.
    """
    if radio not in ('off', 'video'):
        raise ValueError('radio mode must be off or video')
    if dither and not queued:
        raise ValueError('dithering requires queued playback')
    fps = reference.get('fps')
    if queued and fps not in (10, 15, 20):
        raise ValueError('re-convert video at 10, 15 or 20 fps')
    if (len(nals) != reference['nal_count'] or list(map(len, nals)) != reference['nal_sizes'] or
            reference['frame_count'] != len(reference['frames']) or not reference['frame_count'] or
            any(not data or len(data) > 4086 for data in nals)):
        raise ValueError('reference/NAL/count/bound mismatch')
    def per_lens(report):
        return report.get('by_lens', {client.lens: report})
    capabilities = client.command(CAPABILITIES)
    if any(r['state'] != 0 or r['result'] or not r.get('compact_credits')
           for r in per_lens(capabilities).values()):
        raise ValueError('playback requires idle owners with compact credits')
    if radio == 'off' and any(not r.get('radio_supported') for r in per_lens(capabilities).values()):
        raise ValueError('radio Off is not advertised by every targeted lens')
    stream = capabilities['stream_high'] + 1
    preroll = reference.get('preroll', 0)
    decoded_count = reference.get('decoded_frame_count', reference['frame_count'])
    controls_available = False
    if queued:
        playback = client.playback_capabilities()
        if dither and any(not r.get('dither') for r in playback.values()):
            raise ValueError('owned Y8 dithering is not advertised by every recipient')
        controls_available = all(r.get('controls', False) for r in playback.values())
        if any(not r['compact_metadata'] or (client.targets == 3 and not r['paired']) for r in playback.values()):
            raise ValueError('every recipient must advertise queued playback and compact metadata')
        if ((preroll or paused) and any(not r.get('seek') or preroll > r['max_preroll'] for r in playback.values())):
            raise ValueError('seek/paused preview is not advertised by every recipient')
        if client.configure_timeline(stream, reference.get('duration_ms', (reference['frame_count'] * 1000 + fps // 2) // fps),
                                     reference.get('start_pts_ms', 0), fps)['result']:
            raise ValueError('timeline configuration refused')
    deadline = clock() + timeout
    try:
        start_options = dict(native=native, verify=verify, radio=radio)
        if queued:
            start_options['queued'] = True
            start_options['fps'] = fps
            if dither: start_options['dither'] = True
            if preroll or paused:
                start_options.update(preroll=preroll, paused=paused)
        state = client.command(START, stream, **start_options)
        if state['result']:
            raise ValueError(f'START refused: {state["result"]}')
        while state['state'] != 2:
            if state['error'] or any(r['state'] not in (1, 2) for r in per_lens(state).values()) or clock() > deadline:
                steps = {lens: r.get('power_step_name', 'unavailable') for lens, r in per_lens(state).items()}
                raise RuntimeError(f'all targeted decoders did not become READY; power steps: {steps}')
            renew(); sleep(.02); state = client.command(STATUS)
        tokens = {lens: report['token'] for lens, report in per_lens(state).items()}
        if set(tokens) != set(sources(client.targets)) or any(not token for token in tokens.values()):
            raise ValueError('READY did not identify every targeted owner')
        on_ready(state)
        progress(f'READY stream={stream} tokens={tokens}; verification={verify}; native={native}')
        copy_samples = {lens: [] for lens in tokens}
        rows = {lens: [] for lens in tokens}
        acknowledged = {lens: 0 for lens in tokens}
        index, started = 0, clock()
        last_status = last_progress = started
        credit_waits = 0
        input_finished = None
        eof_sampled = False
        timed_frames = reference.get('timeline_frames', reference['frames'])
        finishing = {r['last_nal']: r['ordinal'] for r in timed_frames}
        timed_by_ordinal = {r['ordinal']: r for r in timed_frames}
        while True:
            if clock() > deadline:
                raise TimeoutError('bounded upload/result drain deadline expired')
            renew()
            on_control(client, stream, tokens)
            credit = client.credits(stream, tokens)
            reports = per_lens(credit)
            if set(reports) != set(tokens) or any(r['result'] or r['error'] or r['state'] != 2 or
                    r['expected'] > index for r in reports.values()):
                raise RuntimeError('owner or credit progress failed during playback')
            # One early EOF snapshot can anchor a tail-rate interval without
            # adding STATUS polling to the upload path.
            if clock() - last_status >= 5 or index == len(nals) and (not eof_sampled or credit['expected'] == index):
                state = client.command(STATUS)
                for lens, r in per_lens(state).items():
                    if (r['result'] or r['error'] or r['state'] != 2 or
                            (r['stream'], r['token']) != (stream, tokens[lens]) or
                            r['consumed'] > index or r['pictures'] > decoded_count or
                            r['presented'] > reference['frame_count']):
                        raise RuntimeError('decoder/session/count failed during playback')
                    copy_samples[lens].append(dict(presented=r['presented'], copy_tick=r['copy_tick']))
                last_status = clock()
                eof_sampled |= index == len(nals)
            if verify:
                # Consumption and copying are separate. A future row may refuse;
                # retry with a fresh request after more work, never fabricate it.
                for lens, token in tokens.items():
                    while len(rows[lens]) < reference['frame_count']:
                        expected = reference['frames'][len(rows[lens])]
                        if expected['last_nal'] >= reports[lens]['expected']:
                            break
                        row = client.command(FRAME_READ, stream, token=token,
                                             ordinal=len(rows[lens]) + 1, lens=lens)
                        if row['result']:
                            break
                        checked = verify_row(row, expected, stream, token)
                        rows[lens].append(checked)
                        progress(f'lens {lens} frame {checked["ordinal"]}: Y CRC matched; '
                                 f'ticks={checked["ticks"]} ms excluded={checked["timing"]["excluded"]}')
                    if len(rows[lens]) > acknowledged[lens]:
                        count = len(rows[lens])
                        ack = client.command(FRAME_ACK, stream, token=token, ordinal=count, lens=lens)
                        if ack['result'] or (ack['stream'], ack['token'], ack['acked']) != (stream, token, count):
                            raise ValueError('result acknowledgement mismatch')
                        acknowledged[lens] = count
            if index == len(nals) and credit['expected'] == index:
                finished = all(r['consumed'] == index and r['presented'] == reference['frame_count']
                               for r in per_lens(state).values())
                if finished and (not verify or all(len(row) == reference['frame_count'] for row in rows.values())):
                    if queued and (paused or controls_available):
                        playback = client.playback(stream, tokens)
                        if any(r['result'] for r in playback.values()):
                            raise RuntimeError('paused EOF owner could not be checked')
                        if any(r['paused'] or r['requested_pause'] or r['pending'] for r in playback.values()):
                            sleep(.02)
                            continue
                    break
            # This is a slot count AND sequence-distance limit on each recipient.
            count = min(ACK_WINDOW, credit['credits'], len(nals) - index,
                        *(r['expected'] + r['capacity'] - index for r in reports.values()))
            if count < 0:
                raise RuntimeError('sender exceeded a recipient sequence window')
            if pace_fps:
                for offset in range(count):
                    ordinal = finishing.get(index + offset)
                    if ordinal and clock() < started + (ordinal - 1) / pace_fps:
                        count = offset
                        break
            if count:
                if queued:
                    for offset in range(count):
                        ordinal = finishing.get(index + offset)
                        if ordinal:
                            metadata = client.frame_time_fast(stream, ordinal, index + offset,
                                timed_by_ordinal[ordinal].get('pts_ms', ((ordinal - 1) * 1000 + fps // 2) // fps))
                            if metadata['result'] == 3:
                                count = offset
                                break
                            if metadata['result']:
                                raise ValueError('completed-picture metadata refused')
                if not count:
                    sleep(.01)
                    continue
                result = client.send_window([nal(stream, index + offset, nals[index + offset])
                                             for offset in range(count)])
                if (len(result) != count or any(set(row) != set(tokens) or
                        not all(row.values()) for row in result)):
                    raise RuntimeError('a targeted lens refused the input window; retire both owners')
                index += count
                if index == len(nals):
                    input_finished = clock()
            else:
                credit_waits += not credit['credits']
                sleep(.01)
            if clock() - last_progress >= 2:
                progress(f'upload {index}/{len(nals)} NALs; '
                         f'next-to-consume={ {lens: r["expected"] for lens, r in reports.items()} }')
                last_progress = clock()
        final = client.command(STATUS)
        playback_seconds = clock()-started
        if verify and any(r['acked'] != reference['frame_count'] or r['result_high'] != reference['frame_count']
                          for r in per_lens(final).values()):
            raise ValueError('EOF did not drain every recipient result')
        on_finished(final)
        stopped = client.command(STOP, stream)
        if stopped['result']:
            raise RuntimeError('STOP refused')
        while stopped['state'] != 0:
            if clock() > deadline or stopped['error'] or stopped['state'] == 4:
                raise RuntimeError('STOP did not safely reclaim every owner')
            sleep(.02); stopped = client.command(STATUS)
        if stopped['error'] or stopped['incomplete']:
            raise RuntimeError('verification or cleanup remained incomplete')
        rates = {lens: presentation_rate(
            [dict(presented=row['ordinal'], copy_tick=row['copy_tick']) for row in rows[lens]]
            if verify else copy_samples[lens] + [dict(presented=report['presented'], copy_tick=report['copy_tick'])])
            for lens, report in per_lens(final).items()}
        progress(f'Presentation rates from device copy ticks: {rates}')
        primary = rows[client.lens]
        return dict(valid=True, stream=stream, token=tokens[client.lens], tokens=tokens, rows=primary,
                    rows_by_lens=rows, summary=summarize(primary) if verify else None,
                    summaries={lens: summarize(value) for lens, value in rows.items()} if verify else {},
                    status=final, elapsed_seconds=clock() - started, credit_waits=credit_waits,
                    input_refusals=0, verified=verify, playback_seconds=playback_seconds,
                    presentation_by_lens=rates,
                    delivered_fps=min(r['fps'] for r in rates.values()) if all(r['fps'] is not None for r in rates.values()) else None,
                    host_completion_fps=reference['frame_count']/playback_seconds,
                    input_seconds=input_finished-started,
                    nal_bytes_per_second=sum(map(len, nals))/(input_finished-started))
    except VideoSeekRequested:
        # The supplied-link controller immediately owns RESET/restoration.
        raise
    except Exception:
        # A failed paired START may still own the other lens. Retire all selected
        # recipients when reachable; connection loss also has firmware deadlines.
        try:
            client.command(STOP, stream)
        except Exception:
            pass
        raise
