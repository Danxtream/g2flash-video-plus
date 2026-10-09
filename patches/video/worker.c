/* SPDX-License-Identifier: GPL-3.0-only */
#include "worker.h"
#include "storage.h"
#include "lifecycle.h"
#include "queue.h"
#include "../cfw_context.h"
#include "../h264/g2_h264.h"
#include "platform.h"

/* Keep the new worker out of the original C text section. Otherwise clang's
 * same-section MOVW address folding overflows for distant stock callbacks.
 * This preserves the C flags and lets the linker resolve cross-section PIC. */
#pragma clang section text=".text.video"

typedef struct {
    customCfwContext *context;
    uint32_t token, thread, ingress_allowance, shadow_missing;
    uint32_t wake, complete;
    uint32_t control_generation, extension_attempted;
    volatile uint32_t armed;
    void *raw, *object_allocation, *object, *decoder, *stack_allocation;
    uint8_t *stack;
    uint8_t tcb[VIDEO_THREAD_CONTROL_BYTES] __attribute__((aligned(8)));
    uint8_t wake_control[VIDEO_EVENT_CONTROL_BYTES] __attribute__((aligned(8)));
    uint8_t complete_control[VIDEO_EVENT_CONTROL_BYTES] __attribute__((aligned(8)));
    video_nal_queue queue;
    video_storage storage;
    g2_h264_runtime runtime;
} video_owner;

static video_owner *video_current_owner(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    return ctx ? __atomic_load_n((video_owner **)&ctx->video_owner, __ATOMIC_ACQUIRE) : 0;
}

static int video_control_take(customCfwContext *ctx) {
    return ctx && ctx->image_mutex && VIDEO_OS_MUTEX_TAKE(ctx->image_mutex, 0) == 0;
}
static void video_control_give(customCfwContext *ctx) {
    VIDEO_OS_MUTEX_GIVE(ctx->image_mutex);
}
static int video_control_wait(customCfwContext *ctx, uint32_t timeout) {
    return ctx && ctx->image_mutex && VIDEO_OS_MUTEX_TAKE(ctx->image_mutex, timeout) == 0;
}
static int video_lease_valid(customCfwContext *ctx) {
    uint32_t deadline = __atomic_load_n(&ctx->direct_lease_deadline, __ATOMIC_ACQUIRE);
    return deadline && (int32_t)(deadline - VIDEO_TICK) > 0;
}

static uint32_t video_fault_reason(uint32_t fault) {
    switch (fault) {
    case VIDEO_FAULT_GAP: return VIDEO_CONTROL_GAP;
    case VIDEO_FAULT_INACTIVITY: return VIDEO_CONTROL_INACTIVITY;
    case VIDEO_FAULT_FORMAT: return VIDEO_CONTROL_INPUT;
    case VIDEO_FAULT_CONFLICT: return VIDEO_CONTROL_CONFLICT;
    case VIDEO_FAULT_SEQUENCE: return VIDEO_CONTROL_SEQUENCE;
    default: return fault >= G2_H264_FAIL_ALLOC && fault <= G2_H264_FAIL_LENGTH ?
        VIDEO_CONTROL_MEMORY : VIDEO_CONTROL_DECODER;
    }
}
static int video_control_generation_valid(customCfwContext *ctx, uint32_t generation) {
    uint32_t deadline = __atomic_load_n(&ctx->video_control.active_deadline, __ATOMIC_ACQUIRE);
    return !generation || (ctx->video_control.start_guard == generation && deadline &&
                           (int32_t)(deadline - VIDEO_TICK) > 0);
}
static void video_fault_locked(customCfwContext *ctx, uint32_t fault) {
    ctx->video_control.start_guard = 0;
    ctx->video_control.error = video_fault_reason(fault);
    __atomic_fetch_and(&ctx->video_control.controller_reasons, ~VIDEO_CONTROLLER_START,
                       __ATOMIC_ACQ_REL);
    if (ctx->video.state != VIDEO_IDLE) {
        __atomic_store_n(&ctx->video.fault, fault, __ATOMIC_RELEASE);
        video_worker_request_stop_locked();
    }
    if (!video_controller_request_locked(VIDEO_CONTROLLER_STOP) && ctx->video.token)
        __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
}

