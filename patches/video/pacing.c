/* SPDX-License-Identifier: GPL-3.0-only */
#include "pacing.h"

#pragma clang section text=".text.video"

int video_pacing_anchor(video_pacing *p, uint32_t generation, uint32_t ordinal, uint32_t tick, uint32_t fps) {
    if (!p || !generation || !ordinal || (fps != 10 && fps != 15 && fps != 20)) return 0;
    *p = (video_pacing){tick, ordinal, generation, fps};
    return 1;
}

int video_pacing_deadline(const video_pacing *p, uint32_t generation, uint32_t ordinal, uint32_t *tick) {
    if (!p || !tick || !p->fps || p->generation != generation || ordinal < p->ordinal) return 0;
    /* Split whole seconds to avoid an overflowing product or a 64-bit divide. */
    uint32_t delta = ordinal - p->ordinal, seconds = delta / p->fps;
    if (seconds > INT32_MAX / 1000) return 0;
    uint32_t offset = seconds * 1000 + (delta % p->fps) * 1000 / p->fps;
    if (offset > INT32_MAX) return 0;
    *tick = p->tick + (uint32_t)offset;
    return 1;
}

#pragma clang section text=""
