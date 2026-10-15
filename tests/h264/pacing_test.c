/* Rational clock and finite ordinal tests. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "../../patches/video/pacing.h"
int main(void) {
    video_pacing p = {0}; uint32_t tick;
    assert(!video_pacing_deadline(&p, 1, 1, &tick));
    assert(!video_pacing_anchor(&p, 0, 1, 0, 15));
    assert(!video_pacing_anchor(&p, 7, 1, 0, 16));
    for (uint32_t fps = 10; fps <= 20; fps += 5) {
        assert(video_pacing_anchor(&p, 7, 1, UINT32_MAX - 30, fps));
        for (uint32_t i = 0; i < 100000; ++i) {
            assert(video_pacing_deadline(&p, 7, i + 1, &tick));
            assert(tick == p.tick + i * 1000 / fps);
        }
        assert(!video_pacing_deadline(&p, 8, 2, &tick));
        assert(!video_pacing_deadline(&p, 7, UINT32_MAX, &tick));
        assert(video_pacing_anchor(&p, 7, UINT32_MAX - 2, 100, fps));
        assert(video_pacing_deadline(&p, 7, UINT32_MAX, &tick) && tick == 100 + 2000 / fps);
        assert(!video_pacing_deadline(&p, 7, 1, &tick));
    }
    return 0;
}