static void video_park_owner(video_owner *owner, uint32_t fault) __attribute__((noreturn));
static void video_park_owner(video_owner *owner, uint32_t fault) {
    uint32_t wake = owner->wake, complete = owner->complete;
    uint32_t token = owner->token, controlled = owner->control_generation;
    video_lifecycle_park(&owner->context->video, owner->token, fault);
    /* Do not dereference owner again. The controller terminates this different
     * static task before freeing its TCB/stack, including this suspended frame. */
    VIDEO_OS_EVENT_SET(complete, VIDEO_COMPLETE_PARKED);
    if (controlled) video_controller_parked(token);
    for (;;) VIDEO_OS_EVENT_WAIT(wake, VIDEO_WAKE_ALL, 0, VIDEO_WAIT_FOREVER);
}
static void video_runtime_fail(uint32_t fault) __attribute__((noreturn));
static void video_runtime_fail(uint32_t fault) {
    video_owner *owner = video_current_owner();
    if (!owner) __builtin_trap();
    video_park_owner(owner, fault);
}
static void *video_runtime_alloc(uint32_t size) {
    video_owner *owner = video_current_owner();
    return owner ? video_storage_alloc(&owner->storage, size) : 0;
}
static void video_runtime_release(void *memory) {
    video_owner *owner = video_current_owner();
    if (!owner || !video_storage_release(&owner->storage, memory))
        video_runtime_fail(VIDEO_FAULT_OWNERSHIP);
}
static int video_runtime_preflight(const g2_h264_request *requests, uint32_t count,
                                    g2_h264_failure *failure) {
    video_owner *owner = video_current_owner();
    return owner && video_storage_preflight(&owner->storage, requests, count, failure);
}

static void video_worker_entry(void *argument) {
    video_owner *owner = argument;
    customCfwContext *ctx = owner->context;
    uint32_t token = owner->token;
    while (!__atomic_load_n(&owner->armed, __ATOMIC_ACQUIRE)) {
        if (video_lifecycle_cancelled(&ctx->video, token)) video_park_owner(owner, 0);
        VIDEO_OS_EVENT_WAIT(owner->wake, VIDEO_WAKE_ALL, 0, VIDEO_WAIT_FOREVER);
    }
    if (!video_lifecycle_cancelled(&ctx->video, token)) {
        owner->decoder = g2_h264_init(owner->object, g2_h264_size());
        if (!owner->decoder) video_runtime_fail(VIDEO_FAULT_INIT);
        video_storage_finish_call(&owner->storage);
        video_lifecycle_ready(&ctx->video, token);
    }
    /* No NAL consumer or presenter yet. A future worker command loop must
     * finish each decoder call and drain output pins before its next call. */
    while (!video_lifecycle_cancelled(&ctx->video, token)) {
        uint32_t now = VIDEO_TICK;
        uint32_t deadline = __atomic_load_n(&ctx->direct_lease_deadline, __ATOMIC_ACQUIRE);
        if (!deadline || (int32_t)(deadline - now) <= 0) {
            __atomic_store_n(&ctx->video.cancel, 1, __ATOMIC_RELEASE);
            break;
        }
        uint32_t timeout = deadline - now;
        if (owner->control_generation) {
            uint32_t activity = __atomic_load_n(&ctx->video_control.active_deadline, __ATOMIC_ACQUIRE);
            uint32_t gap = __atomic_load_n(&owner->queue.gap_deadline, __ATOMIC_ACQUIRE);
            uint32_t fault = !activity || (int32_t)(activity - now) <= 0 ?
                VIDEO_FAULT_INACTIVITY : gap && (int32_t)(gap - now) <= 0 ? VIDEO_FAULT_GAP : 0;
            if (fault) {
                __atomic_store_n(&ctx->video.fault, fault, __ATOMIC_RELEASE);
                __atomic_store_n(&ctx->video.cancel, 1, __ATOMIC_RELEASE);
                break;
            }
            if (activity - now < timeout) timeout = activity - now;
            if (gap && gap - now < timeout) timeout = gap - now;
        }
        /* Sticky wake bits close the predicate-to-wait race. Only the nearest
         * actual lease, gap or activity deadline bounds this event wait. */
        uint32_t result = VIDEO_OS_EVENT_WAIT(owner->wake, VIDEO_WAKE_ALL, 0, timeout);
        if ((result & VIDEO_EVENT_ERROR) && result != VIDEO_EVENT_TIMEOUT)
            video_runtime_fail(VIDEO_FAULT_EVENT);
    }
    while (__atomic_load_n(&ctx->video.output_pins, __ATOMIC_ACQUIRE) ||
           __atomic_load_n(&ctx->video.callback_users, __ATOMIC_ACQUIRE))
        VIDEO_OS_EVENT_WAIT(owner->wake, VIDEO_WAKE_ALL, 0, VIDEO_WAIT_FOREVER);
    video_storage_finish_call(&owner->storage);
    if (owner->decoder) {
        g2_h264_destroy(owner->decoder);
        owner->decoder = 0;
    }
    video_park_owner(owner, 0);
}

