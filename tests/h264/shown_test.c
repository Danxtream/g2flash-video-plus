/* Nonblocking ring overwrite and wrap evidence. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "../../patches/video/shown.h"
int main(void) {
    video_shown_ring r = {0}; video_shown_frame row; uint32_t serial;
    assert(!video_shown_read(&r, 0, &row, &serial));
    for (uint32_t n = 1; n <= 50; ++n) {
        video_shown_frame shown = {n, n * 63, UINT32_MAX - 100 + n * 63,
                                  UINT32_MAX - 110 + n * 63, n * 63, 0};
        assert(video_shown_publish(&r, &shown));
    }
    assert(r.high == 50 && r.first == 35 && r.lost == 34);
    assert(video_shown_read(&r, 1, &row, &serial) && serial == 35 && row.ordinal == 35);
    assert((uint32_t)(row.copy_tick - row.trigger_tick) == 10);
    assert(video_shown_read(&r, 50, &row, &serial) && serial == 50);
    assert(!video_shown_read(&r, 51, &row, &serial));
    assert(!video_shown_publish(&r, &row));
    r.high = UINT32_MAX; row.ordinal = 51;
    assert(!video_shown_publish(&r, &row));
    return 0;
}
