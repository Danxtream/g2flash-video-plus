/* SPDX-License-Identifier: GPL-3.0-only */
#include "lifecycle.h"

static uint32_t video_signal(const volatile uint32_t *p) {
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

uint32_t video_lifecycle_claim(video_lifecycle *s) {
    if (!s || s->state != VIDEO_IDLE || s->preparing || s->published) return 0;
    /* Never reuse a generation after wrap: stale callbacks may outlive STOP. */
    if (s->generation == UINT32_MAX) { s->state = VIDEO_QUARANTINED; return 0; }
    s->token = ++s->generation;
    s->state = VIDEO_STARTING;
    s->preparing = 1;
    s->terminated = s->output_pins = s->callback_users = 0;
    __atomic_store_n(&s->cancel, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->ready, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->parked, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s->fault, 0, __ATOMIC_RELEASE);
    return s->token;
}

int video_lifecycle_cancelled(const video_lifecycle *s, uint32_t token) {
    /* token remains stable until termination, even when generation advances. */
    return !s || !token || token != s->token || video_signal(&s->cancel);
}

int video_lifecycle_publish(video_lifecycle *s, uint32_t token) {
    if (!s || !s->preparing || s->state != VIDEO_STARTING ||
        token != s->generation || video_lifecycle_cancelled(s, token)) return 0;
    s->published = 1;
    s->preparing = 0;
    return 1;
}

void video_lifecycle_stop(video_lifecycle *s) {
    if (!s || s->state == VIDEO_IDLE) return;
    if (s->state != VIDEO_STOPPING && s->state != VIDEO_QUARANTINED &&
        s->generation != UINT32_MAX) ++s->generation;
    __atomic_store_n(&s->cancel, 1, __ATOMIC_RELEASE);
    if (s->state != VIDEO_QUARANTINED) s->state = VIDEO_STOPPING;
}

void video_lifecycle_abort_start(video_lifecycle *s, uint32_t token, int worker_exists) {
    if (!s || token != s->token || !s->preparing || s->published) return;
    video_lifecycle_stop(s);
    s->preparing = 0;
    if (!worker_exists) {
        __atomic_store_n(&s->parked, 1, __ATOMIC_RELEASE);
        s->terminated = 1;
    }
}

int video_lifecycle_ready(video_lifecycle *s, uint32_t token) {
    if (video_lifecycle_cancelled(s, token)) return 0;
    __atomic_store_n(&s->ready, 1, __ATOMIC_RELEASE);
    /* STOP can race this store; the controller checks cancel before READY. */
    return !video_lifecycle_cancelled(s, token);
}

void video_lifecycle_park(video_lifecycle *s, uint32_t token, uint32_t fault) {
    if (!s || token != s->token) return;
    if (fault) {
        __atomic_store_n(&s->fault, fault, __ATOMIC_RELEASE);
        __atomic_store_n(&s->cancel, 1, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&s->parked, 1, __ATOMIC_RELEASE);
}

uint32_t video_lifecycle_state(video_lifecycle *s) {
    if (!s) return VIDEO_IDLE;
    if (video_signal(&s->fault)) video_lifecycle_stop(s);
    if (s->state == VIDEO_STARTING && video_signal(&s->ready) &&
        !video_signal(&s->cancel)) s->state = VIDEO_READY;
    return s->state;
}

int video_lifecycle_can_terminate(const video_lifecycle *s) {
    return s && !s->preparing && !s->terminated && video_signal(&s->parked);
}

void video_lifecycle_terminated(video_lifecycle *s) {
    if (video_lifecycle_can_terminate(s)) s->terminated = 1;
}

void video_lifecycle_quarantine(video_lifecycle *s) {
    if (!s || s->state == VIDEO_IDLE) return;
    video_lifecycle_stop(s);
    s->state = VIDEO_QUARANTINED;
}

int video_lifecycle_pin(video_lifecycle *s, uint32_t token, int callback) {
    if (!s || s->state != VIDEO_READY || token != s->generation ||
        video_lifecycle_cancelled(s, token)) return 0;
    uint32_t *count = callback ? &s->callback_users : &s->output_pins;
    if (*count == UINT32_MAX) return 0;
    ++*count;
    return 1;
}

int video_lifecycle_unpin(video_lifecycle *s, uint32_t token, int callback) {
    if (!s || token != s->token) return 0;
    uint32_t *count = callback ? &s->callback_users : &s->output_pins;
    if (!*count) return 0;
    --*count;
    return 1;
}

int video_lifecycle_can_reclaim(const video_lifecycle *s) {
    return s && s->state == VIDEO_STOPPING && !s->preparing && s->terminated &&
           video_signal(&s->parked) && !s->output_pins && !s->callback_users;
}

int video_lifecycle_reclaimed(video_lifecycle *s) {
    if (!video_lifecycle_can_reclaim(s)) return 0;
    s->published = 0;
    s->state = VIDEO_IDLE;
    return 1;
}
