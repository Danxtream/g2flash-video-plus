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
                            MESSAGE, VERSION, START, STOP, STATUS, CAPABILITIES,
                            DIAGNOSTICS, PAGE, nal)

FRAME_READ, FRAME_ACK, CREDITS, VERIFY_FRAMES, NATIVE = 8, 9, 10, 128, 1
TIMELINE, FRAME_TIME = 11, 12
PLAYBACK_CAPABILITIES, QUEUED = 13, 4
PAIRED = 8
FRAME_TIME_FAST = 14
RADIO_OFF = 2
BASE_STATUS_BYTES, STATUS_BYTES, FRAME_BYTES, ACK_BYTES = 128, 192, 80, 16
RESULT_FIELDS = ('ordinal', 'first_nal', 'last_nal', 'geometry', 'crc', 'cycles', 'ticks',
                 'clock_before', 'clock_after', 'flags', 'decode_tick', 'copy_tick', 'calls',
                 'finishing_cycles', 'finishing_ticks', 'no_output_cycles')
POWER_STEPS = ('none', 'local startup refused', 'local UI not awake', 'local driver not initialized',
               'local active app is not dashboard', 'bridge send refused', 'peer acknowledgement missing',
               'peer not ready', 'peer refused', 'owner cancelled or expired', 'peer acknowledgement expired')


def status(raw):
    """Require the active decoder/display contract and bounded independent counts."""
    raw = bytes(raw)
    if (len(raw) not in (BASE_STATUS_BYTES, STATUS_BYTES) or raw[:2] != bytes((VERSION, 7)) or raw[3] > 4 or
            struct.unpack_from('<IIHHBB', raw, 32) != (4096, 4086, 320, 192, 1, 2) or
            raw[46:50] != bytes((1, 1, 4, 6)) or raw[50] not in (0, 4, 6) or raw[51] > raw[50] or
            raw[70] != 63 or raw[71] not in (VERIFY_FRAMES | NATIVE, VERIFY_FRAMES | NATIVE | RADIO_OFF,
                                           VERIFY_FRAMES | NATIVE | RADIO_OFF | QUEUED,
                                           VERIFY_FRAMES | NATIVE | RADIO_OFF | QUEUED | PAIRED) or raw[68] > 2 or raw[69] >> 3 >= len(POWER_STEPS)):
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


class VideoClient(VideoLink):
    def playback_capabilities(self):
        """Query explicit queued support without enabling playback."""
        request = self.counter.reserve()
        payload = struct.pack('<BBBBI', MESSAGE, PLAYBACK_CAPABILITIES, VERSION, 0, request)
        replies = self.raw_replies(payload, 16)
        for raw in replies.values():
            if raw[:3] != bytes((VERSION, PLAYBACK_CAPABILITIES, 0)) or raw[3] not in (1, 3, 7) or raw[4:8] != bytes((2, 16, 0, 0)):
                raise ValueError('unsupported queued playback capability')
        return {lens: dict(paired=bool(raw[3] & 2), compact_metadata=bool(raw[3] & 4), depth=raw[4], fps=raw[5], a4_bytes=struct.unpack_from('<I', raw, 8)[0],
                           stack_bytes=struct.unpack_from('<I', raw, 12)[0]) for lens, raw in replies.items()}

    def configure_timeline(self, stream, duration_ms, start_pts_ms=0):
        """Prepare optional picture metadata without enabling queued presentation."""
        if not 0 < stream <= 0xffffffff or not 0 < duration_ms <= 0xffffffff or not 0 <= start_pts_ms <= duration_ms:
            raise ValueError('invalid timeline stream, duration or position')
        request = self.counter.reserve()
        payload = struct.pack('<BBBBIIIIHH', MESSAGE, TIMELINE, VERSION, 0,
                              request, stream, duration_ms, start_pts_ms, 16, 1)
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
        if any(r['credits'] > 4 for r in reports.values()):
            raise ValueError('invalid metadata credits')
        return dict(result=max(r['result'] for r in reports.values()), by_lens=reports)

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
                message = struct.pack('<BBBBIB', MESSAGE, PAGE, VERSION, 0, request, page)
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

    def command(self, op, stream=0, *, native=False, verify=False, queued=False, paired=False, radio="video", token=0, ordinal=0, lens=None):
        if radio not in ("off", "video"):
            raise ValueError("radio mode must be off or video")
        if paired and not queued:
            raise ValueError('paired presentation requires queued playback')
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
            payload[18] = (NATIVE if native else 0) | (VERIFY_FRAMES if verify else 0) | (RADIO_OFF if radio == "off" else 0) | (QUEUED if queued else 0) | (PAIRED if paired else 0)
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

    def configure_timeline(self, stream, duration_ms, start_pts_ms=0):
        return paired_report({side: client.configure_timeline(stream, duration_ms, start_pts_ms)
                              for side, client in self.clients.items()})

    def frame_time_fast(self, stream, ordinal, nal_sequence, pts_ms):
        reports = {side: client.frame_time_fast(stream, ordinal, nal_sequence, pts_ms)['by_lens'][side]
                   for side, client in self.clients.items()}
        return dict(result=max(r['result'] for r in reports.values()), by_lens=reports)

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


def stream_clip(client, nals, reference, *, verify=True, native=False, queued=False, radio="video", progress=lambda text: None,
                renew=lambda: None, on_ready=lambda state: None, on_finished=lambda state: None, pace_fps=0, timeout=600,
                clock=time.monotonic, sleep=time.sleep):
    """Use owner-bound credit windows; a reconnect or refusal voids the run.

    Full status is read at startup, infrequently during input, and at EOF. Frame
    verification is optional and independent of receive credit acknowledgements.
    """
    if radio not in ('off', 'video'):
        raise ValueError('radio mode must be off or video')
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
    if queued:
        playback = client.playback_capabilities()
        if any(not r['compact_metadata'] or (client.targets == 3 and not r['paired']) for r in playback.values()):
            raise ValueError('every recipient must advertise queued playback and compact metadata')
        if client.configure_timeline(stream, (reference['frame_count'] * 1000 + 15) // 16)['result']:
            raise ValueError('timeline configuration refused')
    deadline = clock() + timeout
    try:
        start_options = dict(native=native, verify=verify, radio=radio)
        if queued:
            start_options['queued'] = True
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
        finishing = {r['last_nal']: r['ordinal'] for r in reference['frames']}
        while True:
            if clock() > deadline:
                raise TimeoutError('bounded upload/result drain deadline expired')
            renew()
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
                            r['consumed'] > index or r['pictures'] > reference['frame_count'] or
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
                            metadata = client.frame_time_fast(stream, ordinal, index + offset, (ordinal - 1) * 1000 // 16)
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
    except Exception:
        # A failed paired START may still own the other lens. Retire all selected
        # recipients when reachable; connection loss also has firmware deadlines.
        try:
            client.command(STOP, stream)
        except Exception:
            pass
        raise
