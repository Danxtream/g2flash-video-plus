/* Tagged shared-shadow display lifetime. SPDX-License-Identifier: GPL-3.0-only */
#include "present.h"
#include "storage.h"
#include "worker.h"

#pragma clang section text=".text.video"

#ifndef VIDEO_DISPLAY_TAKE
/* The donor's void display-wait wrapper discards a semaphore timeout. Call
 * its authenticated underlying take and retain the actual ownership result. */
static int video_platform_display_take(void) {
    uint32_t gate = *(const volatile uint32_t *)0x2007698cU;
    return gate && ((int (*)(uint32_t, uint32_t))0x00442389U)(gate, 1000) == 1;
}
#define VIDEO_DISPLAY_TAKE video_platform_display_take
#define VIDEO_DISPLAY_SIGNAL FW_DISPLAY_SIGNAL
#define VIDEO_SHADOW_ALLOC cfw_heap13_malloc
#define VIDEO_SHADOW_FREE cfw_heap13_free
#endif

static int video_shadow_room(uint32_t allowance, uint32_t allocation) {
    video_heap_view view;
    uint32_t charge = allocation ? ((allocation + 3u) & ~3u) + 4u : 0;
    return allowance <= UINT32_MAX - VIDEO_DISPLAY_RESERVE &&
        video_platform_heap_view(0, VIDEO_HEAP_DISPLAY, &view) &&
        view.free_bytes != UINT32_MAX && view.max_alloc <= view.free_bytes &&
        view.free_bytes >= VIDEO_DISPLAY_RESERVE + allowance &&
        charge <= view.free_bytes - VIDEO_DISPLAY_RESERVE - allowance &&
        (!allocation || allocation <= view.max_alloc);
}

static int video_prepare_shadow(video_owner *owner) {
    customCfwContext *ctx = owner->context;
    if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return 0;
    int present = ctx->framebuffer_shadow != 0;
    int eligible = video_lease_valid(ctx) && !video_lifecycle_cancelled(&ctx->video, owner->token);
    video_control_give(ctx);
    if (!eligible || present) return eligible;
    if (!video_shadow_room(owner->ingress_allowance, VIDEO_PANEL_BYTES)) return 0;
    uint8_t *candidate = VIDEO_SHADOW_ALLOC(VIDEO_PANEL_BYTES);
    if (!candidate) return 0;
    bzero(candidate, VIDEO_PANEL_BYTES);
    int room = video_shadow_room(owner->ingress_allowance, 0);
    int published = 0;
    if (video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) {
        if (room && ctx->video_owner == owner && video_lease_valid(ctx) &&
            !video_lifecycle_cancelled(&ctx->video, owner->token) && !ctx->framebuffer_shadow) {
            __atomic_store_n(&ctx->framebuffer_shadow, candidate, __ATOMIC_RELEASE);
            owner->shadow_missing = 0;
            owner->storage.display_allowance = owner->ingress_allowance;
            published = 1;
        }
        video_control_give(ctx);
    }
    if (!published) VIDEO_SHADOW_FREE(candidate);
    return published;
}

uint32_t video_display_claim(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || ctx->direct_shadow != ctx->framebuffer_shadow) return 0;
    uint32_t expected = VIDEO_DISPLAY_QUEUED;
    return __atomic_compare_exchange_n(&ctx->video_presentation.phase, &expected,
        VIDEO_DISPLAY_COPYING, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ?
        ctx->video_presentation.token : 0;
}

void video_display_complete(uint32_t token, int success) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !token || token != ctx->video_presentation.token ||
        __atomic_load_n(&ctx->video_presentation.phase, __ATOMIC_ACQUIRE) != VIDEO_DISPLAY_COPYING) return;
    ctx->video_presentation.copy_tick = VIDEO_TICK;
    __atomic_store_n(&ctx->video_presentation.phase,
        success ? VIDEO_DISPLAY_COMPLETE : VIDEO_DISPLAY_FAILED, __ATOMIC_RELEASE);
    video_controller_presented(token);
}

int video_display_queue_failed(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !ctx->video_presentation.pin) return 1;
    __atomic_store_n(&ctx->video_presentation.queue_failed, 1, __ATOMIC_RELEASE);
    uint32_t expected = VIDEO_DISPLAY_QUEUED;
    int unclaimed = __atomic_compare_exchange_n(&ctx->video_presentation.phase, &expected,
        VIDEO_DISPLAY_FAILED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    video_controller_presented(ctx->video_presentation.token);
    return unclaimed;
}

