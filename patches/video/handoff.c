/* SPDX-License-Identifier: GPL-3.0-only */
#include "handoff.h"
#include "control.h"
#include "present.h"
#include "platform.h"

/* Keep new code out of the callback-bearing section: same-section PIC MOVW
 * addends are signed 16-bit even though the final address is 32-bit. */
#pragma clang section text=".text.video.handoff"

#ifndef VIDEO_HANDOFF_QUEUE
#ifndef VIDEO_HANDOFF_PUT
#define VIDEO_HANDOFF_PUT ((int (*)(uint32_t, const void *, uint8_t, uint32_t))0x00443299U)
#endif
/* The guarded donor creates a 96-entry, copied 36-byte display queue. Avoid
 * the public refresh wrapper's one-second wait and failure diagnostics. */
static int video_handoff_queue(const uint32_t *record) {
    uint32_t queue = *(const volatile uint32_t *)0x20000760U;
    if ((queue & 3u) || queue < 0x20000000U || queue > 0x203fffffU - 68 ||
        *(const volatile uint32_t *)(uintptr_t)(queue + 60) != 96 ||
        *(const volatile uint32_t *)(uintptr_t)(queue + 64) != 36) return 0;
    return VIDEO_HANDOFF_PUT(queue, record, 0, 0) == 0;
}
#define VIDEO_HANDOFF_QUEUE video_handoff_queue
#endif

void video_display_note_output(uint32_t token) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return;
    uint32_t epoch = __atomic_load_n(&ctx->video_output_epoch, __ATOMIC_RELAXED);
    /* Never let wraparound make an old output tag eligible again. */
    if (epoch != UINT32_MAX) ++epoch;
    __atomic_store_n(&ctx->video_output_epoch, epoch, __ATOMIC_RELEASE);
    video_presentation_state *p = &ctx->video_presentation;
    __atomic_store_n(&p->output_epoch,
        token && token == p->token && epoch != UINT32_MAX ? epoch : 0, __ATOMIC_RELEASE);
}

static int video_handoff_eligible(customCfwContext *ctx, uint32_t generation) {
    video_presentation_state *p = &ctx->video_presentation;
    return generation == ctx->video_control.control_generation &&
        p->generation == generation && !p->pin &&
        !ctx->video_control.start_guard && ctx->video.state == VIDEO_IDLE &&
        !ctx->video_owner && !ctx->direct_pending && p->output_epoch &&
        p->output_epoch == ctx->video_output_epoch;
}

int video_handoff_stop(uint32_t generation) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !generation) return 1;
    video_handoff_state *h = &ctx->video_handoff;
    if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return 0;
    uint32_t phase = __atomic_load_n(&h->phase, __ATOMIC_ACQUIRE);
    if (h->generation == generation && phase >= VIDEO_HANDOFF_COMPLETE) {
        video_control_give(ctx); return phase == VIDEO_HANDOFF_COMPLETE ? 1 : -1;
    }
    if (phase != VIDEO_HANDOFF_QUEUED && phase != VIDEO_HANDOFF_COPYING) {
        if (!video_handoff_eligible(ctx, generation)) { video_control_give(ctx); return 1; }
        if (!VIDEO_DISPLAY_TAKE()) {
            ctx->direct_active = 0; /* Known refusal returns ordinary repaint policy. */
            video_control_give(ctx); return -1;
        }
        /* A stock copy can invalidate the output while acquiring the gate. */
        if (!video_handoff_eligible(ctx, generation)) {
            VIDEO_DISPLAY_SIGNAL(); video_control_give(ctx); return 1;
        }
        h->generation = generation; h->output_epoch = ctx->video_output_epoch;
        ctx->direct_active = 0; ctx->direct_shadow = 0; ctx->direct_pending = 1;
        __atomic_store_n(&h->phase, VIDEO_HANDOFF_QUEUED, __ATOMIC_RELEASE);
        video_control_give(ctx);
        /* Words 7/8 are unused by type-3 refresh. Stock wrappers zero both;
         * a nonzero generation in word 7 identifies only this copied job. */
        uint32_t record[VIDEO_HANDOFF_RECORD_WORDS] = {
            3, 0, 0, 0, 0, VIDEO_PANEL_WIDTH, VIDEO_PANEL_HEIGHT, generation, 0};
        if (!VIDEO_HANDOFF_QUEUE(record)) {
            uint32_t expected = VIDEO_HANDOFF_QUEUED;
            if (__atomic_compare_exchange_n(&h->phase, &expected, VIDEO_HANDOFF_FAILED,
                    0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                ctx->direct_pending = 0;
                VIDEO_DISPLAY_SIGNAL(); return -1;
            }
            /* An accepted job may complete before an unusual error return.
             * Once claimed only the stock display task may release its gate. */
        }
    } else {
        video_control_give(ctx);
        if (h->generation != generation) return 0;
    }
    uint32_t deadline = VIDEO_TICK + VIDEO_HANDOFF_LIMIT_MS;
    do {
        phase = __atomic_load_n(&h->phase, __ATOMIC_ACQUIRE);
        if (phase == VIDEO_HANDOFF_COMPLETE) return 1;
        if (phase == VIDEO_HANDOFF_FAILED) return -1;
        VIDEO_OS_DELAY(10);
    } while ((int32_t)(deadline - VIDEO_TICK) > 0);
    /* Unknown queue ownership retains the gate and stable tags. No borrowed
     * buffer is freed, new video is refused, and late completion is safe. */
    return 0;
}

void video_display_restore_copy(const uint32_t *record) {
    customCfwContext *ctx = peekCustomCfwContext();
    video_handoff_state *h = ctx ? &ctx->video_handoff : 0;
    uint32_t expected = VIDEO_HANDOFF_QUEUED;
    if (!h || !record || record[0] != 3 || !h->generation ||
        record[7] != h->generation || record[8] ||
        !__atomic_compare_exchange_n(&h->phase, &expected, VIDEO_HANDOFF_COPYING,
            0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        display_copy_hook(); return;
    }
    uint8_t *fb = FW_DISPLAY_FB;
    /* The stock compositor updates only its 576x288 viewport. Clear the video
     * margins too, then copy the current UI rather than a saved second frame.
     * Type-3 refresh never powers up an inactive driver. */
    if (fb) bzero(fb, VIDEO_PANEL_BYTES);
    FW_DISPLAY_COPY();
    if (fb) {
        uint32_t descriptor[2] = {(uint32_t)(uintptr_t)fb, VIDEO_PANEL_BYTES};
        FW_FLUSH(descriptor);
    }
    video_display_note_output(0);
    ctx->direct_pending = 0;
    __atomic_store_n(&h->phase, fb ? VIDEO_HANDOFF_COMPLETE : VIDEO_HANDOFF_FAILED,
                      __ATOMIC_RELEASE);
    video_controller_request_locked(VIDEO_CONTROLLER_PRESENT);
}

#ifndef VIDEO_HANDOFF_NATIVE
/* At the guarded BL the donor's nine-word queue copy begins at caller SP+12.
 * Tail-call without a new frame so the stack-derived argument stays exact. */
__attribute__((naked)) void video_display_refresh_gate(void) {
    __asm volatile("add r0, sp, #12\n\tb video_display_restore_copy");
}
#endif

#pragma clang section text=""
