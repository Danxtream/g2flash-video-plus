/* No hot-path logging, allocation or waiting. SPDX-License-Identifier: GPL-3.0-only */
#include "profile.h"
#pragma clang section text=".text.video"

video_profile_capture video_profile_begin(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    uint32_t generation = ctx ? __atomic_load_n(&ctx->video_profile.generation, __ATOMIC_ACQUIRE) : 0;
    if (!generation || generation != __atomic_load_n(&ctx->video_control.start_guard, __ATOMIC_ACQUIRE))
        return (video_profile_capture){0};
    return (video_profile_capture){generation, VIDEO_TICK, VIDEO_CYCLES};
}

void video_profile_end(video_profile_capture captured, uint32_t stage, uint32_t bytes) {
    if (!captured.generation || stage >= VIDEO_PROFILE_STAGES) return;
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return;
    video_profile_state *s = &ctx->video_profile;
    if (captured.generation != __atomic_load_n(&s->generation, __ATOMIC_ACQUIRE)) return;
    uint32_t ticks = VIDEO_TICK - captured.tick, cycles = VIDEO_CYCLES - captured.cycles;
    for (uint32_t i = 0; i < VIDEO_PROFILE_SLOTS; ++i) {
        video_profile_mail *m = &s->mail[stage][i];
        uint32_t empty = 0;
        if (!__atomic_compare_exchange_n(&m->phase, &empty, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        m->generation = captured.generation; m->ticks = ticks; m->cycles = cycles; m->bytes = bytes;
        __atomic_store_n(&m->phase, 2, __ATOMIC_RELEASE);
        return;
    }
    __atomic_fetch_or(&s->flags, VIDEO_PROFILE_DROPPED, __ATOMIC_RELAXED);
    if (__atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED) == UINT32_MAX)
        __atomic_fetch_or(&s->flags, VIDEO_PROFILE_SATURATED, __ATOMIC_RELAXED);
}

/* Called only by the presenter under publication protection. Never reset a
 * mailbox phase: an old producer may still be publishing into that slot. */
static void video_profile_start_locked(video_presenter *p) {
    video_profile_state *s = &p->decoder->context->video_profile;
    __atomic_store_n(&s->generation, 0, __ATOMIC_RELEASE);
    bzero((uint8_t *)s->rows, sizeof(s->rows));
    s->stream = p->sync.stream; s->token = p->token;
    __atomic_store_n(&s->dropped, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s->flags, 0, __ATOMIC_RELAXED);
    VIDEO_CLOCK_INIT();
    __atomic_store_n(&s->generation, p->generation, __ATOMIC_RELEASE);
}

static uint32_t video_profile_add(video_profile_state *s, uint32_t a, uint32_t b) {
    if (b <= UINT32_MAX - a) return a + b;
    __atomic_fetch_or(&s->flags, VIDEO_PROFILE_SATURATED, __ATOMIC_RELAXED);
    return UINT32_MAX;
}

static void video_profile_fold_locked(video_profile_state *s) {
    uint32_t generation = __atomic_load_n(&s->generation, __ATOMIC_ACQUIRE);
    for (uint32_t stage = 0; stage < VIDEO_PROFILE_STAGES; ++stage)
        for (uint32_t i = 0; i < VIDEO_PROFILE_SLOTS; ++i) {
            video_profile_mail *m = &s->mail[stage][i];
            if (__atomic_load_n(&m->phase, __ATOMIC_ACQUIRE) != 2) continue;
            if (m->generation == generation) {
                video_profile_row *r = &s->rows[stage];
                r->count = video_profile_add(s, r->count, 1);
                r->bytes = video_profile_add(s, r->bytes, m->bytes);
                r->ticks = video_profile_add(s, r->ticks, m->ticks);
                if (m->ticks > r->maximum) r->maximum = m->ticks;
                /* Counter frequency is not sampled here. Long brackets can
                 * span multiple cycle wraps; retain ticks and flag uncertainty. */
                if (m->ticks > 1000) __atomic_fetch_or(&s->flags, VIDEO_PROFILE_SPAN, __ATOMIC_RELAXED);
                else {
                    uint32_t low = r->cycles_lo + m->cycles;
                    r->cycles_hi = video_profile_add(s, r->cycles_hi, low < r->cycles_lo);
                    r->cycles_lo = low;
                }
            }
            __atomic_store_n(&m->phase, 0, __ATOMIC_RELEASE);
        }
}

static void video_profile_write(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}

static int video_profile_pack(uint8_t *out, uint32_t bytes, const video_frame_descriptor *frame) {
    video_profile_capture captured = video_profile_begin();
    int result = video_pack_frame(out, bytes, frame);
    video_profile_end(captured, VIDEO_PROFILE_PACK, frame->bytes);
    return result;
}

static int video_profile_snapshot_locked(customCfwContext *ctx, uint8_t page, uint8_t origin, uint8_t *out) {
    video_profile_state *s = &ctx->video_profile;
    uint32_t generation = __atomic_load_n(&s->generation, __ATOMIC_ACQUIRE);
    if (page > 1) return VIDEO_CONTROL_FORMAT;
    if (!generation || generation != ctx->video_control.control_generation ||
        origin != ctx->video_control.owner_origin) return VIDEO_CONTROL_STALE;
    out[3] = VIDEO_PROFILE_DIAGNOSTICS; out[4] = page; out[5] = 1; out[6] = ctx->video_control.owner_here;
    video_profile_write(out + 8, generation); video_profile_write(out + 12, s->stream); video_profile_write(out + 16, s->token);
    video_profile_write(out + 20, __atomic_load_n(&s->dropped, __ATOMIC_RELAXED));
    video_profile_write(out + 24, __atomic_load_n(&s->flags, __ATOMIC_RELAXED));
    for (uint32_t i = 0; i < 4; ++i) {
        const video_profile_row *r = &s->rows[page * 4 + i];
        uint32_t words[] = {r->count, r->bytes, r->ticks, r->cycles_lo, r->cycles_hi, r->maximum};
        for (uint32_t j = 0; j < 6; ++j) video_profile_write(out + 32 + i * 24 + j * 4, words[j]);
    }
    return VIDEO_CONTROL_ACCEPTED;
}

#ifndef VIDEO_CORE_REGISTER
#define VIDEO_CORE_REGISTER(address) (*(const volatile uint32_t *)(address))
#endif
/* The memory check reads the cycle counter directly, apart from the decode
 * timing's VIDEO_CYCLES, so it never counts as playback measurement work. */
#define VIDEO_BENCH_CYCLES VIDEO_CORE_REGISTER(0xE0001004u)
/* The first check on the glasses read zero for every interval: the counter
 * can be off between pictures (seen in the clock reports), so the check turns
 * it on with the same writes as the decode timing's start and records DWT_CTRL
 * before and after. */
#ifndef VIDEO_BENCH_COUNTER_ON
#define VIDEO_BENCH_COUNTER_ON() video_clock_init()
#endif

/* Measuring build: once per stream, on the decoder worker right after its
 * second picture, read the core's cache and MPU control registers (read only;
 * CCR bit 16 is the data cache, bit 17 the instruction cache) and time plain
 * byte loads from the decoded picture's own memory against an arithmetic loop.
 * Volatile loads keep the compiler from merging or skipping them. One sample
 * each: interrupts can only make a reading slower, never faster. */
__attribute__((noinline, minsize))
static void video_bench_run(const g2_h264_frame_info *frame, video_bench_result *r) {
    const volatile uint8_t *y = frame->y;
    const uint32_t stride = frame->stride, width = frame->width, height = frame->height;
    volatile uint32_t sink = 0;
    uint32_t sum = 0, start;
    *r = (video_bench_result){0};
    r->ccr = VIDEO_CORE_REGISTER(0xE000ED14u);
    r->mpu_ctrl = VIDEO_CORE_REGISTER(0xE000ED94u);
    r->frame_address = (uint32_t)(uintptr_t)frame->y;
    r->stride = stride; r->width = width; r->height = height;
    if (!y || width <= 24 || height <= 21) return;
    r->counter_before = VIDEO_CORE_REGISTER(0xE0001000u);
    VIDEO_BENCH_COUNTER_ON();
    start = VIDEO_BENCH_CYCLES;
    for (uint32_t i = 0; i < 4096; ++i) sum = sum * 3u + i;
    r->alu = VIDEO_BENCH_CYCLES - start;
    start = VIDEO_BENCH_CYCLES;
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t col = 0; col < width; ++col) sum += y[row * stride + col];
    r->frame_read = VIDEO_BENCH_CYCLES - start;
    r->frame_bytes = width * height;
    /* The same 16 rows twice: the second pass shows what a cache hit costs. */
    for (uint32_t pass = 0; pass < 2; ++pass) {
        start = VIDEO_BENCH_CYCLES;
        for (uint32_t row = 0; row < 16; ++row)
            for (uint32_t col = 0; col < width; ++col) sum += y[row * stride + col];
        if (pass) r->block_hot = VIDEO_BENCH_CYCLES - start;
        else r->block_cold = VIDEO_BENCH_CYCLES - start;
    }
    r->block_bytes = 16 * width;
    /* 32 windows the size motion prediction reads for a 16x16 block with its
     * filter margins (21 rows of 24 bytes), spread across the picture. */
    start = VIDEO_BENCH_CYCLES;
    for (uint32_t w = 0; w < 32; ++w) {
        uint32_t x = (w * 37u) % (width - 24u), top = (w * 53u) % (height - 21u);
        for (uint32_t row = 0; row < 21; ++row)
            for (uint32_t col = 0; col < 24; ++col) sum += y[(top + row) * stride + x + col];
    }
    r->windows = VIDEO_BENCH_CYCLES - start;
    r->window_bytes = 32 * 21 * 24;
    sink += sum;
    r->counter_after = VIDEO_CORE_REGISTER(0xE0001000u);
    r->done = 1;
}

