/* Versioned video controls. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "../message_transport.h"
#include "timeline.h"
#include "sync.h"
#include "geometry.h"

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
    VIDEO_CONTROL_FRAME_READ = 8,
    VIDEO_CONTROL_FRAME_ACK = 9,
    VIDEO_CONTROL_CREDITS = 10,
    VIDEO_CONTROL_TIMELINE = 11,
    VIDEO_CONTROL_FRAME_TIME = 12,
    VIDEO_CONTROL_FRAME_TIME_FAST = 14,
    VIDEO_CONTROL_PAUSE = 16,
    VIDEO_CONTROL_RESUME = 17,
    VIDEO_CONTROL_PLAYBACK = 18,
    VIDEO_CONTROL_RADIO_POLICY = 21,
    VIDEO_CONTROL_TRIGGER_TIMING = 22,
    /* A picture's VCL NAL carrying its own frame time, so the phone needs no
     * separate FRAME_TIME_FAST round trip per picture. 23 stays retired. */
    VIDEO_CONTROL_NAL_TIMED = 24,
    /* The phone's audio clock re-anchors the coordinator's picture schedule:
     * stream, both owner tokens, picture ordinal, then left and right lens ticks. */
    VIDEO_CONTROL_PACE = 25,
    VIDEO_CONTROL_NAL_FLOW = 26,
    VIDEO_CONTROL_NAL_TIMED_FLOW = 27,
    /* Capability and START opt-in; old owners keep their exact six-slot path. */
    VIDEO_DELIVERY_FLOW = 64,
    VIDEO_FLOW_REPLY_BYTES = 29,
    VIDEO_FLOW_REPLY_MARKER = 0xd1,
    VIDEO_CREDITS_WIDE = 2,
    VIDEO_CREDITS_WIDE_BYTES = 16,
    VIDEO_PACE_BYTES = 32,
    VIDEO_NAL_TIMED_HEADER_BYTES = 18,
    VIDEO_TIMED_TIMELINE_REFUSED = -4,
    VIDEO_TRIGGER_TIMING_BYTES = 96,
    VIDEO_RADIO_POLICY_BYTES = 24,
    VIDEO_RADIO_POLICY_OFF = 0x100,
    VIDEO_RADIO_POLICY_CONFIGURED = 0x200,
    VIDEO_RADIO_POLICY_MASK = VIDEO_RADIO_POLICY_OFF | VIDEO_RADIO_POLICY_CONFIGURED,
    VIDEO_PLAYBACK_STATE_BYTES = 36,
    VIDEO_CREDITS_REQUEST_BYTES = 20,
    /* Same request plus a flags word; VIDEO_CREDITS_TIMELINE packs the free
     * timeline records into the reply's result byte. */
    VIDEO_CREDITS_TIMELINE_REQUEST_BYTES = 24,
    VIDEO_CREDITS_TIMELINE = 1,
    VIDEO_CREDIT_RESULT_MASK = 31,
    VIDEO_CREDIT_TIMELINE_SHIFT = 5,
    VIDEO_CREDITS_BYTES = 6,
    VIDEO_FEATURE_CREDITS = 8,
    VIDEO_FEATURE_DECODE_TOTALS = 16,
    VIDEO_STATUS_EXTENSION_UNIT = 64,
    VIDEO_STATUS_EXTENSION_SHIFT = 5,
    VIDEO_CREDIT_CAPACITY_SIX = 8,
    VIDEO_CREDIT_STATE_SHIFT = 4,
    VIDEO_CREDIT_GAP = 128,
    VIDEO_CONTROL_HEADER_BYTES = 8,
    VIDEO_START_BYTES = 24,
    VIDEO_START_GEOMETRY_BYTES = 32,
    VIDEO_START_PICTURE_BYTES = 36,
    VIDEO_CONTROL_REPLAYS = 4,
    VIDEO_BASE_STATUS_BYTES = 128,
    VIDEO_STATUS_BYTES = VIDEO_BASE_STATUS_BYTES + VIDEO_STATUS_EXTENSION_UNIT,
    VIDEO_DIAGNOSTICS_BYTES = 128,
    VIDEO_REPLY_HEADER_BYTES = 8,
    /* Video-only bound; each replay freezes the proven ingress budget. */
    VIDEO_PAGE_REPLY_CAPACITY = CFW_VIDEO_REPLY_MAX_SIZE,
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
    VIDEO_CONTROLLER_PRESENT = 16,
    VIDEO_CONTROLLER_POWER = 32,
    VIDEO_CONTROLLER_RADIO = 64,
    VIDEO_CONTROLLER_SYNC = 128,
    VIDEO_RADIO_OFF = 2,
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
    VIDEO_CONTROL_SEQUENCE,
    VIDEO_CONTROL_DISPLAY,
    VIDEO_CONTROL_INCOMPLETE,
    VIDEO_CONTROL_POWER,
    VIDEO_CONTROL_HANDOFF,
    VIDEO_CONTROL_RADIO
};

/* Cached exact requests and frozen snapshots make retries idempotent, including
 * after a reply enqueue fails. Pagination never re-executes a control. */
typedef struct {
    uint32_t request;
    uint8_t length, result, capacity, pending;
    uint8_t snapshot_bytes;
    uint8_t command[VIDEO_START_PICTURE_BYTES];
    uint8_t snapshot[VIDEO_STATUS_BYTES];
} video_control_replay;

typedef struct {
    uint32_t request_high[2], stream_high, stream, interval;
    uint32_t start_guard, control_generation, error, owner_origin;
    uint32_t options;
    video_geometry geometry;
    uint32_t picture_style;
    uint32_t stop_generation; /* STOP remains visible until stock hand-back completes. */
    volatile uint32_t active_deadline;
    volatile uint32_t controller_serial, controller_job, controller_reasons;
    volatile uint32_t controller_park_token;
    volatile uint32_t controller_failed;
    volatile uint32_t notify_release_generation, notify_lease_generation;
    volatile uint32_t notify_failed_generation;
    volatile uint32_t stack_sequence, stack_task, stack_used, stack_bytes;
    uint8_t replay_next[2];
    uint8_t owner_here, owner_targets; /* Reuse alignment padding before timeline. */
    video_timeline timeline;
    video_sync_mail sync_mail;
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
/* Display hook posts stable completion only, through zero-timeout dispatch. */
void video_controller_presented(uint32_t token);
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

/* Copy only a reason and post a zero-timeout pool job; no lock/heap waits. */
void video_control_notify_radio(void);
