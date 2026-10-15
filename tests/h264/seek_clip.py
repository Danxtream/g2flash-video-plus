"""Prepare bounded IDR-based seeks from decoded references, without transport.

SPDX-License-Identifier: GPL-3.0-only
"""
import math
from bisect import bisect_left


class ParameterBits:
    """Bounded RBSP prefix reader for parameter-set and slice identifiers only."""
    def __init__(self, nal):
        if not nal or len(nal) > 4086 or nal[0] & 128:
            raise ValueError('invalid bounded NAL')
        body, zeros = bytearray(), 0
        for i, byte in enumerate(nal[1:], 1):
            if zeros == 2 and byte == 3:
                if i + 1 >= len(nal) or nal[i + 1] > 3:
                    raise ValueError('invalid emulation prevention byte')
                zeros = 0
                continue
            body.append(byte)
            zeros = zeros + 1 if byte == 0 else 0
        self.body, self.position = body, 0

    def read(self, count):
        if count < 0 or self.position + count > len(self.body) * 8:
            raise ValueError('truncated parameter identifier')
        value = 0
        for _ in range(count):
            value = value * 2 + ((self.body[self.position // 8] >> (7 - self.position % 8)) & 1)
            self.position += 1
        return value

    def ue(self):
        zeros = 0
        while not self.read(1):
            zeros += 1
            if zeros > 31:
                raise ValueError('unbounded parameter identifier')
        return (1 << zeros) - 1 + self.read(zeros)


def prepare_seek(nals, reference, target_ms):
    """Select preceding IDR and its active SPS/PPS, retaining every tail NAL.

    Full picture decoding stays with the existing configured decoder. Reference
    ordinals identify completed pictures, never NAL count. Seeking to duration
    selects the final available picture. More than255 hidden pictures refuses.
    """
    frames = reference['frames']
    if not frames or len(nals) != reference['nal_count'] or list(map(len, nals)) != reference['nal_sizes']:
        raise ValueError('seek source/reference mismatch')
    if not isinstance(target_ms, (int, float)) or not math.isfinite(target_ms):
        raise ValueError('invalid seek position')
    fps = reference.get('fps')
    if fps not in (10, 15, 20):
        raise ValueError('re-convert video at 10, 15 or 20 fps')
    duration = (len(frames) * 1000 + fps // 2) // fps
    requested = max(0, min(duration, int(target_ms)))
    target = min(len(frames) - 1, bisect_left(
        range(len(frames)), requested, key=lambda i: (i * 1000 + fps // 2) // fps))
    idr = next((i for i in range(target, -1, -1) if frames[i]['kind'] == 'IDR'), None)
    if idr is None or target - idr > 255:
        raise ValueError('no bounded preceding IDR')
    start = frames[idr]['last_nal']
    if not 0 <= start < len(nals) or nals[start][0] & 31 != 5:
        raise ValueError('reference IDR is not a terminating IDR slice')
    sps, pps = {}, {}
    for index, nal in enumerate(nals[:start]):
        kind = nal[0] & 31
        if kind not in (7, 8):
            continue
        bits = ParameterBits(nal)
        if kind == 7:
            bits.read(24)
            identifier = bits.ue()
            if identifier > 31:
                raise ValueError('SPS identifier out of bounds')
            sps[identifier] = index
        else:
            identifier, parent = bits.ue(), bits.ue()
            if identifier > 255 or parent > 31:
                raise ValueError('PPS identifier out of bounds')
            pps[identifier] = (parent, index)
    bits = ParameterBits(nals[start])
    bits.ue(); bits.ue()
    selected = bits.ue()
    if selected not in pps or pps[selected][0] not in sps:
        raise ValueError('IDR has no applicable SPS/PPS')
    parent, pps_index = pps[selected]
    prefix_indices = (sps[parent], pps_index)
    # A same-ID SPS may be replaced after the PPS. Supply its active version
    # before that PPS in the new decoder, regardless of the original order.
    selected_nals = [nals[i] for i in prefix_indices] + list(nals[start:])
    offset = 2 - start
    timeline = []
    for index, row in enumerate(frames[idr:]):
        timeline.append(dict(row, ordinal=index + 1, pts_ms=((idr + index) * 1000 + fps // 2) // fps,
                             first_nal=0 if index == 0 else row['first_nal'] + offset,
                             last_nal=row['last_nal'] + offset))
    hidden = target - idr
    visible = [dict(row, ordinal=index + 1) for index, row in enumerate(timeline[hidden:])]
    result = dict(reference, nal_count=len(selected_nals), nal_sizes=list(map(len, selected_nals)),
                  frame_count=len(visible), decoded_frame_count=len(timeline), frames=visible,
                  timeline_frames=timeline, preroll=hidden, duration_ms=duration,
                  start_pts_ms=timeline[hidden]['pts_ms'], requested_pts_ms=requested)
    return selected_nals, result