static void video_sections_write64(uint8_t *p, uint64_t value) {
    video_profile_write(p, (uint32_t)value);
    video_profile_write(p + 4, (uint32_t)(value >> 32));
}

/* Measuring build. The worker publishes both blocks under the control lock
 * after every decode call; all counters are cumulative for the stream, so a
 * reader takes differences and a missed read loses nothing. */
__attribute__((minsize))
static int video_sections_snapshot_locked(customCfwContext *ctx, uint8_t origin, uint8_t *out) {
    uint32_t generation = ctx->video_control.control_generation;
    /* The header goes out even with a refusal, so a reader can tell this
     * firmware's STALE from older firmware that lacks the page. */
    out[3] = VIDEO_SECTION_DIAGNOSTICS; out[4] = 0; out[5] = 1; out[6] = ctx->video_control.owner_here;
    if (!generation || origin != ctx->video_control.owner_origin) return VIDEO_CONTROL_STALE;
    const video_decode_totals *d = &ctx->video_decode_last;
    const g2_h264_sections_info *s = &ctx->video_sections;
    video_profile_write(out + 8, generation);
    video_profile_write(out + 12, d->pictures);
    video_sections_write64(out + 16, d->cycles);
    video_profile_write(out + 24, d->max_cycles);
    video_profile_write(out + 28, s->pictures);
    const uint64_t cycles[] = {s->intra_i, s->inter_p, s->intra_p, s->completion,
                               s->motion, s->residual, s->transform, s->intra};
    for (uint32_t i = 0; i < 8; ++i) video_sections_write64(out + 32 + i * 8, cycles[i]);
    const uint32_t words[] = {s->motion_calls, s->residual_calls, s->transform_calls, s->intra_calls,
                              s->zero_transforms, s->timer_cost, d->header_cycles};
    for (uint32_t i = 0; i < 7; ++i) video_profile_write(out + 96 + i * 4, words[i]);
    return VIDEO_CONTROL_ACCEPTED;
}

