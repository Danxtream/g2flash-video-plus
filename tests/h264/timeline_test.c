/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include "../../patches/video/timeline.c"

int main(void) {
    _Static_assert(VIDEO_TIMELINE_RECORDS == 12, "Twelve independent picture records");
    video_timeline s = {0};
    video_frame_time out = {0};
    const uint16_t rates[] = {10, 15, 20, 24};
    for (uint32_t r = 0; r < 4; ++r) {
        uint16_t fps = rates[r];
        assert(video_timeline_configure(&s, 1, 1000, 0, fps, 1));
        assert(s.fps == fps);
    }
    assert(!video_timeline_configure(&s, 1, 1000, 0, 30, 1));
    assert(!video_timeline_configure(&s, 1, 1000, 0, 15, 2));
    assert(!video_timeline_configure(&s, 0, 1000, 0, 15, 1));
    assert(!video_timeline_configure(&s, 1, 1000, 1001, 15, 1));
    assert(!video_timeline_configure(&s, 1, 1000, 0, 16, 1));
    assert(video_timeline_configure(&s, 1, 1000, 0, 15, 1));
    assert(!video_timeline_start(&s, 2, 7));
    assert(!video_timeline_start(&s, 1, 0));
    assert(video_timeline_start(&s, 1, 7));
    assert(!video_timeline_configure(&s, 2, 1000, 0, 15, 1));
    for (uint32_t i = 1; i <= VIDEO_TIMELINE_RECORDS; ++i) {
        video_frame_time f = {i, 2 * i, ((i - 1) * 1000 + 7) / 15};
        assert(!video_timeline_offer(&s, 1, 8, &f));
        assert(video_timeline_offer(&s, 1, 7, &f));
        assert(video_timeline_offer(&s, 1, 7, &f));
        f.pts++;
        assert(!video_timeline_offer(&s, 1, 7, &f));
    }
    const uint32_t last = VIDEO_TIMELINE_RECORDS + 1, pts = (VIDEO_TIMELINE_RECORDS * 1000 + 7) / 15;
    video_frame_time next = {last, 2 * last, pts};
    assert(!video_timeline_offer(&s, 1, 7, &next));
    assert(!video_timeline_take(&s, 8, 1, 2, &out));
    assert(!video_timeline_take(&s, 7, 1, 1, &out));
    assert(video_timeline_take(&s, 7, 1, 2, &out));
    assert(out.ordinal == 1 && out.nal_sequence == 2 && out.pts == 0);
    assert(video_timeline_offer(&s, 1, 7, &next));
    for (uint32_t i = 2; i <= last; ++i) {
        assert(video_timeline_take(&s, 7, i, 2 * i, &out));
        assert(out.ordinal == i);
    }
    assert(!video_timeline_take(&s, 7, last + 1, 2 * (last + 1), &out));
    next = (video_frame_time){last + 1, 2 * last - 1, pts + 10};
    assert(!video_timeline_offer(&s, 1, 7, &next));
    next = (video_frame_time){last + 1, 2 * (last + 1), pts - 1};
    assert(!video_timeline_offer(&s, 1, 7, &next));
    next = (video_frame_time){last + 1, 2 * (last + 1), 1001};
    assert(!video_timeline_offer(&s, 1, 7, &next));
    video_timeline_stop(&s, 8);
    assert(s.active);
    s.accepted = UINT32_MAX;
    next = (video_frame_time){1, 12, 260};
    assert(!video_timeline_offer(&s, 1, 7, &next));
    video_timeline_stop(&s, 7);
    assert(!s.active && !s.configured && !s.count);
    assert(video_timeline_configure(&s, 2, 1000, 500, 15, 1));
    assert(video_timeline_start(&s, 2, 8));
    next = (video_frame_time){1, 4, 500};
    assert(video_timeline_offer(&s, 2, 8, &next));
    video_timeline_stop(&s, 7);
    assert(s.active);
    puts("Bounded picture timelines, duplicate/conflict, NAL independence and stale generations PASS");
    video_timeline_stop(&s, 8);
    assert(video_timeline_configure(&s, 3, 1000, 125, 15, 1));
    assert(video_timeline_start(&s, 3, 9));
    next = (video_frame_time){1, 2, 0};
    assert(!video_timeline_offer(&s, 3, 9, &next));
    assert(!video_timeline_offer_preroll(&s, 3, 9, &next, 256));
    assert(video_timeline_offer_preroll(&s, 3, 9, &next, 2));
    next = (video_frame_time){2, 3, 62};
    assert(video_timeline_offer_preroll(&s, 3, 9, &next, 2));
    next = (video_frame_time){3, 4, 124};
    assert(!video_timeline_offer_preroll(&s, 3, 9, &next, 2));
    next.pts = 125;
    assert(video_timeline_offer_preroll(&s, 3, 9, &next, 2));
    return 0;
}
