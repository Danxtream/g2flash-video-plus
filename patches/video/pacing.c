/* SPDX-License-Identifier: GPL-3.0-only */
#include "pacing.h"

#pragma clang section text=".text.video"

int video_pacing_anchor(video_pacing *p, uint32_t generation, uint32_t ordinal, uint32_t tick) {
    if (!p || !generation || !ordinal) return 0;
    *p = (video_pacing){tick, ordinal, generation, 1};
    return 1;
}

int video_pacing_deadline(const video_pacing *p, uint32_t generation, uint32_t ordinal, uint32_t *tick) {
    if (!p || !tick || !p->valid || p->generation != generation || ordinal < p->ordinal) return 0;
    uint64_t offset = ((uint64_t)(ordinal - p->ordinal) * 1000) >> 4;
    if (offset > INT32_MAX) return 0;
    *tick = p->tick + (uint32_t)offset;
    return 1;
}

#pragma clang section text=""