/* Page 1: the time outside the macroblock totals and the stream's memory and
 * CPU check, published by the worker like page 0. */
__attribute__((minsize))
static int video_sections_detail_locked(customCfwContext *ctx, uint8_t origin, uint8_t *out) {
    uint32_t generation = ctx->video_control.control_generation;
    out[3] = VIDEO_SECTION_DIAGNOSTICS; out[4] = 1; out[5] = 1; out[6] = ctx->video_control.owner_here;
    if (!generation || origin != ctx->video_control.owner_origin) return VIDEO_CONTROL_STALE;
    const g2_h264_sections_info *s = &ctx->video_sections;
    const video_bench_result *b = &ctx->video_bench;
    video_profile_write(out + 8, generation);
    video_sections_write64(out + 12, s->slice_setup);
    video_sections_write64(out + 20, s->mb_loop);
    video_sections_write64(out + 28, s->input);
    const uint32_t words[] = {b->done, b->ccr, b->mpu_ctrl, b->frame_address, b->stride, b->width,
                              b->height, b->alu, b->frame_read, b->frame_bytes, b->block_cold,
                              b->block_hot, b->block_bytes, b->windows, b->window_bytes,
                              b->counter_before, b->counter_after};
    for (uint32_t i = 0; i < 17; ++i) video_profile_write(out + 36 + i * 4, words[i]);
    video_sections_write64(out + 104, s->deblock);
    return VIDEO_CONTROL_ACCEPTED;
}
#pragma clang section text=""
