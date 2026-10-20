/* Accounting only, never admission policy. SPDX-License-Identifier: GPL-3.0-only */
#include "heap_meter.h"
#pragma clang section text=".text.video"

#ifndef VIDEO_METER_READ
#define VIDEO_METER_READ(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define VIDEO_METER_COUNT(q) (((uint32_t (*)(uint32_t))0x004433a1U)(q))
#define VIDEO_METER_TAKE(m) (((int (*)(uint32_t, uint32_t))0x00442f91U)(m, 0))
#define VIDEO_METER_GIVE(m) (((int (*)(uint32_t))0x00442ff7U)(m))
#endif

/* Persistent donor queues; its count API uses only a short critical section.
 * Authenticate geometry before calling it. No queue contents are dereferenced. */
static uint16_t video_meter_queue(uint32_t address) {
    uint32_t q = VIDEO_METER_READ(address);
    if ((q & 3) || q < 0x20000000U || q > 0x203ffff0U - 72 ||
        VIDEO_METER_READ(q + 60) != 150 || VIDEO_METER_READ(q + 64) != 4) return UINT16_MAX;
    uint32_t n = VIDEO_METER_COUNT(q);
    return n <= 150 ? n : UINT16_MAX;
}
static void video_meter_queues(video_heap_meter *m) {
    m->queues[0] = video_meter_queue(0x20076d18U);
    m->queues[1] = video_meter_queue(0x2000478cU);
    m->queues[2] = UINT16_MAX;
    /* Static UART0 RX stream buffer: +0 tail, +4 head, +8 length, +24 data.
     * A stable head brackets the tail read; invalid/racing samples stay unknown. */
    uint32_t q = VIDEO_METER_READ(0x20076e70U);
    if (q == 0x20075930U && VIDEO_METER_READ(q + 8) == 24576 &&
        VIDEO_METER_READ(q + 24) == 0x20312824U) {
        uint32_t head = VIDEO_METER_READ(q + 4), tail = VIDEO_METER_READ(q);
        if (head < 24576 && tail < 24576 && head == VIDEO_METER_READ(q + 4))
            m->queues[2] = (head + 24576 - tail) % 24576;
    }
    m->queues[3] = __atomic_load_n(&m->returns, __ATOMIC_RELAXED);
    m->queues[4] = __atomic_load_n(&m->replies, __ATOMIC_RELAXED);
    m->queues[5] = 1; /* Extension present; old replies had zero reserved bytes. */
}

/* Payload sizes only: no heap payload, title or allocation contents are read.
 * Bounded by the minimum physical TLSF block, with every successor validated. */
static int video_meter_walk(video_heap_shape *s, uint32_t arena, uint32_t bytes) {
    uint32_t end = arena + bytes, p = arena + 0xc74;
    for (uint32_t n = 0; n < bytes / 16; ++n) {
        if ((p & 3) || p > end - 4) return 0;
        uint32_t flags = VIDEO_METER_READ(p), size = flags & ~3u;
        if (!size) return p == end - 4;
        if (size < 12 || p > end - 8 || size > end - p - 8) return 0;
        if (flags & 1) s->free_bytes += size;
        else {
            ++s->blocks; s->used_bytes += size;
            if (size <= 512) ++s->small;
            for (uint32_t i = 0; i < 4; ++i) if (size > s->large[i][1]) {
                for (uint32_t j = 3; j > i; --j) {
                    s->large[j][0] = s->large[j - 1][0];
                    s->large[j][1] = s->large[j - 1][1];
                }
                s->large[i][0] = p; s->large[i][1] = size; break;
            }
        }
        p += size + 4;
    }
    return 0;
}

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
        m->shape = (video_heap_shape){0};
        __atomic_store_n(&m->refused, 0, __ATOMIC_RELAXED);
        for (uint32_t i = 0; i < 6; ++i) m->queues[i] = UINT16_MAX;
        m->queues[5] = 0;
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
        if (!index && (!r->samples || v->free_bytes < r->minimum)) video_meter_queues(m);
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
    memcpy(out + 20, m->queues, sizeof(m->queues));
    _Static_assert(sizeof(video_heap_meter_row) == 32 && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "heap meter layout");
    memcpy(out + 32, m->rows, sizeof(m->rows));
    for (uint32_t i = 0; i < 3; ++i) if (m->rows[i].samples) out[5] |= 1u << i;
    int valid = m->stream == stream && m->generation != 0;
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
    return valid;
}

void video_heap_meter_refused(video_heap_meter *m) {
    uint32_t empty = 0;
    if (!__atomic_compare_exchange_n(&m->refused, &empty, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) return;
    if (!video_heap_meter_take(m)) return;
    if (!m->shape.status) {
        m->shape.status = 2; /* Unavailable: try-lock busy, no wait or retry. */
        uint32_t mutex = VIDEO_METER_READ(0x20076c38U);
        if (mutex && VIDEO_METER_TAKE(mutex) == 0) {
            if (VIDEO_METER_READ(0x20076e08U) == 0x202020a8U)
                m->shape.status = video_meter_walk(&m->shape, 0x202020a8U, 0x70800) ? 1 : 3;
            VIDEO_METER_GIVE(mutex);
        }
    }
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
}
int video_heap_shape_snapshot(video_heap_meter *m, uint32_t stream, uint8_t lens, uint8_t *out) {
    if (!video_heap_meter_take(m)) return 0;
    out[3] = VIDEO_HEAP_SHAPE_DIAGNOSTICS; out[4] = 1; out[6] = lens;
    video_write32(out + 8, m->stream); video_write32(out + 12, m->token);
    video_write32(out + 16, m->generation);
    memcpy(out + 20, m->queues, sizeof(m->queues));
    memcpy(out + 32, &m->shape, sizeof(m->shape));
    if (!m->shape.status && __atomic_load_n(&m->refused, __ATOMIC_RELAXED)) video_write32(out + 32, 4);
    int valid = m->stream == stream && m->generation != 0;
    __atomic_store_n(&m->busy, 0, __ATOMIC_RELEASE);
    return valid;
}
#pragma clang section text=""
