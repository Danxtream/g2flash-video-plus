/* Synthetic accounting, no allocator/clock. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "../../patches/video/heap_meter.h"
static void video_write32(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < 4; ++i) p[i] = n >> (8 * i); }
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
    puts("heap minima, paired ledgers, external usage, invalid views, contention and reset PASS");
}
