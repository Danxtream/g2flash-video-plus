/* Independent fixed-clock picture consumer. SPDX-License-Identifier: GPL-3.0-only */
#include "presenter.h"

#pragma clang section text=".text.video"

static void video_presenter_wake_locked(video_owner *owner) {
    if (owner && owner->presenter && owner->presenter->wake)
        VIDEO_OS_EVENT_SET(owner->presenter->wake, VIDEO_WAKE_ALL);
}

static void video_presenter_park(video_presenter *p) __attribute__((noreturn));
static void video_presenter_park(video_presenter *p) {
    uint32_t wake = p->wake, complete = p->complete;
    __atomic_store_n(&p->parked, 1, __ATOMIC_RELEASE);
    VIDEO_OS_EVENT_SET(complete, VIDEO_COMPLETE_PARKED);
    for (;;) VIDEO_OS_EVENT_WAIT(wake, VIDEO_WAKE_ALL, 0, VIDEO_WAIT_FOREVER);
}

static void video_presenter_fault(video_owner *owner, uint32_t fault) {
    __atomic_store_n(&owner->context->video.fault, fault, __ATOMIC_RELEASE);
    __atomic_store_n(&owner->context->video.cancel, 1, __ATOMIC_RELEASE);
    VIDEO_OS_EVENT_SET(owner->wake, VIDEO_WAKE_CANCEL);
    video_controller_parked(owner->token);
    video_presenter_park(owner->presenter);
}

static void video_presenter_entry(void *argument) {
    video_presenter *p = argument;
    video_owner *owner = p->decoder;
    customCfwContext *ctx = owner->context;
    int buffering = 1;
    while (!__atomic_load_n(&p->armed, __ATOMIC_ACQUIRE)) {
        if (video_lifecycle_cancelled(&ctx->video, p->token)) video_presenter_park(p);
        VIDEO_OS_EVENT_WAIT(p->wake, VIDEO_WAKE_ALL, 0, VIDEO_WAIT_FOREVER);
    }
    while (!video_lifecycle_cancelled(&ctx->video, p->token)) {
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS))
            video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
        video_decoded_queue *q = &p->queue;
        uint32_t now = VIDEO_TICK, ordinal = p->shown + 1, deadline = now;
        int available = q->count && q->slots[q->head].state == VIDEO_DECODED_READY;
        /* Prebuffer two pictures, except an explicitly timed terminal picture.
         * After starvation re-anchor, never run a catch-up presentation burst. */
        int terminal = available && q->slots[q->head].media.pts + 63 >= ctx->video_control.timeline.duration;
        if (!available) {
            if (!buffering) ++p->holds;
            buffering = 1;
        }
        if (available && buffering && (q->count == q->depth || terminal)) {
            video_pacing_anchor(&p->pacing, p->generation, ordinal, now);
            buffering = 0;
        }
        if (available && !buffering &&
            !video_pacing_deadline(&p->pacing, p->generation, ordinal, &deadline)) {
            video_control_give(ctx);
            video_presenter_fault(owner, VIDEO_FAULT_SEQUENCE);
        }
        video_frame_descriptor frame;
        video_frame_time media;
        int claim = available && !buffering && (int32_t)(now - deadline) >= 0 &&
            video_decoded_claim(q, p->generation, ordinal,
                ctx->video_control.options & VIDEO_PRESENT_NATIVE ? VIDEO_SCALE_NATIVE : VIDEO_SCALE_DOUBLE,
                &frame, &media);
        uint32_t wait = available && !buffering && !claim ? deadline - now : 1000;
        video_control_give(ctx);
        if (!claim) {
            uint32_t result = VIDEO_OS_EVENT_WAIT(p->wake, VIDEO_WAKE_ALL, 0, wait);
            if ((result & VIDEO_EVENT_ERROR) && result != VIDEO_EVENT_TIMEOUT)
                video_presenter_fault(owner, VIDEO_FAULT_EVENT);
            continue;
        }
        if (!video_present_frame(&frame)) {
            if (video_lifecycle_cancelled(&ctx->video, p->token)) break;
            video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
        }
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS))
            video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
        if (owner->evidence) {
            video_evidence *w = owner->evidence;
            if (ordinal != w->high + 1 || w->high - w->acked >= VIDEO_EVIDENCE_ROWS) {
                video_control_give(ctx);
                video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
            }
            video_frame_result *row = &p->evidence[q->head];
            row->ordinal = ordinal; row->copy_tick = ctx->video_presentation.copy_tick;
            w->rows[(ordinal - 1) % VIDEO_EVIDENCE_ROWS] = *row;
            w->high = ordinal;
        }
        int released = video_decoded_release(q, p->generation, ordinal);
        p->shown = ordinal;
        /* Copy/decode overrun holds the cadence instead of showing overdue
         * pictures back-to-back. RIGHT paired coordination extends this later. */
        if ((int32_t)(VIDEO_TICK - deadline) >= 63) {
            ++p->holds;
            video_pacing_anchor(&p->pacing, p->generation, ordinal + 1, VIDEO_TICK + 63);
        }
        VIDEO_OS_EVENT_SET(owner->wake, VIDEO_WAKE_INPUT);
        video_control_give(ctx);
        if (!released) video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
    }
    video_presenter_park(p);
}

