/* Standard PHY ownership regressions. SPDX-License-Identifier: GPL-3.0-only */
#define main adapter_checks
#define VIDEO_RADIO_HERE() 1
#include "radio_test.c"
#undef main

static int owner_live = 1, deny_set, lose_set, lose_read, cancel_wait, disconnect_wait;
static int delay_mock(uint32_t ms) {
    tick += ms;
    if (cancel_wait) { owner_live = 0; cancel_wait = 0; }
    if (disconnect_wait) { event(VIDEO_RADIO_DM_CLOSE, 1, 0x123, 0, 0, 0); disconnect_wait = 0; }
    if (mail) dispatch();
    video_radio_state *s = &context.video_radio;
    if (s->pending == 2 && !(s->operation == VIDEO_RADIO_SET ? lose_set : lose_read)) {
        video_radio_link *l = &s->links[s->connection - 1];
        if (s->operation == VIDEO_RADIO_READ)
            event(VIDEO_RADIO_DM_READ, s->connection, l->handle, 0, l->tx, l->rx);
        else if (deny_set) event(VIDEO_RADIO_DM_UPDATE, s->connection, l->handle, 0x1a, l->tx, l->rx);
        else event(VIDEO_RADIO_DM_UPDATE, s->connection, l->handle, 0, s->tx, s->rx);
    }
    return 0;
}
#define VIDEO_OS_THREAD_NEW 0
#define VIDEO_POOL_DISPATCH 0
#define VIDEO_OS_DELAY delay_mock
#define VIDEO_RADIO_OWNER_VALID(ctx, generation) (owner_live && (generation))
#include "../../patches/video/radio_lifecycle.c"

static void fresh(uint8_t tx, uint8_t rx) {
    assert(!mail); memset(&context, 0, sizeof(context)); memset(ccbs, 0, sizeof(ccbs));
    visible = room = pool_room = allocation_ok = owner_live = 1;
    reads = sets = 0; tick = 100; deny_set = lose_set = lose_read = cancel_wait = disconnect_wait = 0;
    live(1, 0x123, 1); video_radio_bind(1);
    event(VIDEO_RADIO_DM_READ, 1, 0x123, 0, tx, rx);
}
static void settle(void) {
    for (uint32_t i = 0; i < 20; ++i) { delay_mock(50); video_radio_step(); }
    if (mail) dispatch();
}
int main(void) {
    fresh(1, 2); assert(video_radio_acquire(1, 0));
    video_radio_link *l = &context.video_radio.links[0];
    assert(reads == 1 && sets == 1 && l->before.tx == 1 && l->before.rx == 2);
    assert(video_radio_acquire(1, 0) && video_radio_acquire(2, 0) && l->owners == 2);
    unsigned n = sets; video_radio_release(1); settle(); assert(sets == n && l->owners == 1);
    video_radio_release(2); settle(); assert(sets == n + 1 && l->after.valid &&
        l->after.tx == 1 && l->after.rx == 2 && !l->permission && l->phase == VIDEO_RADIO_RESTORED);
    fresh(2, 2); assert(video_radio_acquire(1, 0)); video_radio_release(1); settle();
    assert(!sets && l->after.valid && l->after.tx == 2);
    fresh(1, 1); assert(video_radio_acquire(1, 1)); video_radio_release(1); settle();
    assert(!sets && !reads && l->after.valid);
    fresh(1, 1); assert(video_radio_acquire(1, 1));
    assert(!reads && !sets && !l->owners && !l->permission && !video_radio_generation_lost(1));
    event(VIDEO_RADIO_DM_CLOSE, 1, 0x123, 0, 0, 0);
    assert(video_radio_generation_lost(1));
    video_radio_release(1); settle(); assert(!reads && !sets);
    fresh(1, 1); assert(video_radio_acquire(1, 0) && video_radio_acquire(2, 1));
    video_radio_release(2); settle(); assert(sets == 1 && l->owners == 1);
    video_radio_release(1); settle(); assert(sets == 2 && l->after.tx == 1);
    fresh(1, 1); deny_set = 1; assert(video_radio_acquire(1, 0));
    assert(l->phase == VIDEO_RADIO_FALLBACK && reads == 2 && !l->permission);
    video_radio_release(1); settle(); assert(sets == 1 && l->after.valid);
    fresh(1, 1); assert(video_radio_acquire(1, 0)); lose_set = 1;
    video_radio_release(1); uint32_t end = l->deadline;
    assert(video_radio_acquire(2, 1)); video_radio_release(2);
    assert(l->deadline == end); lose_set = 0; settle();
    assert(l->phase == VIDEO_RADIO_RESTORED && l->after.valid);
    fresh(1, 1); room = 0; assert(!video_radio_acquire(1, 0)); room = 1;
    assert(!sets && !l->permission && !l->owners);
    fresh(1, 1); lose_read = 1; assert(!video_radio_acquire(1, 0));
    assert(!sets && !l->owners && !l->permission); lose_read = 0; settle();
    fresh(1, 1); cancel_wait = 1; assert(!video_radio_acquire(1, 0)); settle();
    assert(!sets && !l->permission && !l->owners);
    fresh(1, 1); disconnect_wait = 1; assert(!video_radio_acquire(1, 0)); settle();
    assert(!l->connected && !l->permission);
    fresh(1, 1); assert(video_radio_acquire(1, 0));
    event(VIDEO_RADIO_DM_CLOSE, 1, 0x123, 0, 0, 0); assert(video_radio_generation_lost(1));
    live(1, 0x123, 1); video_radio_bind(1); assert(video_radio_generation_lost(1));
    video_radio_release(1); settle(); assert(!l->permission && sets == 1);
    fresh(1, 1); assert(video_radio_acquire(1, 0)); lose_set = 1;
    video_radio_release(1); delay_mock(10); video_radio_step();
    tick += VIDEO_RADIO_RESTORE_MS; video_radio_step();
    assert(l->uncertain && !l->permission && !l->after.valid && !video_radio_acquire(2, 0));
    lose_set = 0; settle(); assert(l->uncertain); /* Late completion cannot clear quarantine. */
    unsigned unchanged = sets; assert(video_radio_acquire(3, 1));
    video_radio_release(3); assert(sets == unchanged && l->uncertain);
    fresh(1, 1); live(2, 0x456, 1); video_radio_bind(2);
    assert(!video_radio_acquire(1, 0) && !sets);
    fresh(1, 1); event(VIDEO_RADIO_DM_CLOSE, 1, 0x123, 0, 0, 0);
    assert(video_radio_acquire(1, 0)); video_radio_release(1); assert(!reads && !sets);
    assert(!mail && !depth);
    return 0;
}
