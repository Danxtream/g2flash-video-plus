/* Versioned, receive-only video controls. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "../message_transport.h"

enum {
    VIDEO_MESSAGE_ID = 31,
    VIDEO_PROTOCOL_VERSION = 1,
    VIDEO_CONTROL_CAPABILITIES = 0,
    VIDEO_CONTROL_START = 1,
    VIDEO_CONTROL_STOP = 2,
    VIDEO_CONTROL_RESET = 3,
    VIDEO_CONTROL_STATUS = 4,
    VIDEO_CONTROL_PAGE = 5,
    VIDEO_CONTROL_NAL = 6,
    VIDEO_CONTROL_DIAGNOSTICS = 7,
    VIDEO_CONTROL_HEADER_BYTES = 8,
    VIDEO_START_BYTES = 24,
    VIDEO_CONTROL_REPLAYS = 4,
    VIDEO_STATUS_BYTES = 80,
    VIDEO_DIAGNOSTICS_BYTES = 128,
    VIDEO_REPLY_HEADER_BYTES = 8,
    VIDEO_RECORD_LIMIT = 4096,
    VIDEO_NAL_LIMIT = VIDEO_RECORD_LIMIT - 10,
    VIDEO_FRAME_WIDTH = 320,
    VIDEO_FRAME_HEIGHT = 192,
    VIDEO_FRAME_REFERENCES = 1,
    VIDEO_FRAME_DPB = 2,
    VIDEO_INTERVAL_MIN = 10,
    VIDEO_INTERVAL_MAX = 1000,
    VIDEO_INACTIVITY_LIMIT_MS = 30000,
    VIDEO_CONTROLLER_START = 1,
    VIDEO_CONTROLLER_STOP = 2,
    VIDEO_CONTROLLER_REAP = 4,
    VIDEO_CONTROLLER_LEASE = 8,
    VIDEO_CONTROL_ACCEPTED = 0,
    VIDEO_CONTROL_FORMAT,
    VIDEO_CONTROL_STALE,
    VIDEO_CONTROL_BUSY,
    VIDEO_CONTROL_LEASE,
    VIDEO_CONTROL_MEMORY,
    VIDEO_CONTROL_DISPATCH,
    VIDEO_CONTROL_QUARANTINE,
    VIDEO_CONTROL_DECODER,
    VIDEO_CONTROL_GAP,
    VIDEO_CONTROL_INACTIVITY,
    VIDEO_CONTROL_INPUT,
    VIDEO_CONTROL_CONFLICT,
    VIDEO_CONTROL_SEQUENCE
};

/* Cached exact requests and frozen snapshots make retries idempotent, including
 * after a reply enqueue fails. Pagination never re-executes a control. */
typedef struct {
    uint32_t request;
    uint8_t length, result, capacity, pending;
    uint8_t snapshot_bytes;
    uint8_t command[VIDEO_START_BYTES];
    uint8_t snapshot[VIDEO_DIAGNOSTICS_BYTES];
} video_control_replay;

typedef struct {
    uint32_t request_high[2], stream_high, stream, interval;
    uint32_t start_guard, control_generation, error, owner_origin;
    volatile uint32_t active_deadline;
    volatile uint32_t controller_serial, controller_job, controller_reasons;
    volatile uint32_t controller_park_token;
    volatile uint32_t controller_failed;
    volatile uint32_t notify_release_generation, notify_lease_generation;
    volatile uint32_t notify_failed_generation;
    volatile uint32_t stack_sequence, stack_task, stack_used, stack_bytes;
    uint8_t replay_next[2];
    video_control_replay replay[2][VIDEO_CONTROL_REPLAYS];
} video_control_state;

/* Dispatch task-context controls; replies are sent after releasing image_mutex.
 * The transport keeps input borrowed only until this synchronous call returns. */
int video_control_received(const uint8_t *, uint16_t, const cfw_message_route *);
/* Request task-context reclamation under image_mutex, without waiting/freeing. */
int video_controller_request_locked(uint32_t reasons);
/* Mode-11 cancellation under image_mutex: wake/request only, never join/free. */
void video_control_cancel_locked(void);
/* Existing lease callbacks may hold display locks or run on service threads.
 * Post stable generation-tagged signals with zero-timeout dispatch only: no
 * mutex/event/heap access. The task-context controller folds them later. */
void video_control_notify_lease(int released);
/* Last worker notification uses only stable context and its generation token. */
void video_controller_parked(uint32_t token);
/* Refuse conflicting shadow/cache mutation while video preparation owns RAM. */
int video_control_blocks_custom(const uint8_t *, uint32_t);
/* Task-context transport admission preserves the display reserve and shadow
 * allowance while video owns cached memory. Recheck after allocating too. */
int video_transport_room(uint32_t bytes);
/* Sample the current cleanup task outside shared locks; retain the greatest
 * lifetime usage observed on an authenticated donor pool stack. */
void video_controller_stack_sample(void);
/* Capture approximate live heaps and safely completed worker/stack reports
 * outside display/image locks into one bounded, versioned response. */
int video_diagnostics_snapshot(uint8_t out[VIDEO_DIAGNOSTICS_BYTES]);