static uint32_t video_owner_charge(uint32_t size) {
    return ((size + 3u) & ~3u) + 4u; /* stock TLSF payload rounding/header */
}
static int video_bootstrap_room(uint32_t raw, uint32_t display_allowance) {
    const uint32_t heaps[] = {VIDEO_HEAP_CACHED, VIDEO_HEAP_DISPLAY, VIDEO_HEAP_OTHER};
    const uint32_t keep[] = {VIDEO_CACHED_RESERVE + VIDEO_INPUT_ALLOWANCE,
                            VIDEO_DISPLAY_RESERVE, VIDEO_OTHER_RESERVE};
    for (uint32_t i = 0; i < 3; ++i) {
        video_heap_view view;
        if (!video_platform_heap_view(0, heaps[i], &view) ||
            view.max_alloc > view.free_bytes || view.free_bytes == UINT32_MAX) return 0;
        uint32_t extra = i == 0 ? video_owner_charge(raw) : i == 1 ? display_allowance : 0;
        if (extra > UINT32_MAX - keep[i] || view.free_bytes < keep[i] + extra ||
            (i == 0 && view.max_alloc < raw)) return 0;
    }
    return 1;
}

static void video_fill_stack(video_owner *owner) {
    uint8_t *base = owner->stack_allocation;
    for (uint32_t i = 0; i < VIDEO_WORKER_STACK_BYTES + 2 * VIDEO_STACK_GUARD_BYTES; ++i)
        base[i] = VIDEO_GUARD_FILL;
    owner->stack = base + VIDEO_STACK_GUARD_BYTES;
    for (uint32_t i = 0; i < VIDEO_WORKER_STACK_BYTES; ++i) owner->stack[i] = VIDEO_STACK_FILL;
}
static video_worker_report video_completed_report(video_owner *owner) {
    video_worker_report report = {0};
    report.valid = 1;
    report.token = owner->token;
    report.fault = __atomic_load_n(&owner->context->video.fault, __ATOMIC_ACQUIRE);
    report.storage_peak = owner->storage.peak;
    report.storage_left = owner->storage.used;
    report.object_bytes = g2_h264_size();
    report.object_alignment = g2_h264_alignment();
    report.stack_bytes = VIDEO_WORKER_STACK_BYTES;
    report.guards_ok = 1;
    if (owner->stack) {
        uint8_t *base = owner->stack_allocation;
        for (uint32_t i = 0; i < VIDEO_STACK_GUARD_BYTES; ++i)
            if (base[i] != VIDEO_GUARD_FILL ||
                owner->stack[VIDEO_WORKER_STACK_BYTES + i] != VIDEO_GUARD_FILL)
                report.guards_ok = 0;
        uint32_t untouched = 0;
        while (untouched < VIDEO_WORKER_STACK_BYTES &&
               owner->stack[untouched] == VIDEO_STACK_FILL) ++untouched;
        report.stack_used = VIDEO_WORKER_STACK_BYTES - untouched;
    }
    const uint32_t heaps[] = {VIDEO_HEAP_CACHED, VIDEO_HEAP_DISPLAY, VIDEO_HEAP_OTHER};
    for (uint32_t i = 0; i < 3; ++i) {
        video_heap_view view = {UINT32_MAX, UINT32_MAX};
        video_platform_heap_view(0, heaps[i], &view);
        report.free_bytes[i] = view.free_bytes;
        report.max_alloc[i] = view.max_alloc;
    }
    return report;
}

