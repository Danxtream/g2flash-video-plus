/* Synthetic accounting, no allocator/clock. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../patches/video/heap_meter.h"
static void video_write32(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < 4; ++i) p[i] = n >> (8 * i); }
static uint32_t words[0x70800 / 4], mutex_busy, takes, gives, reads;
static uint32_t read_word(uint32_t a) {
    ++reads;
    if (a == 0x20076d18) return 0x20010000;
    if (a == 0x2000478c) return 0x20011000;
    if (a == 0x20010000 + 60 || a == 0x20011000 + 60) return 150;
    if (a == 0x20010000 + 64 || a == 0x20011000 + 64) return 4;
    if (a == 0x20076e70) return 0x20075930;
    if (a == 0x20075930 + 8) return 24576;
    if (a == 0x20075930 + 24) return 0x20312824;
    if (a == 0x20075930 + 4) return 12;
    if (a == 0x20075930) return 24570;
    if (a == 0x20076c38) return 123;
    if (a == 0x20076e08) return 0x202020a8;
    if (a >= 0x202020a8 && a < 0x202728a8) return words[(a - 0x202020a8) / 4];
    return 0;
}
#define VIDEO_METER_READ(a) read_word(a)
#define VIDEO_METER_COUNT(q) ((q) == 0x20010000 ? 150u : 7u)
#define VIDEO_METER_TAKE(m) ((void)(m), ++takes, mutex_busy ? -1 : 0)
#define VIDEO_METER_GIVE(m) ((void)(m), ++gives)
#include "../../patches/video/heap_meter.c"
int main(void) {
    video_heap_meter m = {0}; video_heap_view v = {400000, 390000};
    video_heap_meter_sample(&m, 0, 1, 0, 0, &v); assert(!m.generation);
    for (uint32_t i = 0; i < 3; ++i) video_heap_meter_sample(&m, 1, 50, 101, i, &v);
    m.decoder = 150000; m.presenter = 70000;
    v = (video_heap_view){150000, 140000}; video_heap_meter_sample(&m, 1, 50, 101, 0, &v);
    assert(m.rows[0].external_max == 30000 && m.rows[0].decoder_at_min == 150000);
    assert(m.rows[0].presenter_at_min == 70000 && m.rows[0].largest_at_min == 140000);
    v = (video_heap_view){160000, 120000}; video_heap_meter_sample(&m, 1, 50, 101, 0, &v);
    assert(m.rows[0].minimum == 150000 && m.rows[0].min_largest == 120000);
    uint32_t n = m.rows[0].samples; v.free_bytes = UINT32_MAX;
    video_heap_meter_sample(&m, 1, 50, 101, 0, &v); assert(m.rows[0].samples == n);
    v = (video_heap_view){140000, 130000}; m.busy = 1;
    video_heap_meter_sample(&m, 1, 50, 101, 0, &v); assert(m.dropped && m.rows[0].samples == n);
    m.busy = 0; uint8_t out[128] = {0};
    assert(video_heap_meter_snapshot(&m, 50, 2, out) && out[3] == 9 && out[7]);
    assert(!video_heap_meter_snapshot(&m, 49, 2, out));
    m.decoder = m.presenter = 0; v = (video_heap_view){400000, 390000};
    video_heap_meter_sample(&m, 2, 51, 102, 0, &v);
    assert(m.rows[0].samples == 1 && !m.rows[0].external_max && !m.dropped);
    m.generation = 0; m.decoder = 100;
    video_heap_meter_sample(&m, 3, 52, 103, 0, &v);
    assert(m.rows[0].external_max == UINT32_MAX); /* Missed START baseline is unknown. */
    assert(m.queues[0] == 150 && m.queues[1] == 7 && m.queues[2] == 18 && m.queues[5] == 1);
    uint32_t before = reads; video_heap_meter_sample(&m, 3, 52, 103, 0, &v);
    assert(before == reads); /* Equal minima do not resample queues. */
    m.shape = (video_heap_shape){0}; m.refused = 0; mutex_busy = 1;
    video_heap_meter_refused(&m); assert(m.shape.status == 2 && takes == 1 && !gives);
    mutex_busy = 0; video_heap_meter_refused(&m); assert(takes == 1); /* Exactly once. */
    m.shape = (video_heap_shape){0}; m.refused = 0;
    uint32_t p = 0xc74 / 4;
    words[p] = 58000; p += (58000 + 4) / 4;
    words[p] = 300; p += (300 + 4) / 4;
    words[p] = (sizeof(words) - p * 4 - 8) | 1;
    video_heap_meter_refused(&m);
    assert(m.shape.status == 1 && m.shape.blocks == 2 && m.shape.small == 1 && gives == 1);
    assert(m.shape.large[0][1] == 58000 && m.shape.large[1][1] == 300);
    assert(video_heap_shape_snapshot(&m, 52, 2, out) && out[3] == 10);
    m.shape = (video_heap_shape){0}; m.refused = 0; words[0xc74 / 4] = UINT32_MAX;
    video_heap_meter_refused(&m); assert(m.shape.status == 3 && gives == 2);
    m.shape = (video_heap_shape){0}; m.refused = 0; m.busy = 1;
    video_heap_meter_refused(&m); m.busy = 0;
    video_heap_meter_refused(&m); assert(gives == 2);
    assert(video_heap_shape_snapshot(&m, 52, 2, out) && out[32] == 4);
    puts("heap minima, paired ledgers, external usage, invalid views, contention and reset PASS");
}
