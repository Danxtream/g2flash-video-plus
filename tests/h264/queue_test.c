/* Owned snapshots with fake consumers. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../patches/video/queue.c"

int main(void) {
    video_nal_queue q, other;
    uint8_t slots[VIDEO_QUEUE_INITIAL][VIDEO_SLOT_BYTES];
    uint8_t peer[VIDEO_QUEUE_INITIAL][VIDEO_SLOT_BYTES];
    uint8_t nal[VIDEO_RAW_NAL_BYTES];
    memset(nal, 0x55, sizeof(nal)); nal[0] = 0x67;
    video_queue_init(&q, 7); video_queue_init(&other, 9);
    for (uint32_t i = 0; i < VIDEO_QUEUE_INITIAL; ++i) {
        assert(video_queue_attach(&q, slots[i]));
        assert(video_queue_attach(&other, peer[i]));
    }
    assert(!video_queue_attach(&q, slots[0]));
    assert(!video_queue_push(&q, 0, nal, 0));
    assert(!video_queue_push(&q, 0, nal, VIDEO_RAW_NAL_BYTES + 1));
    for (uint32_t i = 0; i < VIDEO_QUEUE_INITIAL; ++i) {
        nal[1] = i;
        assert(video_queue_push(&q, i, nal, sizeof(nal)));
    }
    memset(nal, 0xee, sizeof(nal));
    video_nal_queue frozen = q;
    assert(!video_queue_push(&q, 4, nal, sizeof(nal)));
    assert(!memcmp(&q, &frozen, sizeof(q)));
    assert(q.accepted == 4 && !q.consumed && !other.count);
    assert(video_queue_push(&other, 30, nal, 1) && other.count == 1);
    video_nal_view view, stale;
    assert(!video_queue_claim(&q, 8, &view));
    for (uint32_t i = 0; i < VIDEO_QUEUE_INITIAL; ++i) {
        assert(video_queue_claim(&q, 7, &view));
        assert(view.sequence == i && view.length == sizeof(nal) &&
               view.bytes[0] == 0x67 && view.bytes[1] == i && view.bytes[2] == 0x55);
        assert(!video_queue_claim(&q, 7, &stale));
        stale = view; ++stale.token;
        assert(!video_queue_release(&q, &stale));
        stale = view; ++stale.sequence;
        assert(!video_queue_release(&q, &stale));
        assert(video_queue_release(&q, &view));
        assert(!video_queue_release(&q, &view));
    }
    assert(!q.count && q.consumed == 4 && q.accepted == 4 && other.count == 1);
    assert(!video_queue_claim(&q, 7, &view));
    /* Parameter sets and slice NALs each use credits, without inventing pictures. */
    const uint8_t headers[] = {0x67, 0x68, 0x65, 0x41};
    for (uint32_t i = 0; i < 4; ++i)
        assert(video_queue_push(&q, i + 4, headers + i, 1));
    assert(q.count == 4 && q.accepted == 8 && q.consumed == 4);
    puts("owned bounds, borrowed overwrite, full rollback, consumer lifetime and lens isolation PASS");
}
