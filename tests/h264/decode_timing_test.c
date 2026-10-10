/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../../patches/video/decode_timing.c"

int main(void) {
    video_decode_accumulator a = {0};
    video_decode_add(&a, 300, 3, 0);
    assert(a.totals.header_calls == 1 && a.totals.header_ticks == 3 && !a.totals.pictures);
    const uint32_t ticks[] = {30, 50, 62, 80, 100, 101};
    a.vcl = 1;
    for (uint32_t i = 0; i < VIDEO_DECODE_BUCKETS; ++i) {
        video_decode_add(&a, 250000 * 10, 10, 0);
        assert(a.totals.pictures == i);
        video_decode_add(&a, 250000 * (ticks[i]-10), ticks[i]-10, 1);
        assert(a.totals.pictures == i+1 && a.totals.buckets[i] == 1);
        assert(!a.pending_cycles && !a.pending_ticks);
    }
    assert(a.totals.cycles == 250000ULL*423 && a.totals.ticks == 423);
    assert(a.totals.max_ticks == 101 && a.totals.max_cycles == 25250000);
    uint8_t bytes[VIDEO_DECODE_TOTAL_BYTES];
    video_decode_encode(&a.totals, bytes);
    assert(bytes[0] == 1 && !bytes[1] && bytes[2] == 1 && bytes[4] == 6 && bytes[32] == 1);
    /* Unsigned bracket subtraction permits a single counter/tick wrap. */
    video_decode_add(&a, 3u-(UINT32_MAX-4), 3u-(UINT32_MAX-4), 1);
    assert(a.totals.pictures == 7 && a.totals.ticks == 431 && !a.totals.flags);
    video_decode_add(&a, 3, VIDEO_DECODE_SAMPLE_LIMIT_MS, 1);
    assert(a.totals.flags == VIDEO_DECODE_LONG_SAMPLE && a.totals.pictures == 8);
    a.totals.cycles = UINT64_MAX-2; a.totals.ticks = UINT64_MAX-2;
    a.totals.pictures = UINT32_MAX; a.totals.buckets[0] = UINT32_MAX;
    video_decode_add(&a, 4, 4, 1);
    assert(a.totals.cycles == UINT64_MAX && a.totals.ticks == UINT64_MAX &&
           a.totals.pictures == UINT32_MAX && a.totals.buckets[0] == UINT32_MAX);
    a.vcl = 0; a.totals.header_calls = UINT16_MAX;
    a.totals.header_cycles = a.totals.header_ticks = UINT32_MAX-2;
    video_decode_add(&a, 4, 4, 0);
    assert(a.totals.header_calls == UINT16_MAX && a.totals.header_cycles == UINT32_MAX &&
           a.totals.header_ticks == UINT32_MAX && a.totals.flags & VIDEO_DECODE_SATURATED);
    video_decode_encode(&a.totals, bytes);
    assert(bytes[1] == 3 && bytes[2] == 255 && bytes[3] == 255);
    puts("multi-NAL frame totals, non-VCL separation, buckets, wrap and saturating evidence PASS");
}
