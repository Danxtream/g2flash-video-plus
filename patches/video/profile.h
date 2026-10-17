/* Bounded elapsed-time mailboxes. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_PROFILE_DIAGNOSTICS = 3,
    /* Measuring build: cumulative decoder section counters, one page. */
    VIDEO_SECTION_DIAGNOSTICS = 7,
    VIDEO_PROFILE_STAGES = 8, VIDEO_PROFILE_SLOTS = 4,
    VIDEO_PROFILE_DECODE = 0, VIDEO_PROFILE_QUEUE_WAIT, VIDEO_PROFILE_QUEUE_COPY,
    VIDEO_PROFILE_PACK, VIDEO_PROFILE_DISPLAY, VIDEO_PROFILE_BRIDGE,
    VIDEO_PROFILE_SYNC, VIDEO_PROFILE_INGRESS,
    VIDEO_PROFILE_DROPPED = 1, VIDEO_PROFILE_SPAN = 2, VIDEO_PROFILE_SATURATED = 4
};
typedef struct { uint32_t generation, tick, cycles; } video_profile_capture;
typedef struct {
    volatile uint32_t phase;
    uint32_t generation, ticks, cycles, bytes;
} video_profile_mail;
typedef struct { uint32_t count, bytes, ticks, cycles_lo, cycles_hi, maximum; } video_profile_row;
typedef struct {
    volatile uint32_t generation, dropped, flags;
    uint32_t stream, token;
    video_profile_capture display_capture;
    video_profile_mail mail[VIDEO_PROFILE_STAGES][VIDEO_PROFILE_SLOTS];
    video_profile_row rows[VIDEO_PROFILE_STAGES];
} video_profile_state;

/* Capture and bounded post only; the presenter owns aggregation. */
video_profile_capture video_profile_begin(void);
void video_profile_end(video_profile_capture, uint32_t stage, uint32_t bytes);
