/* Lazy decoder task ownership. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_WORKER_STACK_BYTES = 16 * 1024,
    VIDEO_STACK_GUARD_BYTES = 32,
    VIDEO_THREAD_CONTROL_BYTES = 112,
    VIDEO_THREAD_STATIC_OFFSET = 109,
    VIDEO_THREAD_STATIC_FLAG = 2,
    VIDEO_STOP_LIMIT_MS = 2000,
    VIDEO_POLL_MS = 1,
    VIDEO_PARK_SLEEP_MS = 1000,
    VIDEO_WORKER_PRIORITY = 8,
    VIDEO_STACK_FILL = 0xa5,
    VIDEO_GUARD_FILL = 0x5a,
    VIDEO_SHADOW_BYTES = 640 * 480 / 2,
    VIDEO_FAULT_INIT = 0x100,
    VIDEO_FAULT_OWNERSHIP,
    VIDEO_FAULT_STACK,
    VIDEO_FAULT_ABI
};

/* Completed-owner diagnostics persist after teardown. valid is false while an
 * unparked worker is quarantined: mutable allocation/stack data is not sampled. */
typedef struct {
    uint32_t valid, token, state, fault, storage_peak, storage_left;
    uint32_t object_bytes, object_alignment, stack_used, stack_bytes, guards_ok;
    uint32_t free_bytes[3], max_alloc[3]; /* cached, display, other heap */
} video_worker_report;

/* Call outside image/display locks. Requires a live framebuffer lease and no
 * texture cache. ingress_allowance is the outstanding peak beyond live buffers,
 * including decoded scratch and actual inflater growth. No receive route here. */
int video_worker_start(uint32_t ingress_allowance);
/* Only cancellation, for callers already holding the image mutex. */
void video_worker_request_stop_locked(void);
/* Call outside image/display locks. Bounded wait; failure retains the owner.
 * A lock-owning handler must request cancellation, leave its scope, then reap. */
int video_worker_stop(uint32_t timeout_ms);
/* Reset destroys the old owner completely before fresh lazy construction. */
int video_worker_reset(uint32_t ingress_allowance, uint32_t timeout_ms);
/* Read completed diagnostics and current lifecycle state under the image mutex. */
int video_worker_get_report(video_worker_report *);
