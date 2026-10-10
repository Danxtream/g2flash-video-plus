/* Finite paired display ownership. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_POWER_BRIDGE = 4,
    VIDEO_POWER_VERSION = 1,
    VIDEO_POWER_BYTES = 26,
    VIDEO_POWER_ACQUIRE = 1,
    VIDEO_POWER_RENEW = 2,
    VIDEO_POWER_RELEASE = 3,
    VIDEO_POWER_ACK = 4,
    VIDEO_POWER_OK = 0,
    VIDEO_POWER_WAIT = 1,
    VIDEO_POWER_REFUSED = 2,
    VIDEO_POWER_LEASE_MS = 15000,
    VIDEO_POWER_RENEW_MS = 2000,
    VIDEO_POWER_START_MS = 2000,
    VIDEO_POWER_RETRY_MS = 50
};

typedef struct {
    uint32_t generation, stream, nonce, serial;
    volatile uint32_t deadline;
    uint32_t closed, requested;
} video_power_peer;

/* The controller owns policy. Receive and UI callbacks publish only copied
 * mail or atomic scheduling hints; this storage outlives every decoder task. */
typedef struct {
    video_power_peer peer;
    uint32_t generation, stream, nonce, serial;
    volatile uint32_t deadline, peer_deadline, signal_tick;
    uint32_t renewed, acquired;
    volatile uint32_t mailbox_state;
    uint8_t mailbox[VIDEO_POWER_BYTES];
    uint32_t ack_serial, ack_result, ack_deadline;
} video_power_state;

/* Task-context acquisition and rollback outside image/display locks. Local
 * and peer UI/driver readiness must both precede decoder arming. */
int video_power_acquire(uint32_t generation);
void video_power_release(uint32_t generation);
/* Fold copied bridge mail and finite renewals on the cleanup controller. */
void video_power_step(void);
/* No mutex, allocation, wait or borrowed pointer escapes this callback. */
uint32_t video_power_received(const uint8_t *, uint32_t);
/* A worker may wait only through the earlier of its input/power deadlines. */
uint32_t video_power_deadline(uint32_t generation);
/* Preserve the original idle decrement except for finite video ownership of
 * the dashboard. Explicit sleep, wear, faults and other UI apps remain stock. */
uint32_t video_dashboard_idle_count(uint32_t count);
void video_dashboard_idle_gate(void);
/* Original application refresh always runs; maintenance only posts work. */
uint32_t video_ui_refresh(uint32_t app, uint32_t event, const void *, uint32_t size);