int video_display_fold_locked(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return 0;
    video_presentation_state *p = &ctx->video_presentation;
    uint32_t phase = __atomic_load_n(&p->phase, __ATOMIC_ACQUIRE);
    if (!p->pin || (phase != VIDEO_DISPLAY_COMPLETE && phase != VIDEO_DISPLAY_FAILED)) return p->pin != 0;
    int ok = phase == VIDEO_DISPLAY_COMPLETE && !__atomic_load_n(&p->queue_failed, __ATOMIC_ACQUIRE);
    if (p->token != ctx->video.token || !video_worker_unpin_locked(p->token, 0)) {
        __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
        return 1;
    }
    p->pin = 0;
    if (ok && p->ordinal == p->presented + 1) p->presented = p->ordinal;
    else {
        ++p->failures;
        video_fault_locked(ctx, VIDEO_FAULT_DISPLAY);
    }
    __atomic_store_n(&p->phase, VIDEO_DISPLAY_IDLE, __ATOMIC_RELEASE);
    return 0;
}

int video_present_frame(const video_frame_descriptor *frame) {
    video_owner *owner = video_current_owner();
    if (!owner || !frame || owner->thread != VIDEO_OS_THREAD_ID() ||
        frame->token != owner->token || frame->generation != owner->control_generation ||
        !video_prepare_shadow(owner)) return 0;
    if (owner->control_generation && !video_power_deadline(owner->control_generation))
        video_runtime_fail(VIDEO_FAULT_POWER);
    customCfwContext *ctx = owner->context;
    if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return 0;
    video_presentation_state *p = &ctx->video_presentation;
    if (p->pin || ctx->direct_pending || !ctx->framebuffer_shadow || !video_lease_valid(ctx) ||
        video_lifecycle_cancelled(&ctx->video, frame->token)) {
        video_control_give(ctx); return 0;
    }
    if (!VIDEO_DISPLAY_TAKE()) { video_control_give(ctx); return 0; }
    if (!video_lease_valid(ctx) || video_lifecycle_cancelled(&ctx->video, frame->token) ||
        !video_pack_frame(ctx->framebuffer_shadow, VIDEO_PANEL_BYTES, frame) ||
        !video_lifecycle_pin(&ctx->video, frame->token, 0)) {
        VIDEO_DISPLAY_SIGNAL(); video_control_give(ctx); return 0;
    }
    p->token = frame->token; p->generation = frame->generation;
    p->ordinal = frame->ordinal; p->copy_tick = 0; p->pin = 1;
    __atomic_store_n(&p->queue_failed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&p->phase, VIDEO_DISPLAY_QUEUED, __ATOMIC_RELEASE);
    cfw_rectlist rectangles = {0};
    present_shadow(VIDEO_PANEL_WIDTH, VIDEO_PANEL_HEIGHT, &rectangles);
    if (!rectangles.direct_submitted) VIDEO_DISPLAY_SIGNAL();
    video_control_give(ctx);
    uint32_t deadline = VIDEO_TICK + VIDEO_DISPLAY_LIMIT_MS;
    for (;;) {
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) break;
        int done = !p->pin && p->ordinal == frame->ordinal;
        int success = done && p->presented == frame->ordinal && !rectangles.direct_failed;
        video_control_give(ctx);
        if (done) return success;
        uint32_t now = VIDEO_TICK;
        if ((int32_t)(deadline - now) <= 0) break;
        uint32_t wake = VIDEO_OS_EVENT_WAIT(owner->wake, VIDEO_WAKE_ALL, 0, deadline - now);
        if ((wake & VIDEO_EVENT_ERROR) && wake != VIDEO_EVENT_TIMEOUT) break;
    }
    /* A submitted pointer may still be live in the stock queue or copier.
     * Never clear it, release its gate or free storage after an unknown timeout. */
    __atomic_store_n(&ctx->video_quarantine_token, owner->token, __ATOMIC_RELEASE);
    video_runtime_fail(VIDEO_FAULT_DISPLAY);
}

#pragma clang section text=""
