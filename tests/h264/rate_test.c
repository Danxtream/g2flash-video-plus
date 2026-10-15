/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../../patches/video/rate.c"
static uint32_t word(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
int main(void) {
    video_rate_state s; uint8_t reply[144];
    for (uint32_t fps = 10; fps <= 20; fps += 5) {
    for (uint32_t anchor = 0; ; anchor = UINT32_MAX - 100) {
        uint32_t period = (1000 + fps - 1) / fps;
        video_rate_start(&s, 8, 9, 10, period);
        video_rate_copy(&s, anchor, 1, 0);
        for (uint32_t i = 1; i <= fps * 10; ++i) {
            video_rate_present(&s, anchor + i * 1000 / fps, 1, 0);
            video_rate_copy(&s, anchor + i * 1000 / fps, i + 1, 0);
        }
        assert(s.high == 10 && s.rows[9 % 8].shown == fps);
        assert(video_rate_snapshot(&s, 0, 1, 1, reply));
        assert(reply[28] == 2 && reply[29] == 56 && word(reply + 32) == 3);
        uint32_t phases = 0;
        for (uint32_t j = 0; j < 10; ++j) phases += (word(reply + 80 + (j >= 8 ? 4 : 0)) >> ((j % 8) * 4)) & 15;
        assert(phases == fps);
        assert(word(reply + 40) == fps && word(reply + 44) <= period);
        assert(!s.dip_high && !(s.flags & VIDEO_RATE_INVALID));
        assert(video_rate_snapshot(&s, 2, 0, 1, reply) && word(reply + 24) == fps * 10 && reply[28] == 16);
        assert(word(reply + 96) == period);
        video_rate_stop(&s, anchor + 10010);
        uint32_t high = s.high;
        video_rate_present(&s, anchor + 30000, 0, 0); assert(s.high == high);
        if (anchor) break;
    }
    }
    video_rate_start(&s, 8, 9, 10, 67); video_rate_copy(&s, 0, 1, 0);
    video_rate_post_worker(&s, 0, VIDEO_RATE_DECODE);
    video_rate_post_worker(&s, 80, VIDEO_RATE_FRAME);
    video_rate_post_worker(&s, 80, VIDEO_RATE_OUTPUT);
    video_rate_present(&s, 500, 0, 0);
    video_rate_copy(&s, 1000, 2, 0);
    assert(s.rows[0].shown == 1 && s.rows[0].service == 80 && s.rows[0].output == 920);
    assert(s.rows[0].overrun == 1 && s.rows[0].decode_max == 80);
    assert(s.dips[0].gap == 1000 && s.dips[0].flags & VIDEO_RATE_SLOW);
    assert(video_rate_snapshot(&s, 1, 0, 2, reply) && reply[6] == 2 && reply[29] == 32);
    video_rate_present(&s, 1500, 0, 1); video_rate_present(&s, 2000, 0, 0);
    assert(s.rows[1].flags & VIDEO_RATE_PAUSED);
    video_rate_copy(&s, 2100, 3, 1); uint32_t high = s.high;
    video_rate_present(&s, 10000, 0, 0); assert(s.high == high);
    video_rate_start(&s, 9, 10, 11, 67); video_rate_copy(&s, 10, 1, 0);
    for (uint32_t i = 0; i <= VIDEO_RATE_POSTS; ++i) video_rate_post_worker(&s, 11 + i, VIDEO_RATE_FRAME);
    assert(s.flags & VIDEO_RATE_INVALID);
    video_rate_present(&s, 100010, 0, 0); assert(s.high >= 99);
    assert(video_rate_snapshot(&s, 0, 1, 1, reply) && reply[7] & VIDEO_RATE_INVALID);
    assert(!video_rate_snapshot(&s, 3, 0, 1, reply));
    printf("cadence windows, waits, finite history, terminal/pause and tick wrap PASS; resident %zu B\n", sizeof(s));
}