/* Startup owns private allocations until publication. An abort acknowledgement
 * is stored in the stable context if its short publication lock is unavailable;
 * a later controller folds it under that lock, without losing the raw owner. */
static void video_fold_signals(customCfwContext *ctx) {
    uint32_t token = __atomic_exchange_n(&ctx->video_abort_token, 0, __ATOMIC_ACQ_REL);
    if (token && token == ctx->video.token && ctx->video.preparing) {
        video_lifecycle_abort_start(&ctx->video, token,
            ctx->video_owner && ((video_owner *)ctx->video_owner)->thread != 0);
        video_owner *owner = ctx->video_owner;
        if (owner) {
            VIDEO_OS_EVENT_SET(owner->wake, VIDEO_WAKE_CANCEL);
            VIDEO_OS_EVENT_SET(owner->complete, VIDEO_COMPLETE_PREPARED);
        }
    }
    token = __atomic_exchange_n(&ctx->video_quarantine_token, 0, __ATOMIC_ACQ_REL);
    if (token && token == ctx->video.token) {
        video_lifecycle_quarantine(&ctx->video);
        ctx->video_reaper = 0;
        ctx->video_last_report = (video_worker_report){0};
        ctx->video_last_report.state = VIDEO_QUARANTINED;
        ctx->video_last_report.token = token;
        ctx->video_last_report.fault = __atomic_load_n(&ctx->video.fault, __ATOMIC_ACQUIRE);
    }
    token = __atomic_exchange_n(&ctx->video_reclaimed_token, 0, __ATOMIC_ACQ_REL);
    if (token && token == ctx->video.token && video_lifecycle_reclaimed(&ctx->video)) {
        ctx->video_reaper = 0;
        ctx->video_last_report.state = VIDEO_IDLE;
        ctx->video_last_report.storage_left = 0;
    }
}
static void video_free_owner(video_owner *owner) {
    void *raw = owner->raw;
    video_storage_reclaim(&owner->storage);
    VIDEO_OWNER_FREE(raw);
}
static int video_stop_token(uint32_t token, uint32_t timeout_ms);
static void video_abort_start(customCfwContext *ctx, uint32_t token) {
    __atomic_store_n(&ctx->video.cancel, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->video_abort_token, token, __ATOMIC_RELEASE);
    /* The completion handle was published with the private claim, before any
     * decoder allocation. STOP can therefore wait without polling startup. */
    /* Folding wakes under the publication lock; a sender outside that lock
     * could otherwise still be returning from SET when another task frees it. */
    video_stop_token(token, VIDEO_STOP_LIMIT_MS);
}

