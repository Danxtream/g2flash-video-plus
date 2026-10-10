/* Bounded decode aggregates. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_DECODE_BUCKETS = 6,
    VIDEO_DECODE_TOTAL_BYTES = 64,
    VIDEO_DECODE_TOTAL_VERSION = 1,
    VIDEO_DECODE_LONG_SAMPLE = 1,
    VIDEO_DECODE_SATURATED = 2,
    /* Longer brackets cannot establish an unambiguous cycle delta. This is
     * a diagnostic limit, never a decode or playback deadline. */
    VIDEO_DECODE_SAMPLE_LIMIT_MS = 1000
};

typedef struct {
    uint16_t flags, header_calls;
    uint32_t pictures;
    uint64_t cycles, ticks;
    uint32_t max_cycles, max_ticks, buckets[VIDEO_DECODE_BUCKETS];
    uint32_t header_cycles, header_ticks;
} video_decode_totals;

typedef struct {
    video_decode_totals totals;
    uint64_t pending_cycles, pending_ticks;
    uint32_t start_cycles, start_tick, vcl, clock_started;
} video_decode_accumulator;

/* Accumulate VCL work until a completed picture, keeping parameter-set/SEI
 * costs separate. Saturation or long samples mark evidence, never abort. */
void video_decode_add(video_decode_accumulator *, uint32_t cycles,
                      uint32_t ticks, int picture_ready);
/* Write an endian-independent fixed schema; publication is caller-locked. */
void video_decode_encode(const video_decode_totals *, uint8_t out[VIDEO_DECODE_TOTAL_BYTES]);
