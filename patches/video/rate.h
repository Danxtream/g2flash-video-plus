/* Bounded observational cadence evidence. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_RATE_DIAGNOSTICS = 6, VIDEO_RATE_ROWS = 8, VIDEO_RATE_POSTS = 16,
    VIDEO_RATE_INPUT = 1, VIDEO_RATE_DECODE = 2, VIDEO_RATE_OUTPUT = 3,
    VIDEO_RATE_FRAME = 4, VIDEO_RATE_READY = 8,
    VIDEO_RATE_EMPTY = 1, VIDEO_RATE_SLOW = 2, VIDEO_RATE_WAIT = 4,
    VIDEO_RATE_PRESENT = 8, VIDEO_RATE_PAUSED = 16, VIDEO_RATE_LOSS = 32,
    VIDEO_RATE_ACTIVE = 1, VIDEO_RATE_ENDED = 2, VIDEO_RATE_INVALID = 4
};
typedef struct {
    uint32_t serial, start, shown, gap, input, output, service, overrun;
    uint32_t decode_max, not_ready, flags, ordinal;
    uint32_t phase_lo, phase_hi; /* Ten 100-ms counts, four bits each. */
} video_rate_window;
typedef struct { uint32_t serial, from, to, previous, ordinal, gap, flags, decode_max; } video_rate_dip;
typedef struct { uint32_t tick, code; } video_rate_post;
typedef struct {
    video_rate_window rows[VIDEO_RATE_ROWS], current;
    video_rate_dip dips[VIDEO_RATE_ROWS];
    video_rate_post posts[VIDEO_RATE_POSTS];
    uint32_t stream, generation, token, anchor, tick, copy_tick, ordinal;
    uint32_t high, dip_high, mode, frame_ms, gap_flags, gap_decode;
    uint32_t ready, paused, flags, head, count, posted_mode;
    uint16_t gaps[16];
    uint32_t gap_count, gap_max;
} video_rate_state;
_Static_assert(sizeof(video_rate_state) <= 1024, "Cadence evidence exceeds its resident bound");

/* Caller holds the existing publication lock. Worker calls only post edges;
 * presenter calls own folding, window closure and shown publication. */
void video_rate_start(video_rate_state *, uint32_t stream, uint32_t generation, uint32_t token);
void video_rate_post_worker(video_rate_state *, uint32_t tick, uint32_t code);
void video_rate_present(video_rate_state *, uint32_t tick, uint32_t ready, uint32_t paused);
void video_rate_copy(video_rate_state *, uint32_t tick, uint32_t ordinal, uint32_t terminal);
void video_rate_stop(video_rate_state *, uint32_t tick);
/* Frozen 144-byte window or 128-byte dip/histogram reply. No allocation or waits. */
int video_rate_snapshot(const video_rate_state *, uint32_t mode, uint32_t cursor, uint8_t lens, uint8_t *);
