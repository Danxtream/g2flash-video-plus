/* Bounded receive evidence. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    /* Subtype 5 was the retired clock snapshot; never reuse it. */
    VIDEO_REFUSAL_DIAGNOSTICS = 4,
    VIDEO_REFUSAL_ROWS = 4, VIDEO_REFUSAL_BYTES = 24,
    VIDEO_REFUSAL_LOCK = 1, VIDEO_REFUSAL_MISSING, VIDEO_REFUSAL_PAGE,
    VIDEO_REFUSAL_CAPACITY, VIDEO_REFUSAL_PENDING, VIDEO_REFUSAL_REPLY_ALLOCATION,
    VIDEO_REFUSAL_SEND, VIDEO_REFUSAL_FORMAT, VIDEO_REFUSAL_REPLAY, VIDEO_REFUSAL_COMMAND
};
typedef struct {
    volatile uint32_t busy, dropped;
    uint32_t serial;
    uint8_t rows[VIDEO_REFUSAL_ROWS][VIDEO_REFUSAL_BYTES];
} video_refusal_ring;
typedef struct {
    volatile uint32_t reply_busy[2];
    uint8_t reply[2][200];
    video_refusal_ring refusals[2];
} video_telemetry_state;
