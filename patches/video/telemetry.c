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

#ifndef VIDEO_CLOCK_READ32
/* Exact donor addresses; Apollo510 register offsets checked against CMSIS.
 * Keep reads out of line so distant literals do not become ROPI MOVW fixups. */
__attribute__((noinline)) static uint32_t video_clock_read32(uint32_t address) {
    return *(const volatile uint32_t *)(uintptr_t)address;
}
#define VIDEO_CLOCK_READ32 video_clock_read32
#define VIDEO_CLOCK_CACHED() (*(const volatile uint8_t *)0x20004d74U)
#endif
#ifndef VIDEO_CLOCK_CYCLES
#define VIDEO_CLOCK_CYCLES VIDEO_CYCLES
#endif

static video_clock_pair video_clock_now(void) {
    uint32_t before = VIDEO_TICK, cycles = VIDEO_CLOCK_CYCLES, after = VIDEO_TICK;
    return (video_clock_pair){before, cycles, after, VIDEO_CLOCK_READ32(0x40021000U)};
}
static void video_clock_capture(customCfwContext *ctx, uint8_t phase, video_clock_pair first) {
    video_clock_state *s = &ctx->video_telemetry.clock;
    uint8_t *out = s->rows[phase];
    bzero(out, 128);
    video_clock_pair last = video_clock_now();
    uint32_t span = last.tick - first.tick;
    uint32_t perf = VIDEO_CLOCK_READ32(0x40021000U);
    out[0] = 1; out[1] = VIDEO_CONTROL_DIAGNOSTICS; out[3] = VIDEO_CLOCK_DIAGNOSTICS;
    out[4] = phase; out[5] = 1; out[6] = ctx->video_control.owner_here;
    out[7] = VIDEO_CLOCK_VALID | (span >= VIDEO_CLOCK_BRACKET_MS && span <= VIDEO_CLOCK_MAX_MS ? VIDEO_CLOCK_LONG : 0);
    video_diag_write32(out + 8, s->stream); video_diag_write32(out + 12, s->generation);
    video_diag_write32(out + 16, first.tick); video_diag_write32(out + 20, first.cycles);
    video_diag_write32(out + 24, first.after); video_diag_write32(out + 28, last.tick);
    video_diag_write32(out + 32, last.cycles); video_diag_write32(out + 36, last.after);
    const uint32_t addresses[] = {0x40004030U, 0x40004044U, 0x40004120U,
        0x400204d8U, 0x400204dcU, 0x400204e0U, 0x2007646cU, 0xe0001000U};
    video_diag_write32(out + 40, perf);
    for (uint32_t i = 0; i < sizeof(addresses) / sizeof(addresses[0]); ++i)
        video_diag_write32(out + 44 + i * 4, VIDEO_CLOCK_READ32(addresses[i]));
    video_diag_write32(out + 76, VIDEO_CLOCK_CACHED());
    video_diag_write32(out + 80, VIDEO_CLOCK_READ32(0x40021000U));
    video_diag_write32(out + 84, first.perf);
    video_diag_write32(out + 88, VIDEO_CLOCK_READ32(0x400204e4U));
    video_diag_write32(out + 92, VIDEO_CLOCK_READ32(0x400204e8U));
    if (video_read32(out + 72) & 1) out[7] |= VIDEO_CLOCK_COUNTER;
    if (perf != video_read32(out + 80) || perf != first.perf) out[7] |= VIDEO_CLOCK_CHANGED;
    if (first.tick != first.after || last.tick != last.after) out[7] |= VIDEO_CLOCK_SKEW;
}
static void video_clock_start_locked(customCfwContext *ctx) {
    video_clock_state *s = &ctx->video_telemetry.clock;
    bzero((uint8_t *)s, sizeof(*s));
    s->stream = ctx->video_control.stream; s->generation = ctx->video_control.control_generation;
    s->anchor = s->previous = video_clock_now();
    video_clock_capture(ctx, VIDEO_CLOCK_START, s->anchor);
}
static void video_clock_step_locked(customCfwContext *ctx) {
    video_clock_state *s = &ctx->video_telemetry.clock;
    if (s->generation && VIDEO_TICK - s->anchor.tick >= VIDEO_CLOCK_BRACKET_MS) {
        video_clock_capture(ctx, VIDEO_CLOCK_STEADY, s->anchor);
        s->previous = s->anchor; s->anchor = video_clock_now();
    }
}
static void video_clock_stop_locked(customCfwContext *ctx, uint32_t generation) {
    video_clock_state *s = &ctx->video_telemetry.clock;
    if (s->generation == generation && !(s->rows[VIDEO_CLOCK_STOP][7] & VIDEO_CLOCK_VALID))
        video_clock_capture(ctx, VIDEO_CLOCK_STOP, s->previous);
}
#pragma clang section text=""