static int video_presenter_prepare(video_owner *owner) {
    uint32_t raw_bytes = sizeof(video_presenter) + VIDEO_ALLOC_ALIGNMENT - 1;
    if (!video_bootstrap_room(raw_bytes, owner->storage.display_allowance)) return 0;
    void *raw = VIDEO_OWNER_ALLOC(raw_bytes);
    if (!raw) return 0;
    video_presenter *p = (void *)(((uintptr_t)raw + VIDEO_ALLOC_ALIGNMENT - 1) &
                                 ~(uintptr_t)(VIDEO_ALLOC_ALIGNMENT - 1));
    bzero((uint8_t *)p, sizeof(*p));
    p->raw = raw; p->decoder = owner; p->generation = owner->control_generation; p->token = owner->token;
    owner->presenter = p;
    video_heap_ops heap = {0, video_platform_allocate, video_platform_release, video_platform_heap_view};
    if (!video_storage_init(&p->storage, &heap, video_owner_charge(raw_bytes),
                            owner->storage.display_allowance)) return 0;
    p->storage.limit = VIDEO_PRESENTER_A4_LIMIT;
    p->storage.cached_allowance = 0; /* Input slots already admitted by decoder. */
    p->pixels = video_storage_alloc(&p->storage, VIDEO_DECODED_DEPTH * VIDEO_DECODED_A4_BYTES);
    p->stack_allocation = video_storage_alloc(&p->storage,
        VIDEO_PRESENTER_STACK_BYTES + 2 * VIDEO_STACK_GUARD_BYTES);
    if (!p->pixels || !p->stack_allocation ||
        !video_decoded_init(&p->queue, p->generation, p->token, VIDEO_SOURCE_A4,
                            VIDEO_DECODED_DEPTH, p->pixels, VIDEO_DECODED_DEPTH * VIDEO_DECODED_A4_BYTES)) return 0;
    uint8_t *base = p->stack_allocation;
    for (uint32_t i = 0; i < VIDEO_PRESENTER_STACK_BYTES + 2 * VIDEO_STACK_GUARD_BYTES; ++i)
        base[i] = VIDEO_GUARD_FILL;
    p->stack = base + VIDEO_STACK_GUARD_BYTES;
    for (uint32_t i = 0; i < VIDEO_PRESENTER_STACK_BYTES; ++i) p->stack[i] = VIDEO_STACK_FILL;
    video_event_attr wake = {"video picture wake", 0, p->wake_control, sizeof(p->wake_control)};
    video_event_attr complete = {"video picture complete", 0, p->complete_control, sizeof(p->complete_control)};
    p->wake = VIDEO_OS_EVENT_NEW(&wake); p->complete = VIDEO_OS_EVENT_NEW(&complete);
    if (!p->wake || !p->complete) return 0;
    video_thread_attr attr = {"video presenter", 0, p->tcb, sizeof(p->tcb), p->stack,
                             VIDEO_PRESENTER_STACK_BYTES, 8, 0, 0};
    p->thread = VIDEO_OS_THREAD_NEW(video_presenter_entry, p, &attr);
    if (p->thread && p->tcb[VIDEO_THREAD_STATIC_OFFSET] != VIDEO_THREAD_STATIC_FLAG) {
        __atomic_store_n(&owner->context->video.fault, VIDEO_FAULT_ABI, __ATOMIC_RELEASE);
        __atomic_store_n(&owner->context->video_quarantine_token, owner->token, __ATOMIC_RELEASE);
        return 0;
    }
    return p->thread != 0;
}

