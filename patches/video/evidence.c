/* Lossless verification outside the decode interval. SPDX-License-Identifier: GPL-3.0-only */
#include "evidence.h"

#pragma clang section text=".text.video"

_Static_assert(sizeof(video_frame_result) == 64, "fixed frame result schema");
_Static_assert(sizeof(video_evidence) + VIDEO_ALLOC_ALIGNMENT + 7 <= VIDEO_EVIDENCE_ALLOWANCE,
               "verification ledger charge stays within its allowance");

#ifndef VIDEO_CYCLES
#define VIDEO_CYCLES (*(const volatile uint32_t *)0xe0001004U)
static void video_clock_init(void) {
    *(volatile uint32_t *)0xe0001fb0U = 0xc5acce55U;
    *(volatile uint32_t *)0xe000edfcu |= 1u << 24;
    *(volatile uint32_t *)0xe0001000U |= 1u;
}
#define VIDEO_CLOCK_INIT video_clock_init
#endif

#ifndef VIDEO_CLOCK_CALIBRATE
static uint32_t video_clock_calibrate(void) {
    uint32_t started = VIDEO_TICK, cycles = VIDEO_CYCLES;
    for (uint32_t attempt = 0; attempt < VIDEO_CLOCK_SAMPLE_ATTEMPTS; ++attempt) {
        uint32_t elapsed = VIDEO_TICK - started;
        if (elapsed >= VIDEO_CLOCK_SAMPLE_TICKS) {
            uint32_t rate = (VIDEO_CYCLES - cycles) / elapsed;
            return elapsed <= 2 * VIDEO_CLOCK_SAMPLE_TICKS && rate <= UINT32_MAX / 1000 ? rate * 1000 : 0;
        }
        if (VIDEO_OS_DELAY(1) != 0) return 0;
    }
    return 0;
}
#define VIDEO_CLOCK_CALIBRATE video_clock_calibrate
#endif

/* One counter pair also feeds optional verification: no duplicate sampling,
 * calibration in normal mode, or statistics-driven worker wait. */
static void video_decode_before(video_owner *owner, uint32_t sequence, uint8_t kind) {
    video_decode_accumulator *a = &owner->timing;
    if (!a->clock_started) { VIDEO_CLOCK_INIT(); a->clock_started = 1; }
    if (owner->evidence) video_evidence_before(owner, sequence);
    a->vcl = kind >= 1 && kind <= 5;
    a->start_tick = VIDEO_TICK; a->start_cycles = VIDEO_CYCLES;
}
static void video_decode_after(video_owner *owner, g2_h264_result result) {
    video_decode_accumulator *a = &owner->timing;
    uint32_t cycles = VIDEO_CYCLES - a->start_cycles;
    uint32_t now = VIDEO_TICK, ticks = now - a->start_tick;
    video_decode_add(a, cycles, ticks, result == G2_H264_FRAME_READY);
    if (owner->evidence) video_evidence_after(owner, result, cycles, ticks, now);
}

uint32_t video_frame_crc(const g2_h264_frame_info *frame) {
    uint32_t crc = UINT32_MAX;
    for (uint32_t y = 0; y < frame->height; ++y)
        for (uint32_t x = 0; x < frame->width; ++x) {
            crc ^= frame->y[y * frame->stride + x];
            for (uint32_t bit = 0; bit < 8; ++bit)
                crc = (crc >> 1) ^ (0xedb88320U & (0u - (crc & 1)));
        }
    return crc ^ UINT32_MAX;
}
#ifndef VIDEO_FRAME_CRC
#define VIDEO_FRAME_CRC video_frame_crc
#endif

static int video_evidence_space(video_owner *owner) {
    video_evidence *w = owner->evidence;
    if (!w || !owner->queue.count || w->high - w->acked < VIDEO_EVIDENCE_ROWS) return 1;
    if (!w->blocked) { ++w->stalls; w->blocked = 1; }
    return 0;
}
static void video_evidence_before(video_owner *owner, uint32_t sequence) {
    video_evidence *w = owner->evidence;
    if (!w->building.calls) {
        w->building.first_nal = sequence;
        w->building.clock_before = VIDEO_CLOCK_CALIBRATE();
        if (!w->building.clock_before) w->building.flags |= VIDEO_TIMING_INVALID;
    }
    w->building.last_nal = sequence;
}
static void video_evidence_after(video_owner *owner, g2_h264_result result,
                                  uint32_t cycles, uint32_t ticks, uint32_t now) {
    video_evidence *w = owner->evidence;
    video_frame_result *r = &w->building;
    if (cycles > UINT32_MAX - r->cycles || ticks > UINT32_MAX - r->ticks || r->calls == UINT32_MAX)
        r->flags |= VIDEO_TIMING_INVALID;
    r->cycles += cycles; r->ticks += ticks; ++r->calls;
    if (result == G2_H264_FRAME_READY) {
        r->finishing_cycles = cycles; r->finishing_ticks = ticks; r->decode_tick = now;
        r->clock_after = VIDEO_CLOCK_CALIBRATE();
        if (!r->clock_after) r->flags |= VIDEO_TIMING_INVALID;
    } else if (result == G2_H264_CONSUMED) {
        r->no_output_cycles += cycles;
        __atomic_fetch_add(&w->no_output_cycles, cycles, __ATOMIC_RELAXED);
        __atomic_fetch_add(&w->no_output_ticks, ticks, __ATOMIC_RELAXED);
        __atomic_fetch_add(&w->no_output_calls, 1, __ATOMIC_RELAXED);
    }
}
static void video_evidence_frame(video_owner *owner, const g2_h264_frame_info *frame) {
    video_frame_result *r = &owner->evidence->building;
    r->geometry = frame->width | frame->height << 16;
    r->crc = VIDEO_FRAME_CRC(frame);
}
static int video_evidence_publish_locked(video_owner *owner, uint32_t ordinal) {
    video_evidence *w = owner->evidence;
    if (!w || !ordinal || ordinal != w->high + 1 || w->high - w->acked >= VIDEO_EVIDENCE_ROWS ||
        owner->context->video_presentation.presented != ordinal) return 0;
    w->building.ordinal = ordinal;
    w->building.copy_tick = owner->context->video_presentation.copy_tick;
    w->rows[(ordinal - 1) % VIDEO_EVIDENCE_ROWS] = w->building;
    w->high = ordinal;
    w->building = (video_frame_result){0};
    return 1;
}

