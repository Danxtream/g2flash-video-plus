/* Accounting only, never admission policy. SPDX-License-Identifier: GPL-3.0-only */
#include "heap_meter.h"
#pragma clang section text=".text.video"

static int video_heap_meter_take(video_heap_meter *m) {
    uint32_t empty = 0;
    if (__atomic_compare_exchange_n(&m->busy, &empty, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    __atomic_store_n(&m->dropped, 1, __ATOMIC_RELAXED); return 0;
}
void video_heap_meter_sample(video_heap_meter *m, uint32_t generation, uint32_t stream,
    uint32_t token, uint32_t index, const video_heap_view *v) {
    if (!generation || index >= 3 || !v || v->free_bytes == UINT32_MAX ||
        v->max_alloc > v->free_bytes || !video_heap_meter_take(m)) return;
    if (m->generation != generation) {
        m->generation = generation; m->stream = stream; m->token = 0;
        __atomic_store_n(&m->dropped, 0, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < 3; ++i) m->rows[i] = (video_heap_meter_row){0};
    }
    if (token) m->token = token;
    uint32_t decoder = __atomic_load_n(&m->decoder, __ATOMIC_RELAXED);
    uint32_t presenter = __atomic_load_n(&m->presenter, __ATOMIC_RELAXED);
    video_heap_meter_row *r = &m->rows[index];
    if (!r->samples) {
        r->baseline = r->minimum = v->free_bytes; r->min_largest = v->max_alloc;
        /* If contention missed the pre-allocation baseline, do not manufacture
         * an external-usage estimate from an already-charged video owner. */
        if (!index && (decoder || presenter)) r->external_max = UINT32_MAX;
    }
    if (v->free_bytes <= r->minimum) {
        r->minimum = v->free_bytes; r->largest_at_min = v->max_alloc;
        r->decoder_at_min = decoder; r->presenter_at_min = presenter;
    }
    if (v->max_alloc < r->min_largest) r->min_largest = v->max_alloc;
    uint32_t own = index ? 0 : decoder + presenter;
    if (r->baseline > v->free_bytes && r->baseline - v->free_bytes > own) {
        uint32_t external = r->baseline - v->free_bytes - own;
        if (r->external_max != UINT32_MAX && external > r->external_max) r->external_max = external;
    }
    if (r->samples != UINT32_MAX) ++r->samples;
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
}
int video_heap_meter_snapshot(video_heap_meter *m, uint32_t stream, uint8_t lens, uint8_t *out) {
    if (!video_heap_meter_take(m)) return 0;
    out[3] = VIDEO_HEAP_DIAGNOSTICS; out[4] = 1; out[6] = lens;
    out[7] = __atomic_load_n(&m->dropped, __ATOMIC_RELAXED) != 0;
    video_write32(out + 8, m->stream); video_write32(out + 12, m->token);
    video_write32(out + 16, m->generation);
    _Static_assert(sizeof(video_heap_meter_row) == 32 && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "heap meter layout");
    memcpy(out + 32, m->rows, sizeof(m->rows));
    for (uint32_t i = 0; i < 3; ++i) if (m->rows[i].samples) out[5] |= 1u << i;
    int valid = m->stream == stream && m->generation != 0;
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
    return valid;
}
#pragma clang section text=""