static int video_presenter_produce(video_owner *owner, const video_frame_descriptor *frame,
                                    uint32_t nal_sequence) {
    video_presenter *p = owner->presenter;
    customCfwContext *ctx = owner->context;
    video_frame_time media;
    uint32_t serial = 0, slot = 0;
    while (!video_lifecycle_cancelled(&ctx->video, owner->token)) {
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return 0;
        int room = p->queue.count < p->queue.depth;
        int matched = room && video_timeline_take(&ctx->video_control.timeline,
            p->generation, frame->ordinal, nal_sequence, &media);
        if (matched) {
            slot = (p->queue.head + p->queue.count) % p->queue.depth;
            serial = video_decoded_reserve(&p->queue, p->generation, &media);
            if (serial && owner->evidence) {
                p->evidence[slot] = owner->evidence->building;
                owner->evidence->building = (video_frame_result){0};
            }
        }
        video_control_give(ctx);
        if (serial) break;
        VIDEO_OS_EVENT_WAIT(owner->wake, VIDEO_WAKE_ALL, 0, 100);
    }
    if (!serial) return 0;
    int copied = video_decoded_copy(&p->queue, p->generation, serial,
                                    frame->pixels, frame->stride, frame->bytes);
    if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return 0;
    int published = copied && video_decoded_publish(&p->queue, p->generation, serial);
    video_presenter_wake_locked(owner);
    video_control_give(ctx);
    return published;
}

static int video_presenter_stop(video_owner *owner, uint32_t timeout) {
    video_presenter *p = owner->presenter;
    if (!p) return 1;
    if (p->thread) {
        VIDEO_OS_EVENT_SET(p->wake, VIDEO_WAKE_CANCEL);
        if (!__atomic_load_n(&p->parked, __ATOMIC_ACQUIRE) &&
            (VIDEO_OS_EVENT_WAIT(p->complete, VIDEO_COMPLETE_PARKED, 0, timeout) & VIDEO_EVENT_ERROR)) return 0;
        if (VIDEO_OS_THREAD_TERMINATE(p->thread)) return 0;
        p->thread = 0;
    }
    if (!video_platform_event_quiescent(p->wake) || !video_platform_event_quiescent(p->complete)) return 0;
    p->guards_ok = 1;
    if (p->stack) {
        const uint8_t *base = p->stack_allocation;
        for (uint32_t i = 0; i < VIDEO_STACK_GUARD_BYTES; ++i)
            if (base[i] != VIDEO_GUARD_FILL || p->stack[VIDEO_PRESENTER_STACK_BYTES + i] != VIDEO_GUARD_FILL)
                p->guards_ok = 0;
        uint32_t untouched = 0;
        while (untouched < VIDEO_PRESENTER_STACK_BYTES && p->stack[untouched] == VIDEO_STACK_FILL) ++untouched;
        p->stack_used = VIDEO_PRESENTER_STACK_BYTES - untouched;
    }
    return p->guards_ok;
}

static void video_presenter_destroy(video_owner *owner) {
    video_presenter *p = owner->presenter;
    if (!p) return;
    video_storage_reclaim(&p->storage);
    VIDEO_OWNER_FREE(p->raw);
    owner->presenter = 0;
}

#pragma clang section text=""
