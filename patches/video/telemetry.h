/* Bounded receive evidence and read-only clocks. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "hop.h"

enum {
    VIDEO_REFUSAL_DIAGNOSTICS = 4, VIDEO_CLOCK_DIAGNOSTICS = 5,
    VIDEO_REFUSAL_ROWS = 4, VIDEO_REFUSAL_BYTES = 24,
    VIDEO_REFUSAL_LOCK = 1, VIDEO_REFUSAL_MISSING, VIDEO_REFUSAL_PAGE,
    VIDEO_REFUSAL_CAPACITY, VIDEO_REFUSAL_PENDING, VIDEO_REFUSAL_REPLY_ALLOCATION,
    VIDEO_REFUSAL_SEND, VIDEO_REFUSAL_FORMAT, VIDEO_REFUSAL_REPLAY, VIDEO_REFUSAL_COMMAND,
    VIDEO_CLOCK_START = 0, VIDEO_CLOCK_STEADY = 1, VIDEO_CLOCK_STOP = 2,
    VIDEO_CLOCK_BRACKET_MS = 250, VIDEO_CLOCK_MAX_MS = 1000,
    VIDEO_CLOCK_VALID = 1, VIDEO_CLOCK_LONG = 2, VIDEO_CLOCK_COUNTER = 4,
    VIDEO_CLOCK_CHANGED = 8, VIDEO_CLOCK_SKEW = 16
};
typedef struct {
    volatile uint32_t busy, dropped;
    uint32_t serial;
    uint8_t rows[VIDEO_REFUSAL_ROWS][VIDEO_REFUSAL_BYTES];
} video_refusal_ring;
typedef struct { uint32_t tick, cycles, after, perf; } video_clock_pair;
typedef struct {
    uint32_t stream, generation;
    video_clock_pair anchor, previous;
    uint8_t rows[3][128];
} video_clock_state;
typedef struct {
    volatile uint32_t reply_busy[2];
    uint8_t reply[2][200];
    video_refusal_ring refusals[2];
    video_clock_state clock;
    video_hop_state hop;
} video_telemetry_state;