int video_worker_ensure_mutex(void) {
    customCfwContext *ctx = getCustomCfwContext();
    if (!ctx) return 0;
    if (!ctx->image_mutex) {
        uint32_t mutex = VIDEO_OS_MUTEX_NEW(0);
        if (!mutex) return 0;
        uint32_t expected = 0;
        if (!__atomic_compare_exchange_n(&ctx->image_mutex, &expected, mutex,
                                         0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            VIDEO_OS_MUTEX_DELETE(mutex);
    }
    return 1;
}

int video_worker_start_guarded(uint32_t ingress_allowance, uint32_t generation) {
    if (!video_worker_ensure_mutex()) return 0;
    customCfwContext *ctx = peekCustomCfwContext();
    if (!video_control_take(ctx)) return 0;
    video_fold_signals(ctx);
    int eligible = !ctx->texture_cache && video_lease_valid(ctx) &&
                   ctx->video.state == VIDEO_IDLE &&
                   video_control_generation_valid(ctx, generation);
    uint32_t missing = ctx->framebuffer_shadow == 0;
    video_control_give(ctx);
    if (!eligible) return 0;
    uint32_t shadow = missing ? VIDEO_SHADOW_BYTES : 0;
    if (ingress_allowance > UINT32_MAX - shadow ||
        !video_bootstrap_room(sizeof(video_owner) + VIDEO_ALLOC_ALIGNMENT - 1u,
                              shadow + ingress_allowance)) {
        return 0;
    }
    uint32_t raw_bytes = sizeof(video_owner) + VIDEO_ALLOC_ALIGNMENT - 1u;
    void *raw = VIDEO_OWNER_ALLOC(raw_bytes);
    if (!raw) return 0;
    video_owner *owner = (void *)(((uintptr_t)raw + VIDEO_ALLOC_ALIGNMENT - 1u) &
                                 ~(uintptr_t)(VIDEO_ALLOC_ALIGNMENT - 1u));
    bzero((uint8_t *)owner, sizeof(*owner));
    owner->raw = raw; owner->context = ctx;
    owner->control_generation = generation;
    owner->ingress_allowance = ingress_allowance; owner->shadow_missing = missing;
    video_heap_ops heap = {0, video_platform_allocate, video_platform_release,
                          video_platform_heap_view};
    if (!video_storage_init(&owner->storage, &heap, video_owner_charge(raw_bytes),
                             shadow + ingress_allowance)) {
        VIDEO_OWNER_FREE(raw); return 0;
    }
    video_event_attr event = {"video-wake", 0, owner->wake_control,
                              sizeof(owner->wake_control)};
    owner->wake = VIDEO_OS_EVENT_NEW(&event);
    event.name = "video-complete";
    event.cb_mem = owner->complete_control;
    owner->complete = owner->wake ? VIDEO_OS_EVENT_NEW(&event) : 0;
    if (!owner->wake || !owner->complete) { video_free_owner(owner); return 0; }
    if (!video_control_take(ctx)) { video_free_owner(owner); return 0; }
    video_fold_signals(ctx);
    uint32_t token = !ctx->texture_cache && video_lease_valid(ctx) &&
        video_control_generation_valid(ctx, generation) &&
        (ctx->framebuffer_shadow == 0) == missing ? video_lifecycle_claim(&ctx->video) : 0;
    if (token) {
        owner->token = token;
        video_queue_init(&owner->queue, token);
        ctx->video_last_report = (video_worker_report){0};
        __atomic_store_n(&ctx->video_owner, owner, __ATOMIC_RELEASE);
    }
    video_control_give(ctx);
    if (!token) { video_free_owner(owner); return 0; }
    uint32_t size = g2_h264_size(), alignment = g2_h264_alignment();
    if (!size || !alignment || alignment & (alignment - 1u) ||
        size > UINT32_MAX - alignment) { video_abort_start(ctx, token); return 0; }
    owner->object_allocation = video_storage_alloc(&owner->storage, size + alignment - 1u);
    owner->stack_allocation = video_storage_alloc(&owner->storage,
                                 VIDEO_WORKER_STACK_BYTES + 2 * VIDEO_STACK_GUARD_BYTES);
    if (!owner->object_allocation || !owner->stack_allocation ||
        video_lifecycle_cancelled(&ctx->video, token)) {
        video_abort_start(ctx, token); return 0;
    }
    owner->object = (void *)(((uintptr_t)owner->object_allocation + alignment - 1u) &
                             ~(uintptr_t)(alignment - 1u));
    if (generation) {
        owner->storage.limit += VIDEO_INPUT_ALLOWANCE;
        for (uint32_t i = 0; i < VIDEO_QUEUE_INITIAL; ++i) {
            /* Replace the outstanding payload allowance with each actual
             * ledger charge, including alignment and stock TLSF overhead. */
            owner->storage.cached_allowance -= VIDEO_SLOT_BYTES;
            uint8_t *bytes = video_storage_alloc(&owner->storage, VIDEO_SLOT_BYTES);
            if (!bytes || !video_queue_attach(&owner->queue, bytes) ||
                video_lifecycle_cancelled(&ctx->video, token)) {
                video_abort_start(ctx, token); return 0;
            }
        }
    }
    video_fill_stack(owner);
    owner->runtime = (g2_h264_runtime){video_runtime_alloc, video_runtime_release,
                                      video_runtime_preflight, video_runtime_fail};
    video_thread_attr attributes = {"video-decoder", 0, owner->tcb, sizeof(owner->tcb),
        owner->stack, VIDEO_WORKER_STACK_BYTES, VIDEO_WORKER_PRIORITY, 0, 0};
    owner->thread = VIDEO_OS_THREAD_NEW(video_worker_entry, owner, &attributes);
    if (!owner->thread) { video_abort_start(ctx, token); return 0; }
    /* xTaskCreateStatic marks caller-owned TCB/stack; never reclaim an unknown ABI. */
    if (owner->tcb[VIDEO_THREAD_STATIC_OFFSET] != VIDEO_THREAD_STATIC_FLAG) {
        __atomic_store_n(&ctx->video.fault, VIDEO_FAULT_ABI, __ATOMIC_RELEASE);
        __atomic_store_n(&ctx->video_quarantine_token, token, __ATOMIC_RELEASE);
        video_abort_start(ctx, token); return 0;
    }
    int admitted = video_storage_admit(&owner->storage);
    if (!video_control_take(ctx)) { video_abort_start(ctx, token); return 0; }
    if (!admitted || ctx->texture_cache || !video_lease_valid(ctx) ||
        !video_control_generation_valid(ctx, generation) ||
        (ctx->framebuffer_shadow == 0) != missing ||
        !video_lifecycle_publish(&ctx->video, token)) {
        video_control_give(ctx); video_abort_start(ctx, token); return 0;
    }
    __atomic_store_n(&ctx->video_owner, owner, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->video_runtime, &owner->runtime, __ATOMIC_RELEASE);
    __atomic_store_n(&owner->armed, 1, __ATOMIC_RELEASE);
    VIDEO_OS_EVENT_SET(owner->complete, VIDEO_COMPLETE_PREPARED);
    VIDEO_OS_EVENT_SET(owner->wake, VIDEO_WAKE_ARM);
    video_control_give(ctx);
    return 1;
}

int video_worker_receive_nal_locked(uint32_t stream, uint32_t sequence,
                                      const uint8_t *p, uint16_t n, uint8_t origin) {
    customCfwContext *ctx = peekCustomCfwContext();
    video_owner *owner = ctx ? ctx->video_owner : 0;
    if (!owner || !p || !n || n > VIDEO_RAW_NAL_BYTES || !video_lease_valid(ctx) ||
        !owner->control_generation || origin != ctx->video_control.owner_origin ||
        stream != ctx->video_control.stream ||
        ctx->video_control.start_guard != owner->control_generation ||
        video_lifecycle_state(&ctx->video) != VIDEO_READY ||
        video_lifecycle_cancelled(&ctx->video, owner->token)) return 0;
    uint32_t now = VIDEO_TICK;
    uint32_t activity = __atomic_load_n(&ctx->video_control.active_deadline, __ATOMIC_ACQUIRE);
    uint32_t fault = !activity || (int32_t)(activity - now) <= 0 ? VIDEO_FAULT_INACTIVITY :
        !video_queue_watch(&owner->queue, now) ? VIDEO_FAULT_GAP :
        (p[0] & 0x80) || !(p[0] & 31) || (p[0] & 31) > 23 ? VIDEO_FAULT_FORMAT : 0;
    int copied = fault ? 0 : video_queue_push(&owner->queue, sequence, p, n);
    if (copied == VIDEO_QUEUE_CONFLICT) fault = VIDEO_FAULT_CONFLICT;
    if (copied == VIDEO_QUEUE_WRAP) fault = VIDEO_FAULT_SEQUENCE;
    if (copied > 0 && !video_queue_headers(&owner->queue)) fault = VIDEO_FAULT_FORMAT;
    if (copied > 0 && !video_queue_watch(&owner->queue, VIDEO_TICK)) fault = VIDEO_FAULT_GAP;
    if (fault) { video_fault_locked(ctx, fault); return 0; }
    if (copied > 0) {
        uint32_t deadline = now + VIDEO_INACTIVITY_LIMIT_MS;
        __atomic_store_n(&ctx->video_control.active_deadline, deadline ? deadline : 1, __ATOMIC_RELEASE);
        video_worker_wake_locked(owner->token, VIDEO_WAKE_INPUT);
    }
    return copied;
}

int video_worker_picture_complete(uint32_t token, int dpb_full) {
    video_owner *owner = video_current_owner();
    if (!owner || token != owner->token || !owner->control_generation || !owner->thread ||
        owner->thread != VIDEO_OS_THREAD_ID()) return 0;
    customCfwContext *ctx = owner->context;
    if (!video_control_take(ctx)) return 0;
    if (video_lifecycle_state(&ctx->video) != VIDEO_READY ||
        video_lifecycle_cancelled(&ctx->video, token) || owner->queue.pictures == UINT32_MAX) {
        video_control_give(ctx); return 0;
    }
    ++owner->queue.pictures;
    int attempt = dpb_full && owner->queue.capacity == VIDEO_QUEUE_INITIAL &&
                  !owner->extension_attempted;
    if (attempt) owner->extension_attempted = 1;
    video_control_give(ctx);
    if (!attempt) return 1;
    uint32_t original_limit = owner->storage.limit;
    owner->storage.limit += (VIDEO_QUEUE_MAX - VIDEO_QUEUE_INITIAL) * VIDEO_SLOT_BYTES;
    /* The caller is the worker at a decoder boundary. It cannot acknowledge
     * park or be reclaimed while its own ledger allocation is in progress. */
    uint8_t *bytes = video_storage_alloc(&owner->storage,
        (VIDEO_QUEUE_MAX - VIDEO_QUEUE_INITIAL) * VIDEO_SLOT_BYTES);
    int published = 0;
    if (bytes && video_control_take(ctx)) {
        if (!video_lifecycle_cancelled(&ctx->video, token) && video_lease_valid(ctx))
            published = video_queue_extend(&owner->queue, bytes);
        video_control_give(ctx);
    }
    if (!published) {
        if (bytes && !video_storage_release(&owner->storage, bytes))
            video_runtime_fail(VIDEO_FAULT_OWNERSHIP);
        owner->storage.limit = original_limit;
    }
    return 1;
}

int video_worker_queue_report_locked(video_queue_report *report) {
    customCfwContext *ctx = peekCustomCfwContext();
    video_owner *owner = ctx ? ctx->video_owner : 0;
    if (!report) return 0;
    *report = (video_queue_report){0};
    if (!owner || ctx->video.preparing || !ctx->video.published) return 0;
    video_nal_queue *q = &owner->queue;
    *report = (video_queue_report){q->capacity, video_queue_credits(q),
                                  q->accepted, q->consumed, q->expected, q->pictures,
                                  __atomic_load_n(&q->gap_deadline, __ATOMIC_ACQUIRE),
                                  q->gap_sequence, q->header_progress};
    return 1;
}

int video_worker_start(uint32_t ingress_allowance) {
    return video_worker_start_guarded(ingress_allowance, 0);
}

void video_worker_request_stop_locked(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx) {
        video_lifecycle_stop(&ctx->video);
        video_worker_wake_locked(ctx->video.token, VIDEO_WAKE_CANCEL);
    }
}

int video_worker_wake_locked(uint32_t token, uint32_t bits) {
    customCfwContext *ctx = peekCustomCfwContext();
    video_owner *owner = ctx ? ctx->video_owner : 0;
    if (!owner || token != owner->token || !bits || (bits & ~VIDEO_WAKE_ALL)) return 0;
    return !(VIDEO_OS_EVENT_SET(owner->wake, bits) & VIDEO_EVENT_ERROR);
}

int video_worker_unpin_locked(uint32_t token, int callback) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !video_lifecycle_unpin(&ctx->video, token, callback)) return 0;
    video_worker_wake_locked(token, VIDEO_WAKE_DRAIN);
    return 1;
}

