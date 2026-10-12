/* Rational local presentation cadence. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

typedef struct { uint32_t tick, ordinal, generation, valid; } video_pacing;

/* A new anchor is explicit; late frames never move an already committed deadline. */
int video_pacing_anchor(video_pacing *, uint32_t generation, uint32_t ordinal, uint32_t tick);
/* Return a wrap-safe 16 fps deadline; refuse intervals beyond signed tick range. */
int video_pacing_deadline(const video_pacing *, uint32_t generation, uint32_t ordinal,
                          uint32_t *tick);
