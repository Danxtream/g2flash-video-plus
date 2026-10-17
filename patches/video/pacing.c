/* SPDX-License-Identifier: GPL-3.0-only */
#include "pacing.h"

#pragma clang section text=".text.video"

/* Ticks between two ordinals delta apart at fps. Split whole seconds to avoid
 * an overflowing product or a 64-bit divide; refuse spans beyond signed range. */
static int video_pacing_span(uint32_t fps, uint32_t delta, uint32_t *span) {
    uint32_t seconds = delta / fps;
    if (seconds > INT32_MAX / 1000) return 0;
    uint32_t offset = seconds * 1000 + (delta % fps) * 1000 / fps;
    if (offset > INT32_MAX) return 0;
    *span = offset;
    return 1;
}

int video_pacing_anchor(video_pacing *p, uint32_t generation, uint32_t ordinal, uint32_t tick, uint32_t fps) {
    if (!p || !generation || !ordinal || (fps != 10 && fps != 15 && fps != 20)) return 0;
    *p = (video_pacing){tick, ordinal, generation, fps};
    return 1;
}

int video_pacing_retime(video_pacing *p, uint32_t generation, uint32_t ordinal, uint32_t tick,
                        uint32_t next, uint32_t fps) {
    uint32_t span;
    if (!p || !next || (fps != 10 && fps != 15 && fps != 20) ||
        !video_pacing_span(fps, next >= ordinal ? next - ordinal : ordinal - next, &span)) return 0;
    return video_pacing_anchor(p, generation, next, next >= ordinal ? tick + span : tick - span, fps);
}

int video_pacing_deadline(const video_pacing *p, uint32_t generation, uint32_t ordinal, uint32_t *tick) {
    uint32_t span;
    if (!p || !tick || !p->fps || p->generation != generation || ordinal < p->ordinal ||
        !video_pacing_span(p->fps, ordinal - p->ordinal, &span)) return 0;
    *tick = p->tick + span;
    return 1;
}

#pragma clang section text=""