static int video_stop_token(uint32_t token, uint32_t timeout_ms) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return 1;
    if (timeout_ms > VIDEO_STOP_LIMIT_MS) timeout_ms = VIDEO_STOP_LIMIT_MS;
    if (!ctx->image_mutex) return 1;
    if (!video_control_take(ctx)) return 0;
    if (token && token != ctx->video.token) { video_control_give(ctx); return 1; }
    video_fold_signals(ctx);
    video_owner *owner = ctx->video_owner;
    if (ctx->video.state == VIDEO_IDLE) { video_control_give(ctx); return 1; }
    if (ctx->video.state == VIDEO_QUARANTINED) { video_control_give(ctx); return 0; }
    video_lifecycle_stop(&ctx->video);
    video_worker_wake_locked(ctx->video.token, VIDEO_WAKE_CANCEL);
    if (ctx->video_reaper || (owner && owner->thread == VIDEO_OS_THREAD_ID())) {
        video_control_give(ctx); return 0;
    }
    ctx->video_reaper = 1;
    video_control_give(ctx);
    uint32_t deadline = VIDEO_TICK + timeout_ms;
    for (;;) {
        uint32_t now = VIDEO_TICK;
        uint32_t remaining = (int32_t)(deadline - now) > 0 ? deadline - now : 0;
        if (video_control_wait(ctx, remaining)) {
            video_fold_signals(ctx);
            if (ctx->video.state == VIDEO_QUARANTINED) {
                video_control_give(ctx); return 0;
            }
            owner = ctx->video_owner;
            int preparing = ctx->video.preparing;
            int parked = !preparing && __atomic_load_n(&ctx->video.parked, __ATOMIC_ACQUIRE);
            video_control_give(ctx);
            if (parked) break;
        }
        now = VIDEO_TICK;
        if ((int32_t)(now - deadline) >= 0 || !owner) goto quarantine;
        uint32_t result = VIDEO_OS_EVENT_WAIT(owner->complete,
            VIDEO_COMPLETE_PREPARED | VIDEO_COMPLETE_PARKED, 0, deadline - now);
        if (result & VIDEO_EVENT_ERROR) goto quarantine;
    }
    if (owner && owner->thread) {
        if (VIDEO_OS_THREAD_TERMINATE(owner->thread) != 0) goto quarantine;
        owner->thread = 0;
    }
    video_worker_report report = owner ? video_completed_report(owner) : (video_worker_report){0};
    if (owner && !report.guards_ok) {
        __atomic_store_n(&ctx->video.fault, VIDEO_FAULT_STACK, __ATOMIC_RELEASE);
        goto quarantine;
    }
    if (owner && (!video_platform_event_quiescent(owner->wake) ||
                   !video_platform_event_quiescent(owner->complete))) {
        __atomic_store_n(&ctx->video.fault, VIDEO_FAULT_EVENT, __ATOMIC_RELEASE);
        goto quarantine;
    }
    if (!video_control_take(ctx)) goto quarantine;
    video_lifecycle_terminated(&ctx->video);
    if (!video_lifecycle_can_reclaim(&ctx->video)) {
        video_control_give(ctx); goto quarantine;
    }
    __atomic_store_n(&ctx->video_runtime, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&ctx->video_owner, 0, __ATOMIC_RELEASE);
    ctx->video_last_report = report;
    video_control_give(ctx);
    token = ctx->video.token;
    if (owner) video_free_owner(owner);
    __atomic_store_n(&ctx->video_reclaimed_token, token, __ATOMIC_RELEASE);
    if (!video_control_take(ctx)) return 0;
    video_fold_signals(ctx);
    int complete = ctx->video.state == VIDEO_IDLE;
    video_control_give(ctx);
    return complete;

quarantine:
    /* Preserve binding and all allocations, even if an abort is still private.
     * No timer/lease callback or second controller may free a live task. */
    __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
    if (video_control_take(ctx)) {
        video_fold_signals(ctx);
        video_control_give(ctx);
    }
    return 0;
}

int video_worker_stop(uint32_t timeout_ms) {
    return video_stop_token(0, timeout_ms);
}

int video_worker_reset(uint32_t ingress_allowance, uint32_t timeout_ms) {
    return video_worker_stop(timeout_ms) && video_worker_start(ingress_allowance);
}
int video_worker_get_report(video_worker_report *out) {
    if (!out) return 0;
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) { *out = (video_worker_report){0}; return 1; }
    if (!video_control_take(ctx)) return 0;
    video_fold_signals(ctx);
    *out = ctx->video_last_report;
    out->state = video_lifecycle_state(&ctx->video);
    video_control_give(ctx);
    return 1;
}

#pragma clang section text=""
