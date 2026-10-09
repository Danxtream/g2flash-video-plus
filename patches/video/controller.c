/* Deferred control outside receive/display locks. SPDX-License-Identifier: GPL-3.0-only */
#include "control.h"
#include "platform.h"

#pragma clang section text=".text.video"

static void video_controller_entry(uint32_t, const uint8_t *, uint32_t, uint16_t);

static int video_controller_schedule(customCfwContext *ctx) {
    video_control_state *s = &ctx->video_control;
    if (__atomic_load_n(&s->controller_job, __ATOMIC_ACQUIRE)) return 1;
    uint32_t serial = __atomic_load_n(&s->controller_serial, __ATOMIC_ACQUIRE);
    for (;;) {
        if (serial == UINT32_MAX) return 0;
        uint32_t next = serial + 1;
        if (__atomic_compare_exchange_n(&s->controller_serial, &serial, next, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            serial = next; break;
        }
    }
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&s->controller_job, &expected, serial, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    video_pool_item item = {video_controller_entry, VIDEO_MESSAGE_ID, 0, 0, 0, serial};
    if (VIDEO_POOL_DISPATCH(&item)) return 1;
    expected = serial;
    __atomic_compare_exchange_n(&s->controller_job, &expected, 0, 0,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    return 0;
}

int video_controller_request_locked(uint32_t reasons) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !reasons || reasons & ~(VIDEO_CONTROLLER_START |
        VIDEO_CONTROLLER_STOP | VIDEO_CONTROLLER_REAP)) return 0;
    __atomic_fetch_or(&ctx->video_control.controller_reasons, reasons, __ATOMIC_ACQ_REL);
    return video_controller_schedule(ctx);
}

void video_controller_parked(uint32_t token) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !token || token != __atomic_load_n(&ctx->video.token, __ATOMIC_ACQUIRE)) return;
    __atomic_store_n(&ctx->video_control.controller_park_token, token, __ATOMIC_RELEASE);
    __atomic_fetch_or(&ctx->video_control.controller_reasons, VIDEO_CONTROLLER_REAP,
                       __ATOMIC_ACQ_REL);
    if (!video_controller_schedule(ctx))
        __atomic_store_n(&ctx->video_quarantine_token, token, __ATOMIC_RELEASE);
}

static void video_controller_entry(uint32_t app, const uint8_t *data,
                                    uint32_t serial, uint16_t event) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || app != VIDEO_MESSAGE_ID || data || event || !serial ||
        serial != __atomic_load_n(&ctx->video_control.controller_job, __ATOMIC_ACQUIRE)) return;
    video_control_state *s = &ctx->video_control;
    /* Only this claimed pool callback owns controller waits. Receive handlers
     * coalesce requests without borrowing owner storage or joining a task. */
    for (uint32_t step = 0; step < VIDEO_CONTROL_REPLAYS; ++step) {
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) {
            __atomic_store_n(&s->controller_failed, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&s->controller_reasons, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
            break;
        }
        uint32_t reasons = __atomic_exchange_n(&s->controller_reasons, 0, __ATOMIC_ACQ_REL);
        uint32_t guard = s->start_guard;
        uint32_t parked = __atomic_exchange_n(&s->controller_park_token, 0, __ATOMIC_ACQ_REL);
        video_fold_signals(ctx);
        int current = parked && parked == ctx->video.token;
        if (current) {
            s->start_guard = 0;
            uint32_t fault = __atomic_load_n(&ctx->video.fault, __ATOMIC_ACQUIRE);
            if (fault) s->error = fault >= G2_H264_FAIL_ALLOC && fault <= G2_H264_FAIL_LENGTH ?
                VIDEO_CONTROL_MEMORY : VIDEO_CONTROL_DECODER;
        }
        video_control_give(ctx);
        int stopped = 1;
        if (reasons & VIDEO_CONTROLLER_STOP || current)
            stopped = video_worker_stop(VIDEO_STOP_LIMIT_MS);
        int started = 0;
        if (stopped && guard && reasons & VIDEO_CONTROLLER_START)
            started = video_worker_start_guarded(0, guard);
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) {
            __atomic_store_n(&s->controller_failed, 1, __ATOMIC_RELEASE);
            __atomic_store_n(&s->controller_reasons, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
            break;
        }
        video_fold_signals(ctx);
        if (!stopped || ctx->video.state == VIDEO_QUARANTINED)
            s->error = VIDEO_CONTROL_QUARANTINE;
        else if (guard == s->start_guard && reasons & VIDEO_CONTROLLER_START && !started) {
            s->error = VIDEO_CONTROL_MEMORY;
            s->start_guard = 0;
        }
        if (!__atomic_load_n(&s->controller_reasons, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&s->controller_job, 0, __ATOMIC_RELEASE);
            video_control_give(ctx);
            /* Worker notifications are atomic and need not take this lock.
             * Close the clear-job race without retaining any owner pointer. */
            if (__atomic_load_n(&s->controller_reasons, __ATOMIC_ACQUIRE))
                video_controller_schedule(ctx);
            return;
        }
        video_control_give(ctx);
    }
    __atomic_store_n(&s->controller_job, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&s->controller_reasons, __ATOMIC_ACQUIRE) &&
        !video_controller_schedule(ctx))
        __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
}

#pragma clang section text=""