static uint32_t video_evidence_word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void video_evidence_write(uint8_t *p, uint32_t word) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = (uint8_t)(word >> (i * 8));
}
static video_owner *video_evidence_owner_locked(customCfwContext *ctx, const uint8_t *p, uint8_t origin) {
    video_owner *owner = ctx->video_owner;
    return owner && owner->evidence && !ctx->video.preparing && ctx->video.published &&
        origin == ctx->video_control.owner_origin &&
        video_evidence_word(p + 8) == ctx->video_control.stream &&
        video_evidence_word(p + 12) == owner->token ? owner : 0;
}
static int video_evidence_read_locked(customCfwContext *ctx, const uint8_t *p,
                                      uint8_t origin, uint8_t *out) {
    video_owner *owner = video_evidence_owner_locked(ctx, p, origin);
    if (!owner) return VIDEO_CONTROL_STALE;
    video_evidence *w = owner->evidence;
    uint32_t ordinal = video_evidence_word(p + 16);
    if (!ordinal || ordinal <= w->acked) return VIDEO_CONTROL_STALE;
    if (ordinal > w->high) return VIDEO_CONTROL_BUSY;
    const video_frame_result *r = &w->rows[(ordinal - 1) % VIDEO_EVIDENCE_ROWS];
    if (r->ordinal != ordinal) return VIDEO_CONTROL_STALE;
    video_evidence_write(out + 4, ctx->video_control.stream);
    video_evidence_write(out + 8, owner->token);
    video_evidence_write(out + 12, ordinal);
    /* Copy each scalar as LE32 without assuming native packing or host endian. */
    const uint32_t words[] = {r->ordinal, r->first_nal, r->last_nal, r->geometry, r->crc,
        r->cycles, r->ticks, r->clock_before, r->clock_after, r->flags, r->decode_tick,
        r->copy_tick, r->calls, r->finishing_cycles, r->finishing_ticks, r->no_output_cycles};
    for (uint32_t i = 0; i < 16; ++i) video_evidence_write(out + 16 + i * 4, words[i]);
    return VIDEO_CONTROL_ACCEPTED;
}
static int video_evidence_ack_locked(customCfwContext *ctx, const uint8_t *p,
                                     uint8_t origin, uint8_t *out) {
    video_owner *owner = video_evidence_owner_locked(ctx, p, origin);
    if (!owner) return VIDEO_CONTROL_STALE;
    video_evidence *w = owner->evidence;
    uint32_t ordinal = video_evidence_word(p + 16);
    if (ordinal > w->high) return VIDEO_CONTROL_FORMAT;
    if (ordinal > w->acked) {
        w->acked = ordinal; w->blocked = 0;
        uint32_t deadline = VIDEO_TICK + VIDEO_INACTIVITY_LIMIT_MS;
        __atomic_store_n(&ctx->video_control.active_deadline, deadline ? deadline : 1, __ATOMIC_RELEASE);
        video_worker_wake_locked(owner->token, VIDEO_WAKE_INPUT);
    }
    video_evidence_write(out + 4, ctx->video_control.stream);
    video_evidence_write(out + 8, owner->token);
    video_evidence_write(out + 12, w->acked);
    return VIDEO_CONTROL_ACCEPTED;
}
static void video_evidence_status_locked(customCfwContext *ctx, uint8_t *out) {
    video_owner *owner = ctx->video_owner;
    const video_evidence *w = owner && !ctx->video.preparing && ctx->video.published ? owner->evidence : 0;
    video_evidence_write(out + 96, w ? VIDEO_EVIDENCE_ROWS : 0);
    video_evidence_write(out + 100, w ? w->high : 0);
    video_evidence_write(out + 104, w ? w->acked : 0);
    video_evidence_write(out + 108, w ? w->stalls : 0);
    /* Total no-output calls include any tail after the last completed frame. */
    video_evidence_write(out + 112, w ? __atomic_load_n(&w->no_output_cycles, __ATOMIC_RELAXED) : 0);
    video_evidence_write(out + 116, w ? __atomic_load_n(&w->no_output_ticks, __ATOMIC_RELAXED) : 0);
    video_evidence_write(out + 120, w ? __atomic_load_n(&w->no_output_calls, __ATOMIC_RELAXED) : 0);
    video_evidence_write(out + 124, ctx->video_control.error == VIDEO_CONTROL_INCOMPLETE);
}

#pragma clang section text=""
