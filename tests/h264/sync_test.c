/* Two-lens FIFO completion protocol. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include "../../patches/video/sync.h"
int main(void) {
    video_sync left, right;
    assert(video_sync_init(&left, 9, 1) && video_sync_init(&right, 9, 2));
    assert(!video_sync_init(&right, 0, 2));
    assert(!video_sync_trigger(&right));
    assert(video_sync_ready(&right, 1, 0, 2, 0));
    assert(video_sync_receive(&right, 1, 9, VIDEO_SYNC_READY, 1, 0, 1, 0));
    assert(!video_sync_trigger(&right));
    for (uint32_t n = 1; n <= 10000; ++n) {
        uint32_t pts = (n - 1) * 1000 / 16;
        assert(video_sync_ready(&right, n, pts, 2, 0));
        assert(video_sync_ready(&left, n, pts, 2, 0));
        assert(video_sync_receive(&right, 1, 9, VIDEO_SYNC_READY, n, pts, 2, 0));
        assert(!video_sync_receive(&right, 2, 9, VIDEO_SYNC_READY, n, pts, 2, 0));
        assert(!video_sync_receive(&right, 1, 8, VIDEO_SYNC_READY, n, pts, 2, 0));
        assert(video_sync_trigger(&right));
        assert(!video_sync_trigger(&right));
        assert(video_sync_receive(&left, 2, 9, VIDEO_SYNC_TRIGGER, n, pts, 0, 0));
        assert(video_sync_receive(&left, 2, 9, VIDEO_SYNC_TRIGGER, n, pts, 0, 0));
        assert(video_sync_complete(&right, n));
        assert(!video_sync_trigger(&right)); /* Faster coordinator cannot advance. */
        assert(video_sync_complete(&left, n));
        assert(!video_sync_complete(&left, n));
        assert(video_sync_ready(&right, n + 1, pts + 63, 2, 0));
        assert(video_sync_receive(&right, 1, 9, VIDEO_SYNC_DONE, n, pts, 0, 0));
        assert(video_sync_receive(&right, 1, 9, VIDEO_SYNC_DONE, n, pts, 0, 0));
        assert(!video_sync_trigger(&right)); /* No matching next peer readiness. */
    }
    assert(!video_sync_receive(&left, 2, 9, VIDEO_SYNC_TRIGGER, left.next + 1, 0, 0, 0));
    assert(video_sync_init(&right, 11, 2));
    right.next = UINT32_MAX; right.inflight = right.completed = right.peer_completed = UINT32_MAX;
    assert(!video_sync_trigger(&right) && right.fault);
    assert(video_sync_init(&right, 12, 2));
    assert(video_sync_ready(&right, 1, 0, 1, 1));
    assert(video_sync_receive(&right, 1, 12, VIDEO_SYNC_READY, 1, 0, 1, 1));
    assert(video_sync_trigger(&right));
    return 0;
}
