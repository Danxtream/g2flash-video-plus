/* SPDX-License-Identifier: GPL-3.0-only */
#include "decode_timing.h"

#pragma clang section text=".text.video"

_Static_assert(sizeof(video_decode_totals) == VIDEO_DECODE_TOTAL_BYTES,
               "fixed bounded aggregate storage");

static uint64_t video_decode_sum(uint64_t old, uint64_t add, uint64_t maximum,
                                 video_decode_totals *t) {
    if (add > maximum - old) { t->flags |= VIDEO_DECODE_SATURATED; return maximum; }
    return old + add;
}

void video_decode_add(video_decode_accumulator *a, uint32_t cycles,
                      uint32_t ticks, int picture_ready) {
    video_decode_totals *t = &a->totals;
    if (ticks >= VIDEO_DECODE_SAMPLE_LIMIT_MS) t->flags |= VIDEO_DECODE_LONG_SAMPLE;
    if (!a->vcl) {
        t->header_calls = video_decode_sum(t->header_calls, 1, UINT16_MAX, t);
        t->header_cycles = video_decode_sum(t->header_cycles, cycles, UINT32_MAX, t);
        t->header_ticks = video_decode_sum(t->header_ticks, ticks, UINT32_MAX, t);
        return;
    }
    a->pending_cycles = video_decode_sum(a->pending_cycles, cycles, UINT64_MAX, t);
    a->pending_ticks = video_decode_sum(a->pending_ticks, ticks, UINT64_MAX, t);
    if (!picture_ready) return;
    t->pictures = video_decode_sum(t->pictures, 1, UINT32_MAX, t);
    t->cycles = video_decode_sum(t->cycles, a->pending_cycles, UINT64_MAX, t);
    t->ticks = video_decode_sum(t->ticks, a->pending_ticks, UINT64_MAX, t);
    uint32_t max_cycles = video_decode_sum(0, a->pending_cycles, UINT32_MAX, t);
    uint32_t max_ticks = video_decode_sum(0, a->pending_ticks, UINT32_MAX, t);
    if (max_cycles > t->max_cycles) t->max_cycles = max_cycles;
    if (max_ticks > t->max_ticks) t->max_ticks = max_ticks;
    uint32_t bucket = a->pending_ticks <= 30 ? 0 : a->pending_ticks <= 50 ? 1 :
        a->pending_ticks <= 62 ? 2 : a->pending_ticks <= 80 ? 3 : a->pending_ticks <= 100 ? 4 : 5;
    t->buckets[bucket] = video_decode_sum(t->buckets[bucket], 1, UINT32_MAX, t);
    a->pending_cycles = a->pending_ticks = 0;
}

static void video_decode_word(uint8_t *p, uint64_t value, uint32_t bytes) {
    for (uint32_t i = 0; i < bytes; ++i) p[i] = value >> (8 * i);
}
void video_decode_encode(const video_decode_totals *t, uint8_t out[VIDEO_DECODE_TOTAL_BYTES]) {
    out[0] = VIDEO_DECODE_TOTAL_VERSION; out[1] = t->flags;
    video_decode_word(out + 2, t->header_calls, 2);
    video_decode_word(out + 4, t->pictures, 4);
    video_decode_word(out + 8, t->cycles, 8);
    video_decode_word(out + 16, t->ticks, 8);
    video_decode_word(out + 24, t->max_cycles, 4);
    video_decode_word(out + 28, t->max_ticks, 4);
    for (uint32_t i = 0; i < VIDEO_DECODE_BUCKETS; ++i)
        video_decode_word(out + 32 + 4 * i, t->buckets[i], 4);
    video_decode_word(out + 56, t->header_cycles, 4);
    video_decode_word(out + 60, t->header_ticks, 4);
}

#pragma clang section text=""
