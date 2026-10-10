/* Optional bounded frame verification. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "../h264/g2_h264.h"

enum {
    VIDEO_VERIFY_FRAMES = 0x80,
    VIDEO_EVIDENCE_ROWS = 16,
    VIDEO_EVIDENCE_ALLOWANCE = 1536,
    VIDEO_TIMING_INVALID = 1,
    VIDEO_CLOCK_SAMPLE_TICKS = 10,
    VIDEO_CLOCK_SAMPLE_ATTEMPTS = 20,
    VIDEO_FRAME_RESULT_BYTES = 80,
    VIDEO_FRAME_ACK_BYTES = 16
};

/* Sixteen LE32 fields on the wire. Decode timings exclude calibration, CRC,
 * packing, display and transport; the NAL range includes no-output calls. */
typedef struct {
    uint32_t ordinal, first_nal, last_nal, geometry, crc;
    uint32_t cycles, ticks, clock_before, clock_after, flags;
    uint32_t decode_tick, copy_tick, calls, finishing_cycles, finishing_ticks;
    uint32_t no_output_cycles;
} video_frame_result;

/* Allocate only for explicit verification. Off playback retains no rows and
 * never waits for an ACK; the always-available diagnostics are independent. */
typedef struct {
    video_frame_result rows[VIDEO_EVIDENCE_ROWS], building;
    uint32_t high, acked, stalls, blocked, start_cycles, start_tick;
    uint32_t no_output_cycles, no_output_ticks, no_output_calls;
} video_evidence;

/* CRC32 uses the active Y bytes, without stride padding: reflected 0xedb88320,
 * initial/final xor 0xffffffff, matching the PC decoder reference. */
uint32_t video_frame_crc(const g2_h264_frame_info *);
