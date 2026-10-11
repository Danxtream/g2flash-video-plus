/* Connection-bound standard PHY adapter. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_RADIO_OWNERS = 2,
    VIDEO_RADIO_ACQUIRE_MS = 2000,
    VIDEO_RADIO_RESTORE_MS = 6500,
    VIDEO_RADIO_RETRY_MS = 10,
    VIDEO_RADIO_RESTORE_ATTEMPTS = 3,
    VIDEO_RADIO_OBSERVED = 0, VIDEO_RADIO_CAPTURE, VIDEO_RADIO_SELECT,
    VIDEO_RADIO_ACTIVE, VIDEO_RADIO_FALLBACK, VIDEO_RADIO_RESTORE_WAIT,
    VIDEO_RADIO_RESTORE_SET, VIDEO_RADIO_RESTORE_READ, VIDEO_RADIO_RESTORED,
    VIDEO_RADIO_UNCERTAIN,
    VIDEO_RADIO_LINKS = 3, /* Donor DM connection-control blocks. */
    VIDEO_RADIO_PHY_1M = 1,
    VIDEO_RADIO_PHY_2M = 2,
    VIDEO_RADIO_PHY_CODED = 3,
    VIDEO_RADIO_NONE = 0,
    VIDEO_RADIO_READ = 1,
    VIDEO_RADIO_SET = 2,
    VIDEO_RADIO_COMMAND_EVENT = 0xf3,
    VIDEO_RADIO_COMMAND_TAG = 0x5250,
    VIDEO_RADIO_DM_OPEN = 39,
    VIDEO_RADIO_DM_CLOSE = 40,
    VIDEO_RADIO_DM_READ = 68,
    VIDEO_RADIO_DM_UPDATE = 70,
    VIDEO_RADIO_DIAGNOSTICS = 1,
    VIDEO_RADIO_REFUSAL_DIAGNOSTICS = 2,
    VIDEO_RADIO_REPORT_BYTES = 128,
    VIDEO_RADIO_RECORD_BYTES = 32,
    VIDEO_RADIO_REPORT_VERSION = 1,
    VIDEO_RADIO_TRACE_BYTES = 32,
    VIDEO_RADIO_POOL_RESERVE = 3,
    VIDEO_RADIO_REQUEST_SITE = 1,
    VIDEO_RADIO_CONSUMER_SITE = 2,
    VIDEO_RADIO_REFUSAL_NONE = 0,
    VIDEO_RADIO_REFUSAL_ARGUMENT,
    VIDEO_RADIO_REFUSAL_BUSY,
    VIDEO_RADIO_REFUSAL_SERIAL,
    VIDEO_RADIO_REFUSAL_LINK,
    VIDEO_RADIO_REFUSAL_EPOCH,
    VIDEO_RADIO_REFUSAL_PERMISSION,
    VIDEO_RADIO_REFUSAL_POOL_METADATA,
    VIDEO_RADIO_REFUSAL_POOL_ELIGIBLE,
    VIDEO_RADIO_REFUSAL_POOL_EMPTY,
    VIDEO_RADIO_REFUSAL_POOL_RESERVE,
    VIDEO_RADIO_REFUSAL_ALLOC,
    VIDEO_RADIO_REFUSAL_COMMAND_LINK,
    VIDEO_RADIO_REFUSAL_COMMAND_CREDIT,
    VIDEO_RADIO_REFUSAL_COMMAND_ALLOC
};

typedef struct {
    uint8_t index, free;
    uint16_t size;
} video_radio_pool_sample;

/* Counts stop at four safe nodes; UINT8_MAX means unavailable, not empty.
 * Attempts have their own saturating identity because refused mail has no
 * command serial. The last refusal remains visible after a later success. */
typedef struct {
    uint32_t attempt, tick, epoch;
    uint8_t connection, operation, site, reason;
    uint16_t required;
    uint8_t skipped, reserved;
    video_radio_pool_sample first, selected;
    uint32_t reserved_tail;
} video_radio_trace;

typedef struct {
    uint32_t tick;
    uint8_t tx, rx, valid, reserved;
} video_radio_sample;

typedef struct {
    uint32_t epoch, tick, sample;
    uint32_t permission; /* A finite owner, never supplied by a wire request. */
    uint16_t handle;
    uint8_t connected, bound, tx, rx, valid, status;
    video_radio_sample before, after;
    uint8_t phase, owners, changed, uncertain;
    uint32_t deadline, retry_tick, restore_serial;
    uint8_t attempts;
} video_radio_link;

typedef struct {
    uint32_t generation, epoch;
    uint8_t connection, changing, reserved[2];
} video_radio_owner;

/* One queued command is enough for bounded observation and restoration. The
 * context survives disconnects; copied messages carry both epoch and serial. */
typedef struct {
    video_radio_link links[VIDEO_RADIO_LINKS];
    video_radio_owner owners[VIDEO_RADIO_OWNERS];
    uint32_t permission_serial;
    uint32_t serial, completed, epoch;
    uint32_t permission;
    uint8_t connection, operation, tx, rx;
    volatile uint8_t pending, result;
    uint8_t observe_next, reserved;
    uint32_t attempt_counter, copied_attempt;
    video_radio_trace last_attempt, last_refusal;
} video_radio_state;

/* Bind only the actual peripheral connection supplied by the stock EUS write
 * callback. No connection enumeration or global current-phone handle is used. */
void video_radio_bind(uint8_t connection);
/* Chain the original registered callbacks exactly once. The DM observer only
 * copies fixed state; it never allocates, waits, or issues a PHY command. */
void video_radio_dm_event(void *event);
void video_radio_app_handler(uint8_t events, void *message);
/* Task-context request, outside image/display locks. SET requires permission
 * on this exact epoch; READ is observational. Refusal never changes a PHY. */
int video_radio_request(uint8_t connection, uint32_t epoch, uint8_t operation,
                        uint8_t tx, uint8_t rx, uint32_t permission);
/* Freeze a bounded PHY-only DIAGNOSTICS selection, then request at most one
 * observational refresh. Unknown values remain explicit; this never SETs PHY. */
int video_radio_snapshot(uint8_t out[VIDEO_RADIO_REPORT_BYTES]);
/* Freeze refusal/pool evidence only: no READ, SET, heap walk or context
 * creation. A separate kind keeps the original PHY report byte-compatible. */
int video_radio_refusal_snapshot(uint8_t out[VIDEO_RADIO_REPORT_BYTES]);

/* Controller task only, outside image/display locks. Capture a fresh standard
 * read before granting finite SET permission; Off issues no PHY request. */
int video_radio_acquire(uint32_t generation, int off);
/* Retire a generation without waiting. Last-owner restoration is serviced
 * asynchronously; lost completion leaves explicit quarantine, not success. */
void video_radio_release(uint32_t generation);
void video_radio_step(void);
/* Bounded fixed-state reads, also safe from notification contexts. */
int video_radio_generation_lost(uint32_t generation);
int video_radio_maintenance(void);
