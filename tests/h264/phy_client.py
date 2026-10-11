"""Read-only host PHY evidence; no connection or controller command interface.

SPDX-License-Identifier: GPL-3.0-only
"""
import struct

PHY_DIAGNOSTICS = 1
RADIO_REFUSAL_DIAGNOSTICS = 2
PHY_BYTES, PHY_RECORD_BYTES, PHY_LINKS = 128, 32, 3
REFUSALS = ('none', 'argument', 'busy', 'serial-exhausted', 'link', 'epoch', 'permission',
            'pool-metadata', 'no-eligible-pool', 'eligible-pools-empty', 'pool-reserve',
            'mail-allocation-null', 'command-link', 'command-credit', 'command-allocation-null')
PHASES = ('observed', 'capture', 'select', 'active', 'fallback', 'restore-wait',
          'restore-set', 'restore-read', 'restored', 'uncertain')


def phy_report(raw):
    """Decode frozen confirmed samples, retaining invalid or missing evidence."""
    raw = bytes(raw)
    if (len(raw) != PHY_BYTES or raw[:4] != bytes((1, 7, 0, PHY_DIAGNOSTICS)) or
            raw[8:11] != bytes((1, PHY_RECORD_BYTES, PHY_LINKS)) or raw[11] not in (1, 2) or
            raw[116] > 2 or raw[117] > 2 or raw[118] > PHY_LINKS or raw[119] or any(raw[124:])):
        raise ValueError('unsupported PHY diagnostic format')
    records = []
    for index in range(PHY_LINKS):
        p = raw[12 + index*PHY_RECORD_BYTES:12 + (index+1)*PHY_RECORD_BYTES]
        identity, flags, phase, status = p[:4]
        epoch, current_tick, before_tick, after_tick = struct.unpack_from('<4I', p, 4)
        if (identity != index + 1 or phase >= len(PHASES) or p[26] > 2 or p[27] > 2 or
                flags & 2 and not flags & 1 or flags & 31 and not epoch or
                flags & (4 | 8 | 16) and not flags & 1 or bool(flags & 32) != bool(p[26]) or
                bool(flags & 64) != bool(p[27])):
            raise ValueError('inconsistent PHY connection/ownership')
        samples = {}
        for name, offset, tick, valid in (('before', 20, before_tick, flags & 8),
                                          ('current', 22, current_tick, flags & 4),
                                          ('after', 24, after_tick, flags & 16)):
            tx, rx = p[offset:offset+2]
            if valid and (tx not in (1, 2, 3) or rx not in (1, 2, 3)):
                raise ValueError('invalid confirmed PHY value')
            samples[name] = dict(tx=tx, rx=rx, tick=tick, valid=bool(valid))
        records.append(dict(connection=identity, epoch=epoch, connected=bool(flags & 1),
                            bound=bool(flags & 2), phase=PHASES[phase], status=status,
                            owners=p[26], operation=p[27], pending=bool(flags & 64),
                            uncertain=bool(flags & 128), permission=struct.unpack_from('<I', p, 28)[0],
                            **samples))
    return dict(lens=raw[11], tick=struct.unpack_from('<I', raw, 4)[0], links=records,
                serial=struct.unpack_from('<I', raw, 108)[0], completed=struct.unpack_from('<I', raw, 112)[0],
                pending=raw[116], result=raw[117], connection=raw[118],
                command_epoch=struct.unpack_from('<I', raw, 120)[0])


