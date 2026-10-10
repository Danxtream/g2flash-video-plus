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
    VIDEO_EVENT_CONTROL_BYTES = 32,
    VIDEO_WAIT_FOREVER = UINT32_MAX,
    VIDEO_EVENT_ERROR = 0x80000000u,
    VIDEO_EVENT_TIMEOUT = 0xfffffffeu,
    VIDEO_WAKE_ARM = 1,
    VIDEO_WAKE_INPUT = 2,
    VIDEO_WAKE_CANCEL = 4,
    VIDEO_WAKE_DRAIN = 8,
    VIDEO_WAKE_LEASE = 16,
    VIDEO_WAKE_ALL = 31,
    VIDEO_COMPLETE_PREPARED = 1,
    VIDEO_COMPLETE_PARKED = 2,
    VIDEO_WORKER_PRIORITY = 8,
    VIDEO_STACK_FILL = 0xa5,
    VIDEO_GUARD_FILL = 0x5a,
    VIDEO_SHADOW_BYTES = 640 * 480 / 2,
    VIDEO_FAULT_INIT = 0x100,
    VIDEO_FAULT_OWNERSHIP,
    VIDEO_FAULT_STACK,
    VIDEO_FAULT_ABI,
    VIDEO_FAULT_EVENT,
    VIDEO_FAULT_GAP,
    VIDEO_FAULT_INACTIVITY,
    VIDEO_FAULT_FORMAT,
    VIDEO_FAULT_CONFLICT,
    VIDEO_FAULT_SEQUENCE
};

/* Completed-owner diagnostics persist after teardown. valid is false while an
 * unparked worker is quarantined: mutable allocation/stack data is not sampled. */
typedef struct {
    uint32_t valid, token, state, fault, storage_peak, storage_left;
    uint32_t object_bytes, object_alignment, stack_used, stack_bytes, guards_ok;
    uint32_t free_bytes[3], max_alloc[3]; /* cached, display, other heap */
} video_worker_report;

typedef struct {
    uint32_t capacity, credits, accepted, consumed, expected, pictures;
    uint32_t gap_deadline, gap_sequence, header_progress;
} video_queue_report;
/* Worker-only, outside image/display locks, after a completed decoder call.
 * dpb_full comes from decoder state, never packet count. One failed extension
 * preserves the four slots and all reserves. */
int video_worker_picture_complete(uint32_t token, int dpb_full);
/* Copy one validated raw NAL while holding image_mutex; never invoke C++. */
int video_worker_receive_nal_locked(uint32_t stream, uint32_t sequence,
                                    const uint8_t *, uint16_t, uint8_t origin);
/* Snapshot queue accounting under image_mutex, without sampling decoder data. */
int video_worker_queue_report_locked(video_queue_report *);

/* Call outside image/display locks. Requires a live framebuffer lease and no
 * texture cache. ingress_allowance is the outstanding peak beyond live buffers,
 * including decoded scratch and actual inflater growth. */
int video_worker_start(uint32_t ingress_allowance);
/* Recheck a nonzero control generation before claiming or publishing storage.
 * A STOP that arrives during allocation invalidates this guarded start. */
int video_worker_start_guarded(uint32_t ingress_allowance, uint32_t generation);
/* Lazily create only the shared command mutex, in task context outside locks. */
int video_worker_ensure_mutex(void);
/* Only cancellation, for callers already holding the image mutex. */
void video_worker_request_stop_locked(void);
/* Task-context hints, under the image mutex. Generation checks precede every
 * wake; ISR callers must not defer event pointers beyond their owner's life. */
int video_worker_wake_locked(uint32_t token, uint32_t bits);
/* Release an output/callback pin and notify drain, under the image mutex. */
int video_worker_unpin_locked(uint32_t token, int callback);
/* Call outside image/display locks. Bounded wait; failure retains the owner.
 * A lock-owning handler must request cancellation, leave its scope, then reap. */
int video_worker_stop(uint32_t timeout_ms);
/* Reset destroys the old owner completely before fresh lazy construction. */
int video_worker_reset(uint32_t ingress_allowance, uint32_t timeout_ms);
/* Read completed diagnostics and current lifecycle state under the image mutex. */
int video_worker_get_report(video_worker_report *);
