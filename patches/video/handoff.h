/* Tagged stock-screen restoration. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_HANDOFF_IDLE,
    VIDEO_HANDOFF_QUEUED,
    VIDEO_HANDOFF_COPYING,
    VIDEO_HANDOFF_COMPLETE,
    VIDEO_HANDOFF_FAILED,
    VIDEO_HANDOFF_LIMIT_MS = 2000,
    VIDEO_HANDOFF_RECORD_WORDS = 9
};

/* Only stable tags cross the display task. No decoder or shadow pointer is
 * retained after the retiring worker has parked and released its storage. */
typedef struct {
    volatile uint32_t phase;
    uint32_t generation, output_epoch;
} video_handoff_state;

/* Full-panel publication invalidates older ownership and tags a successful
 * video job. Stock viewport copies leave its uncleared margins owned by video. */
void video_display_note_output(uint32_t token);
/* Controller only, after parking and outside locks. Return 1 for completion or
 * newer output, -1 for known refusal, 0 for an unknown retained queue owner. */
int video_handoff_stop(uint32_t generation);
/* Type-3 refresh trampoline passes the stock task's copied record. Periodic
 * type-6 refreshes cannot claim this job or release its display gate. */
void video_display_refresh_gate(void);
void video_display_restore_copy(const uint32_t *record);