def radio_refusal_report(raw):
    """Decode captured gate evidence without issuing PHY or allocation commands."""
    raw = bytes(raw)
    if (len(raw) != PHY_BYTES or raw[:4] != bytes((1, 7, 0, RADIO_REFUSAL_DIAGNOSTICS)) or
            raw[8:11] != bytes((1, 32, 2)) or raw[11] not in (1, 2) or raw[12] > 1 or
            raw[13:16] != bytes((3, 1, 0)) or raw[32] > 2 or raw[33] > 2 or
            raw[34] > PHY_LINKS or raw[35] > 2 or any(raw[36:40]) or any(raw[104:])):
        raise ValueError('unsupported radio refusal format')
    counter, serial, completed, epoch = struct.unpack_from('<4I', raw, 16)
    traces = {}
    for name, offset in (('last_attempt', 40), ('last_refusal', 72)):
        p = raw[offset:offset + 32]
        attempt, tick, connection_epoch = struct.unpack_from('<3I', p)
        if not attempt:
            if any(p): raise ValueError('nonempty unobserved radio trace')
            traces[name] = None
            continue
        if (attempt > counter or p[14] not in (1, 2) or p[15] >= len(REFUSALS) or
                p[18] > 16 or p[19] or any(p[28:]) or name == 'last_refusal' and not p[15]):
            raise ValueError('inconsistent radio refusal trace')
        pools = {}
        for pool, index in (('first', 20), ('selected', 24)):
            identity, free = p[index:index + 2]
            size = struct.unpack_from('<H', p, index + 2)[0]
            if (identity != 255 and identity >= 16 or free not in (0, 1, 2, 3, 4, 255) or
                    identity == 255 and (free != 255 or size)):
                raise ValueError('inconsistent radio pool sample')
            pools[pool] = dict(index=None if identity == 255 else identity,
                               free=None if free == 255 else free, size=size,
                               capped=free == 4)
        traces[name] = dict(attempt=attempt, tick=tick, epoch=connection_epoch,
                            connection=p[12], operation=p[13], site=p[14],
                            reason=p[15], reason_name=REFUSALS[p[15]],
                            required=struct.unpack_from('<H', p, 16)[0],
                            skipped_empty=p[18], **pools)
    if not raw[12] and any(raw[16:104]):
        raise ValueError('radio trace without context')
    return dict(lens=raw[11], tick=struct.unpack_from('<I', raw, 4)[0], result=0,
                context=bool(raw[12]), reserve=raw[13], empty_only_fallback=bool(raw[14]),
                attempts=counter, serial=serial, completed=completed, command_epoch=epoch,
                pending=raw[32], command_result=raw[33], connection=raw[34], operation=raw[35],
                **traces)


def classify_phy(before, during, after, mode, *, ending=None):
    """Classify actual directions and restoration, never infer 1M from Off."""
    if mode not in ('off', 'video'):
        raise ValueError('radio mode must be off or video')
    if before['lens'] != during['lens'] or after['lens'] != during['lens']:
        raise ValueError('PHY samples belong to different lenses')
    if ending is not None and ending['lens'] != during['lens']:
        raise ValueError('PHY end sample belongs to a different lens')
    rows = []
    for current in during['links']:
        if not current['bound']:
            continue
        identity = current['connection']
        pre, post = before['links'][identity-1], after['links'][identity-1]
        reasons = []
        if any(not r['connected'] or not r['bound'] or r['epoch'] != current['epoch']
               for r in (pre, current, post)):
            reasons.append('connection disappeared or epoch changed')
        if any(r['uncertain'] for r in (pre, current, post)):
            reasons.append('uncertain PHY restoration')
        original = current['before']
        samples = (original, current['current'], post['after'])
        if not all(s['valid'] for s in samples) or not pre['current']['valid']:
            reasons.append('unconfirmed PHY sample')
        if post['owners'] or post['permission'] or post['pending']:
            reasons.append('restoration pending or retained by another owner')
        pair = lambda s: (s['tx'], s['rx'])
        if ending is not None:
            tail = ending['links'][identity-1]
            if (not tail['connected'] or not tail['bound'] or tail['epoch'] != current['epoch'] or
                    tail['uncertain'] or not tail['current']['valid'] or
                    pair(tail['current']) != pair(current['current']) or tail['phase'] == 'fallback'):
                reasons.append('PHY changed or became unconfirmed before input drained')
        restored = all(s['valid'] for s in (original, post['after'])) and pair(original) == pair(post['after'])
        if not restored:
            reasons.append('saved pre-video pair not confirmed after exit')
        if pre['current']['valid'] and original['valid'] and pair(pre['current']) != pair(original):
            reasons.append('PHY changed between preflight and START')
        actual = pair(current['current']) if current['current']['valid'] else None
        group = '1M' if actual == (1, 1) else '2M' if actual == (2, 2) else 'asymmetric/coded/unknown'
        if group not in ('1M', '2M'):
            reasons.append('not a symmetric 1M or 2M sample')
        if mode == 'video' and current['phase'] == 'fallback':
            reasons.append('2M negotiation fell back to the original pair')
        rows.append(dict(connection=identity, epoch=current['epoch'], phy_group=group,
                         mode=mode, before=original, during=current['current'], after=post['after'],
                         restored=restored, eligible=not reasons, excluded=reasons,
                         already_2m=original['valid'] and pair(original) == (2, 2)))
    return dict(lens=during['lens'], mode=mode, links=rows,
                eligible=bool(rows) and all(r['eligible'] for r in rows),
                note=None if rows else 'No EUS-bound host link; the UART bridge has no LE PHY.')
