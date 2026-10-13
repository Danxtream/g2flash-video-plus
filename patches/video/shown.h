/* Nonblocking per-lens copy evidence. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
enum {
    VIDEO_SHOWN_ROWS = 16, VIDEO_SHOWN_BYTES = 64, VIDEO_CONTROL_SHOWN = 15,
    VIDEO_SHOWN_PAIRED = 1, VIDEO_SHOWN_UNCERTAIN = 2, VIDEO_SHOWN_RECOVERED = 4,
    VIDEO_SHOWN_TV = 8, VIDEO_SHOWN_UNSIGNALLED = 16
};
typedef struct {
    uint32_t ordinal, pts, copy_tick, trigger_tick, queued_tick, flags;
} video_shown_frame;
typedef struct {
    uint32_t high, first, lost;
    video_shown_frame rows[VIDEO_SHOWN_ROWS];
} video_shown_ring;
/* Publish actual copy capture time; overwrite old telemetry without gating playback. */
int video_shown_publish(video_shown_ring *, const video_shown_frame *);
/* Cursor names telemetry serial, not NAL or media-frame ordinal. A stale cursor
 * recovers the oldest available row with explicit loss in the caller's report. */
int video_shown_read(const video_shown_ring *, uint32_t cursor,
                      video_shown_frame *, uint32_t *serial);
