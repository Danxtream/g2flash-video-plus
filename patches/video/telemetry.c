/* No clock writes, allocation or waits. SPDX-License-Identifier: GPL-3.0-only */
#include "telemetry.h"
#pragma clang section text=".text.video"

static int video_refusal_claim(video_refusal_ring *ring) {
    uint32_t empty = 0;
    return __atomic_compare_exchange_n(&ring->busy, &empty, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
/* Also usable after a control-lock timeout: bounded try-lock, never block. */
static int video_refused(customCfwContext *ctx, const uint8_t *data, uint16_t size,
                         const cfw_message_route *route, uint8_t reason,
                         uint8_t capacity, uint8_t pages, uint32_t detail) {
    if (!ctx || !route || route->origin < 1 || route->origin > 2) return -1;
    video_refusal_ring *ring = &ctx->video_telemetry.refusals[route->origin - 1];
    if (!video_refusal_claim(ring)) {
        __atomic_fetch_add(&ring->dropped, 1, __ATOMIC_RELAXED); return -1;
    }
    if (ring->serial != UINT32_MAX) {
        uint32_t serial = ++ring->serial;
        uint8_t *p = ring->rows[(serial - 1) % VIDEO_REFUSAL_ROWS];
        bzero(p, VIDEO_REFUSAL_BYTES);
        video_diag_write32(p, serial); video_diag_write32(p + 4, VIDEO_TICK);
        if (data && size >= 8) memcpy(p + 8, data + 4, 4);
        p[12] = data && size > 1 ? data[1] : 255;
        if ((p[12] == VIDEO_CONTROL_NAL || p[12] == VIDEO_CONTROL_NAL_TIMED ||
             p[12] == VIDEO_CONTROL_NAL_ROOM || p[12] == VIDEO_CONTROL_NAL_TIMED_ROOM) && size >= 10)
            memcpy(p + 8, data + 6, 4);
        p[13] = data && size >= 9 && p[12] == VIDEO_CONTROL_PAGE ? data[8] : 0;
        p[14] = reason; p[15] = route->reply_capacity;
        p[16] = capacity; p[17] = pages;
        video_diag_write32(p + 20, detail);
    } else __atomic_fetch_add(&ring->dropped, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&ring->busy, 0, __ATOMIC_RELEASE);
    return -1;
}
static int video_refusal_snapshot(customCfwContext *ctx, uint8_t origin, uint8_t here, uint8_t *out) {
    video_refusal_ring *ring = &ctx->video_telemetry.refusals[origin - 1];
    if (!video_refusal_claim(ring)) return VIDEO_CONTROL_BUSY;
    out[3] = VIDEO_REFUSAL_DIAGNOSTICS; out[4] = 1; out[5] = origin; out[6] = here;
    uint32_t count = ring->serial < VIDEO_REFUSAL_ROWS ? ring->serial : VIDEO_REFUSAL_ROWS;
    out[7] = count; video_diag_write32(out + 8, ring->serial);
    video_diag_write32(out + 12, __atomic_load_n(&ring->dropped, __ATOMIC_RELAXED));
    out[16] = VIDEO_REFUSAL_BYTES;
    for (uint32_t i = 0; i < count; ++i)
        memcpy(out + 32 + i * VIDEO_REFUSAL_BYTES, ring->rows[(ring->serial - 1 - i) % VIDEO_REFUSAL_ROWS], VIDEO_REFUSAL_BYTES);
    __atomic_store_n(&ring->busy, 0, __ATOMIC_RELEASE);
    return VIDEO_CONTROL_ACCEPTED;
}

#pragma clang section text=""
