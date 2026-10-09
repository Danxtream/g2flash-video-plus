/* Owned snapshots with fake consumers. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../../patches/video/queue.c"

static void ordering(void) {
    const uint8_t permutations[6][3] = {{1,2,3},{1,3,2},{2,1,3},{2,3,1},{3,1,2},{3,2,1}};
    uint8_t slots[VIDEO_QUEUE_INITIAL][VIDEO_SLOT_BYTES], extension[2 * VIDEO_SLOT_BYTES];
    const uint8_t nal[] = {0x41, 0x55};
    video_nal_queue q;
    video_nal_view view;
    for (uint32_t permutation = 0; permutation < 6; ++permutation) {
        video_queue_init(&q, 17);
        for (uint32_t i = 0; i < 4; ++i) assert(video_queue_attach(&q, slots[i]));
        for (uint32_t i = 0; i < 3; ++i)
            assert(video_queue_push(&q, permutations[permutation][i], nal, 2) == 1);
        assert(video_queue_credits(&q) == 1 && !video_queue_claim(&q, 17, &view));
        assert(!video_queue_push(&q, 4, nal, 2));
        assert(video_queue_push(&q, 0, nal, 2) == 1);
        assert(!video_queue_credits(&q) && q.accepted == 4);
        assert(video_queue_push(&q, 2, nal, 2) == 2 && q.accepted == 4);
        const uint8_t conflict[] = {0x41, 0x56};
        assert(video_queue_push(&q, 2, conflict, 2) == VIDEO_QUEUE_CONFLICT);
        for (uint32_t i = 0; i < 4; ++i) {
            assert(video_queue_claim(&q, 17, &view) && view.sequence == i);
            assert(video_queue_release(&q, &view));
            assert(q.expected == i + 1 && q.consumed == i + 1);
            assert(video_queue_push(&q, i, nal, 2) == 2 && q.accepted == 4);
            assert(video_queue_push(&q, i, conflict, 2) == VIDEO_QUEUE_CONFLICT);
        }
        uint32_t overwritten = q.slots[0].sequence;
        assert(video_queue_push(&q, 4, nal, 2) == 1);
        assert(video_queue_push(&q, overwritten, nal, 2) == VIDEO_QUEUE_STALE);
        assert(video_queue_extend(&q, extension) && q.capacity == 6);
        assert(!video_queue_extend(&q, extension));
        assert(video_queue_push(&q, 9, nal, 2) == 1);
        assert(!video_queue_push(&q, 10, nal, 2));
    }
    video_queue_init(&q, 19);
    for (uint32_t i = 0; i < 4; ++i) assert(video_queue_attach(&q, slots[i]));
    q.expected = UINT32_MAX - 1;
    assert(video_queue_push(&q, UINT32_MAX - 1, nal, 2) == 1);
    assert(video_queue_claim(&q, 19, &view) && video_queue_release(&q, &view));
    assert(q.expected == UINT32_MAX && video_queue_push(&q, UINT32_MAX, nal, 2) == VIDEO_QUEUE_WRAP);
    q.accepted = UINT32_MAX;
    assert(video_queue_push(&q, UINT32_MAX - 1, nal, 2) == 2);
    puts("all future permutations, reserved expected slot, exact retry history and wrap refusal PASS");
}

static void recovery(void) {
    video_nal_queue q;
    uint8_t slots[4][VIDEO_SLOT_BYTES];
    const uint8_t headers[] = {0x67, 0x68, 0x65, 0x41};
    for (uint32_t wrap = 0; wrap < 2; ++wrap) {
        uint32_t now = wrap ? UINT32_MAX - 2499 : 100;
        video_queue_init(&q, 29);
        for (uint32_t i = 0; i < 4; ++i) assert(video_queue_attach(&q, slots[i]));
        assert(video_queue_push(&q, 2, headers + 2, 1) == 1);
        assert(video_queue_watch(&q, now) && video_queue_headers(&q));
        uint32_t deadline = q.gap_deadline;
        assert(deadline && q.gap_sequence == 0 && !q.header_progress);
        assert(video_queue_push(&q, 3, headers + 3, 1) == 1);
        assert(video_queue_push(&q, 2, headers + 2, 1) == 2);
        assert(video_queue_watch(&q, now + 100) && q.gap_deadline == deadline);
        assert(video_queue_push(&q, 0, headers, 1) == 1);
        assert(video_queue_headers(&q) && q.header_progress == VIDEO_HEADERS_SPS);
        assert(video_queue_watch(&q, now + 200) && q.gap_sequence == 1 && q.gap_deadline == deadline);
        assert(video_queue_watch(&q, deadline - 1));
        assert(!video_queue_watch(&q, deadline));
        /* Even a late complete queue cannot erase the expired retry deadline. */
        assert(video_queue_push(&q, 1, headers + 1, 1) == 1);
        assert(!video_queue_watch(&q, deadline));
        video_queue_init(&q, 30);
        for (uint32_t i = 0; i < 4; ++i) assert(video_queue_attach(&q, slots[i]));
        assert(video_queue_push(&q, 2, headers + 2, 1) == 1);
        assert(video_queue_watch(&q, now));
        assert(video_queue_push(&q, 0, headers, 1) == 1);
        assert(video_queue_push(&q, 1, headers + 1, 1) == 1);
        assert(video_queue_headers(&q) && q.header_progress == 7 && q.header_sequence == 3);
        assert(video_queue_watch(&q, now + 1) && !q.gap_deadline);
    }
    for (uint32_t type = 0; type < 3; ++type) {
        video_queue_init(&q, 31);
        for (uint32_t i = 0; i < 4; ++i) assert(video_queue_attach(&q, slots[i]));
        assert(video_queue_push(&q, 0, headers + type + 1, 1) == 1);
        assert(!video_queue_headers(&q)); /* PPS, IDR and P need predecessors. */
    }
    video_queue_init(&q, 32);
    for (uint32_t i = 0; i < 4; ++i) assert(video_queue_attach(&q, slots[i]));
    const uint8_t aud = 9, sei = 6;
    assert(video_queue_push(&q, 0, &aud, 1) == 1 && video_queue_headers(&q));
    assert(video_queue_push(&q, 1, &sei, 1) == 1 && video_queue_headers(&q));
    assert(q.header_sequence == 2 && !q.header_progress && !q.pictures);
    puts("gap retry deadline, tick wrap, fresh parameter order and nonpicture headers PASS");
}

int main(void) {
    ordering();
    recovery();
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
    assert(video_queue_push(&other, 0, nal, 1) && other.count == 1);
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
