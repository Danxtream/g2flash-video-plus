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
        VIDEO_CONTROLLER_STOP | VIDEO_CONTROLLER_REAP | VIDEO_CONTROLLER_LEASE)) return 0;
    __atomic_fetch_or(&ctx->video_control.controller_reasons, reasons, __ATOMIC_ACQ_REL);
    return video_controller_schedule(ctx);
}

void video_control_cancel_locked(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || (!ctx->video_control.start_guard && ctx->video.state == VIDEO_IDLE)) return;
    __atomic_store_n(&ctx->video_control.start_guard, 0, __ATOMIC_RELEASE);
    __atomic_fetch_and(&ctx->video_control.controller_reasons, ~VIDEO_CONTROLLER_START,
                       __ATOMIC_ACQ_REL);
    video_worker_request_stop_locked();
    if (!video_controller_request_locked(VIDEO_CONTROLLER_STOP)) {
        ctx->video_control.error = VIDEO_CONTROL_DISPATCH;
        if (ctx->video.token)
            __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
    }
}
static void video_notify_generation(volatile uint32_t *target, uint32_t generation) {
    uint32_t old = __atomic_load_n(target, __ATOMIC_ACQUIRE);
    while (old < generation && !__atomic_compare_exchange_n(target, &old, generation,
                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
}
void video_control_notify_lease(int released) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return;
    video_control_state *s = &ctx->video_control;
    uint32_t generation = __atomic_load_n(&s->control_generation, __ATOMIC_ACQUIRE);
    if (!generation || !__atomic_load_n(&s->start_guard, __ATOMIC_ACQUIRE)) return;
    /* Tags and the context survive every owner. Never borrow an event/TCB or
     * wait for a publication/heap lock from this notification path. */
    video_notify_generation(released ? &s->notify_release_generation :
                                      &s->notify_lease_generation, generation);
    __atomic_fetch_or(&s->controller_reasons, VIDEO_CONTROLLER_LEASE, __ATOMIC_ACQ_REL);
    if (!video_controller_schedule(ctx))
        video_notify_generation(&s->notify_failed_generation, generation);
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
    video_controller_stack_sample();
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
        uint32_t release = __atomic_exchange_n(&s->notify_release_generation, 0, __ATOMIC_ACQ_REL);
        uint32_t renewal = __atomic_exchange_n(&s->notify_lease_generation, 0, __ATOMIC_ACQ_REL);
        if (release && release == s->control_generation && guard) {
            video_control_cancel_locked();
            reasons = (reasons & ~VIDEO_CONTROLLER_START) | VIDEO_CONTROLLER_STOP;
            guard = 0;
        } else if (renewal && renewal == s->control_generation && guard)
            video_worker_wake_locked(ctx->video.token, VIDEO_WAKE_LEASE);
        if (guard && !video_control_generation_valid(ctx, guard)) {
            __atomic_store_n(&s->start_guard, 0, __ATOMIC_RELEASE); guard = 0;
            s->error = VIDEO_CONTROL_INACTIVITY;
            reasons &= ~VIDEO_CONTROLLER_START;
        }
        uint32_t parked = __atomic_exchange_n(&s->controller_park_token, 0, __ATOMIC_ACQ_REL);
        video_fold_signals(ctx);
        int current = parked && parked == ctx->video.token;
        if (current) {
            __atomic_store_n(&s->start_guard, 0, __ATOMIC_RELEASE);
            uint32_t fault = __atomic_load_n(&ctx->video.fault, __ATOMIC_ACQUIRE);
            if (fault) s->error = video_fault_reason(fault);
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
            s->error = __atomic_load_n(&s->notify_release_generation, __ATOMIC_ACQUIRE) == guard ?
                VIDEO_CONTROL_ACCEPTED : video_control_generation_valid(ctx, guard) ?
                VIDEO_CONTROL_MEMORY : VIDEO_CONTROL_INACTIVITY;
            __atomic_store_n(&s->start_guard, 0, __ATOMIC_RELEASE);
        }
        if (!__atomic_load_n(&s->controller_reasons, __ATOMIC_ACQUIRE)) {
            video_control_give(ctx);
            video_controller_stack_sample();
            if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) {
                __atomic_store_n(&s->controller_failed, 1, __ATOMIC_RELEASE);
                break;
            }
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
    video_controller_stack_sample();
    __atomic_store_n(&s->controller_job, 0, __ATOMIC_RELEASE);
    if (__atomic_load_n(&s->controller_reasons, __ATOMIC_ACQUIRE) &&
        !video_controller_schedule(ctx))
        __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
}

#pragma clang section text=""
