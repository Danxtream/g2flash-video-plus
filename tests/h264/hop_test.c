/* Synthetic clocks and transport identities. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../../patches/video/hop.h"
#define bzero(p, n) memset(p, 0, n)
typedef struct {
    struct { uint32_t start_guard, stream; uint8_t owner_origin; } video_control;
    struct { uint32_t token; } video;
    struct { video_hop_state hop; } video_telemetry;
} customCfwContext;
static customCfwContext context;
static customCfwContext *peekCustomCfwContext(void) { return &context; }
static uint32_t now;
static uint32_t fake_tick(void) { return now; }
#define VIDEO_TICK fake_tick()
#define VIDEO_CONTROL_ACCEPTED 0
#define VIDEO_CONTROL_STALE 2
#define VIDEO_CONTROL_BUSY 3
#define VIDEO_CONTROL_FORMAT 1
static uint32_t video_read32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void video_write32(uint8_t *p, uint32_t n) { for (uint32_t i = 0; i < 4; ++i) p[i] = n >> (i * 8); }
static int publish_before_claim;
static int test_claim(volatile uint32_t *word, uint32_t *expected, uint32_t desired, int weak, int success, int failure) {
    if (publish_before_claim) {
        publish_before_claim = 0;
        context.video_telemetry.hop.generation = context.video_control.start_guard;
        context.video_telemetry.hop.rows[0].count = 123;
    }
    return __atomic_compare_exchange_n(word, expected, desired, weak, success, failure);
}
#define __atomic_compare_exchange_n test_claim
#include "../../patches/video/hop.c"
#undef __atomic_compare_exchange_n
int main(void) {
    uint8_t data[6] = {31, 26, 7}, out[VIDEO_HOP_BYTES] = {0};
    cfw_message_stream stream = {.stream_id = 240, .options = 3, .hop_rx = 100, .hop_forward = 101};
    video_hop_state *s = &context.video_telemetry.hop;
    now = 103;
    assert(!video_hop_tick());
    video_hop_open(&stream, 1, 1, data); video_hop_event(240 | 1u << 24, VIDEO_HOP_LOCAL, 0);
    assert(!s->generation && !s->busy && !s->rows[3].count);
    for (uint32_t origin = 1; origin <= 2; ++origin) {
        memset(&context, 0, sizeof(context));
        context.video_control.start_guard = origin; context.video_control.stream = 7;
        context.video_control.owner_origin = origin; context.video.token = 99;
        stream.message_id = 0; stream.checksum = 0; stream.hop_rx = 100; stream.hop_forward = 101; now = 103;
        assert(video_hop_tick() == now);
        uint32_t key = 240 | origin << 24;
        video_hop_open(&stream, origin, origin, data);
        stream.message_id = 1; video_hop_open(&stream, origin, origin, data);
        assert(s->pending == 2 && s->maximum == 2);
        now = 110; video_hop_event(key, VIDEO_HOP_WIRE, 3);
        now = 118; video_hop_event(key, VIDEO_HOP_LOCAL, 0);
        now = 150; video_hop_event(key, VIDEO_HOP_PEER, 0);
        now = 151; video_hop_event(key, VIDEO_HOP_RELAY, 0);
        video_hop_event(key, VIDEO_HOP_RELAY, 0);
        assert(s->rows[0].sum == 2 && s->rows[1].sum == 3 && s->rows[2].sum == 10);
        assert(s->rows[3].sum == 8 && s->rows[4].sum == 49 && s->rows[5].sum == 1);
        assert(s->rows[6].sum == 51 && s->rows[6].count == 1 && s->pending == 1);
        assert(!video_hop_snapshot(origin, origin, 7, out));
        assert(out[16] == 1 && out[17] == 2 && video_read32(out + 24) == 50);
        assert(video_hop_snapshot(origin, origin, 8, out) == VIDEO_CONTROL_STALE);
        /* ACK history is a late upper bound and closes only the matching key. */
        video_hop_event(key | 1u << 8, VIDEO_HOP_WIRE, 1);
        video_hop_event(key | 1u << 8, VIDEO_HOP_PEER, 2);
        video_hop_event(key | 1u << 8, VIDEO_HOP_RELAY, 0);
        assert(!s->pending && (s->flags & 16));
        video_hop_event(key | 1u << 8, VIDEO_HOP_LOCAL, 0);
        assert(s->rows[6].count == 2);
        /* A bridge peer and a directly addressed lens have no forwarded work. */
        ++context.video_control.start_guard; stream.message_id = 2;
        assert(video_hop_tick() == now);
        video_hop_open(&stream, origin, origin ^ 3, data);
        video_hop_event(key | 2u << 8, VIDEO_HOP_WIRE, 0);
        video_hop_event(key | 2u << 8, VIDEO_HOP_LOCAL, 0);
        assert(!s->pending && !s->maximum && !s->rows[4].count && s->rows[6].count == 1);
        /* No waiting on a contested writer, and no silent histogram overflow. */
        s->busy = 1; assert(video_hop_snapshot(origin, origin, 7, out) == VIDEO_CONTROL_BUSY);
        assert(s->dropped == 1); s->busy = 0;
        s->rows[0].sum = UINT32_MAX; s->rows[0].bins[0] = UINT16_MAX;
        video_hop_sample(s, 0, 1); assert((s->flags & 1) && s->rows[0].sum == UINT32_MAX);
        /* Tick wrap preserves durations. */
        ++context.video_control.start_guard; stream.hop_rx = UINT32_MAX - 9; stream.hop_forward = UINT32_MAX - 8;
        now = UINT32_MAX - 3; assert(video_hop_tick() == now); video_hop_open(&stream, origin, origin, data);
        now = 5; video_hop_event(key | 2u << 8, VIDEO_HOP_WIRE, 0);
        assert(s->rows[2].sum == 15);
        context.video_control.start_guard = 0;
        assert(!video_hop_tick());
        video_hop_event(key | 2u << 8, VIDEO_HOP_PEER, 0);
        video_hop_event(key | 2u << 8, VIDEO_HOP_RELAY, 0);
        now = 1000; assert(!video_hop_snapshot(origin, origin, 7, out));
        assert(video_read32(out + 20) == 15);
        /* A complete peer ACK can beat local body classification. */
        context.video_control.start_guard = origin + 10; now = 100;
        assert(video_hop_tick() == now); stream.hop_rx = 100; stream.hop_forward = 101;
        stream.checksum = 0x3456; now = 120;
        video_hop_event(key | 2u << 8, VIDEO_HOP_PEER, 0x34560000);
        now = 122; video_hop_event(key | 2u << 8, VIDEO_HOP_RELAY, 0x34560000);
        now = 125; video_hop_open(&stream, origin, origin, data);
        video_hop_event(key | 2u << 8, VIDEO_HOP_WIRE, 4);
        now = 128; video_hop_event(key | 2u << 8, VIDEO_HOP_LOCAL, 0);
        assert(!s->pending && s->maximum == 1 && s->rows[4].sum == 19 && s->rows[5].sum == 2 && s->rows[6].sum == 28);
        assert((s->flags & 32) && !(s->flags & 4));
        /* Another CRC at a reused transport identity cannot confirm this NAL. */
        ++context.video_control.start_guard; assert(video_hop_tick() == now);
        video_hop_event(key | 2u << 8, VIDEO_HOP_PEER, 0x77770000);
        video_hop_open(&stream, origin, origin, data);
        assert(s->pending == 1 && !s->rows[4].count);
        video_hop_event(key | 2u << 8, VIDEO_HOP_PEER, 0x77770000);
        assert(s->pending == 1 && (s->flags & 128));
        /* Generation reset never clears a lock it did not acquire. */
        ++context.video_control.start_guard; s->busy = 1;
        uint32_t generation = s->generation, dropped = s->dropped;
        assert(!video_hop_tick() && s->busy == 1 && s->generation == generation && s->dropped == dropped + 1);
        s->busy = 0; assert(video_hop_tick());
        assert(s->generation == context.video_control.start_guard && !s->dropped && !s->pending);
        ++context.video_control.start_guard; publish_before_claim = 1;
        assert(video_hop_tick() && s->rows[0].count == 123);
    }
    return 0;
}
