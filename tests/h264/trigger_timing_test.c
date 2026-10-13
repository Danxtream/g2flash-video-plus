/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "../../patches/video/trigger_timing.h"

int main(void) {
    video_trigger_timing t = {0};
    video_trigger_record(&t, 1, 0, 20, 8, 6, 10, 0);
    assert(t.valid && t.wire_bound == 16 && t.lower == -8 && t.upper == 16);
    assert(t.samples == 1 && t.minimum == 16 && t.maximum == 16);
    uint32_t sent = UINT32_MAX - 5, received = 10;
    video_trigger_record(&t, 2, 62, received - sent, 8, 6, 10, 0);
    assert(t.valid && t.round_trip == 16 && t.minimum == 12 && t.maximum == 16);
    video_trigger_record(&t, 3, 125, 10, 12, 6, 10, 0);
    assert(t.valid && t.wire_bound == 4); /* Bounded endpoint rounding. */
    const uint32_t cases[][5] = {{20, 8, 6, 10, 1}, {20, 30, 6, 10, 0},
        {20, 8, 9, 10, 0}, {2001, 8, 6, 10, 0}, {20, 8, 6, 2001, 0}};
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        video_trigger_record(&t, 4 + i, 0, cases[i][0], cases[i][1], cases[i][2], cases[i][3], cases[i][4]);
        assert(!t.valid && t.wire_bound == UINT32_MAX && t.anomalies == i + 1 && t.samples == 3);
    }
    t.samples = t.anomalies = UINT32_MAX;
    video_trigger_record(&t, 20, 0, 20, 8, 6, 10, 0);
    assert(t.samples == UINT32_MAX && t.valid);
    video_trigger_record(&t, 21, 0, 20, 8, 6, 10, 1);
    assert(t.anomalies == UINT32_MAX && !t.valid);
    return 0;
}
