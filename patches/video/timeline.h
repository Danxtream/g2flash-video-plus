/* Completed-picture media identity. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_TIMELINE_VERSION = 1,
    VIDEO_TIMELINE_RECORDS = 12, /* Matches the NAL queue; room reports cap at 6. */
    VIDEO_TIMELINE_BYTES = 24,
    VIDEO_TIMELINE_RATE_MASK = 15 /* Bits 0/1/2/3: 10/15/20/24 pictures per second. */
};

typedef struct {
    uint32_t ordinal, nal_sequence, pts;
} video_frame_time;

/* NAL sequence identifies the terminating slice, not a count of pictures.
 * Keep this fixed mailbox separate from decoded pixels and decoder references. */
typedef struct {
    uint32_t stream, generation, duration, start_pts, fps;
    uint32_t accepted, consumed, last_nal, last_pts;
    uint8_t head, count, configured, active;
    video_frame_time frames[VIDEO_TIMELINE_RECORDS];
} video_timeline;

/* Configure only a fresh stopped stream; active owner changes require RESET. */
int video_timeline_configure(video_timeline *, uint32_t stream, uint32_t duration,
                             uint32_t start_pts, uint16_t numerator, uint16_t denominator);
/* Bind the prepared record to the finite START generation without accepting reuse. */
int video_timeline_start(video_timeline *, uint32_t stream, uint32_t generation);
/* Accept monotonic completed-picture metadata with bounded independent credits. */
int video_timeline_offer(video_timeline *, uint32_t stream, uint32_t generation,
                         const video_frame_time *);
/* Permit only the explicit bounded seek prefix below the visible target PTS. */
int video_timeline_offer_preroll(video_timeline *, uint32_t stream, uint32_t generation,
                                 const video_frame_time *, uint32_t prefix);
/* Consume metadata only for the exact completed picture and terminating NAL. */
int video_timeline_take(video_timeline *, uint32_t generation, uint32_t ordinal,
                        uint32_t nal_sequence, video_frame_time *);
/* Retire only the matching generation; stale callbacks cannot clear a successor. */
void video_timeline_stop(video_timeline *, uint32_t generation);
