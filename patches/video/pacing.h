/* Rational local presentation cadence. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

typedef struct { uint32_t tick, ordinal, generation, fps; } video_pacing;

/* A new anchor is explicit; late frames never move an already committed deadline. */
int video_pacing_anchor(video_pacing *, uint32_t generation, uint32_t ordinal, uint32_t tick, uint32_t fps);
/* Move a commanded anchor (ordinal due at tick) to ordinal next, in either
 * direction, with the same rational spacing as deadlines. */
int video_pacing_retime(video_pacing *, uint32_t generation, uint32_t ordinal, uint32_t tick,
                        uint32_t next, uint32_t fps);
/* Return a wrap-safe clip-rate deadline; refuse intervals beyond signed tick range. */
int video_pacing_deadline(const video_pacing *, uint32_t generation, uint32_t ordinal,
                          uint32_t *tick);
