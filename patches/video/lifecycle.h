/* Two-phase session ownership. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum video_session_state {
    VIDEO_IDLE, VIDEO_STARTING, VIDEO_READY, VIDEO_STOPPING, VIDEO_QUARANTINED
};

/* Controller transitions need the image mutex. Worker signals use atomics;
 * no function here waits, allocates, invokes C++, or takes a display lock. */
typedef struct {
    uint32_t generation, token, state, preparing, published, terminated;
    uint32_t output_pins, callback_users;
    volatile uint32_t cancel, ready, parked, fault;
} video_lifecycle;

/* Claim an idle session, keeping it busy until private startup rolls back. */
uint32_t video_lifecycle_claim(video_lifecycle *);
/* Publish only the still-current, uncancelled claim; then arm its worker. */
int video_lifecycle_publish(video_lifecycle *, uint32_t token);
/* Finish an unpublished startup. A created worker must still park and exit. */
void video_lifecycle_abort_start(video_lifecycle *, uint32_t token, int worker_exists);
/* Invalidate acceptance/output immediately; safe inside an existing lock scope. */
void video_lifecycle_stop(video_lifecycle *);
/* Read cancellation without taking a lock needed by the controller. */
int video_lifecycle_cancelled(const video_lifecycle *, uint32_t token);
/* Worker completion signals never publish readiness for an old generation. */
int video_lifecycle_ready(video_lifecycle *, uint32_t token);
/* Last worker signal before sleep: no owned memory may be used afterwards. */
void video_lifecycle_park(video_lifecycle *, uint32_t token, uint32_t fault);
/* Fold worker signals into controller state while holding the image mutex. */
uint32_t video_lifecycle_state(video_lifecycle *);
/* A parked worker may be terminated only after private startup has finished. */
int video_lifecycle_can_terminate(const video_lifecycle *);
/* Record successful termination, or quarantine on timeout/termination failure. */
void video_lifecycle_terminated(video_lifecycle *);
void video_lifecycle_quarantine(video_lifecycle *);
/* Guard later output/timer users against stale or cancelled sessions. */
int video_lifecycle_pin(video_lifecycle *, uint32_t token, int callback);
int video_lifecycle_unpin(video_lifecycle *, uint32_t token, int callback);
/* Reclamation is permitted only after termination and all users are quiescent. */
int video_lifecycle_can_reclaim(const video_lifecycle *);
/* Publish IDLE only after the caller has reclaimed the unpublished owner. */
int video_lifecycle_reclaimed(video_lifecycle *);
