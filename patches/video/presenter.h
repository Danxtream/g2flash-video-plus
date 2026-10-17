/* Lazy owned presenter task. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "decoded_queue.h"
#include "pacing.h"
#include "storage.h"
#include "evidence.h"
#include "worker.h"
#include "sync.h"
#include "shown.h"
#include "menu.h"
#include "trigger_timing.h"

enum {
    VIDEO_QUEUED_PRESENTATION = 4,
    VIDEO_PAIRED_PRESENTATION = 8,
    VIDEO_DITHER_PRESENTATION = 16,
    VIDEO_START_PAUSED = 32,
    VIDEO_PREROLL_SHIFT = 16,
    VIDEO_PRESENTER_STACK_BYTES = 8192,
    /* Separate from the decoder cap: pixels, guarded stack, TCB/events/ledger
     * and room for the later bounded sync/report records, with charged headers. */
    VIDEO_PRESENTER_A4_LIMIT = 77824,
    VIDEO_PRESENTER_Y8_LIMIT = VIDEO_PRESENTER_A4_LIMIT + VIDEO_DECODED_Y8_BYTES,
    VIDEO_PLAYBACK_CAPABILITIES = 13,
    VIDEO_PLAYBACK_CAPABILITIES_BYTES = 16,
    VIDEO_PLAYBACK_QUEUE = 1,
    VIDEO_PLAYBACK_PAIRED = 2,
    VIDEO_PLAYBACK_METADATA = 4,
    VIDEO_PLAYBACK_SHOWN = 8,
    VIDEO_PLAYBACK_CONTROLS = 16,
    VIDEO_PLAYBACK_SEEK = 32,
    VIDEO_PLAYBACK_MENU = 64,
    VIDEO_PLAYBACK_POLICY = 128,
    /* High bits of capability byte 4; low six bits retain decoded depth. */
    VIDEO_PLAYBACK_GEOMETRY = 128,
    VIDEO_PLAYBACK_PICTURE_STYLE = 64
};

struct video_owner;
typedef struct {
    struct video_owner *decoder;
    void *raw, *stack_allocation, *pixels;
    uint8_t *stack;
    uint32_t thread, wake, complete, armed, parked, generation, token;
    uint32_t shown, holds, fault, stack_used, guards_ok;
    video_decoded_queue queue;
    video_pacing pacing;
    video_sync sync;
    uint32_t paired, sync_sent, sync_deadline, ready_sent, done_pts, trigger_tick, wire_capture;
    uint32_t captured_ordinal, queued_ticks[VIDEO_DECODED_DEPTH];
    uint32_t pause_requested, paused, control_serial, control_ack;
    uint32_t control_sent, control_deadline, wire_serial, wire_paused, wire_boundary;
    uint32_t preroll, media_shown, pause_after_first;
    uint32_t control_kind, control_menu, wire_kind, menu_actual, menu_dirty, menu_command;
    uint32_t scale_requested, flight_native;
    /* Phone pacing: the latest commanded ordinal and this lens's due tick;
     * once applied, late pictures catch up instead of re-anchoring to now. */
    uint32_t pace_serial, pace_applied, pace_ordinal, pace_tick, phone_paced;
    uint32_t flight_retried, claim_tick, submit_tick;
    video_trigger_timing trigger_timing;
    video_menu menu;
    video_shown_ring shown_rows;
    video_storage storage;
    video_frame_result evidence[VIDEO_DECODED_DEPTH];
    uint8_t tcb[VIDEO_THREAD_CONTROL_BYTES] __attribute__((aligned(8)));
    uint8_t wake_control[VIDEO_EVENT_CONTROL_BYTES] __attribute__((aligned(8)));
    uint8_t complete_control[VIDEO_EVENT_CONTROL_BYTES] __attribute__((aligned(8)));
} video_presenter;

/* Owner-bound frozen snapshot; calculation and sentinel sampling are task work. */
static int video_trigger_snapshot_locked(customCfwContext *, const uint8_t *, uint8_t, uint8_t *);
