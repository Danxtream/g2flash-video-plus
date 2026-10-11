/* Epoch and bounded command regressions. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#ifdef VIDEO_RADIO_PLATFORM_TEST
#include <sys/mman.h>
#endif
#include "../../patches/cfw_context.h"

static customCfwContext context;
static int visible = 1, room = 1, pool_room = 1, allocation_ok = 1;
static unsigned dm_calls, app_calls, allocations, reads, sets, depth;
static uint32_t tick;
static uint8_t ccbs[3][48], packet[10];
static void *mail;
static customCfwContext *mock_context(void) { return visible ? &context : 0; }
static const uint8_t *mock_ccb(uint8_t id) { assert(id >= 1 && id <= 3); return ccbs[id-1]; }
static void lock(void) { ++depth; }
static void unlock(void) { assert(depth); --depth; }
static void stock_dm(void *p) { (void)p; assert(!depth); ++dm_calls; }
static void stock_app(uint8_t mask, void *p) { (void)mask; (void)p; ++app_calls; }
static int available(uint32_t bytes, video_radio_trace *trace) {
    assert(depth && bytes <= 20);
    trace->required = bytes + 8;
    trace->first = trace->selected = (video_radio_pool_sample){0, pool_room ? 4 : 3, 32};
    trace->reason = pool_room ? VIDEO_RADIO_REFUSAL_NONE : VIDEO_RADIO_REFUSAL_POOL_RESERVE;
    return pool_room;
}
static void *allocate(uint16_t bytes) {
    ++allocations; assert(depth && !mail && bytes == 20);
    return allocation_ok ? calloc(1, bytes) : 0;
}
static void send(uint8_t id, void *p) { assert(depth && id == 7); mail = p; }
static uint8_t *command_allocate(uint16_t opcode, uint8_t bytes) {
    assert(depth && opcode == 0x2030 && bytes == 2); memset(packet, 0, sizeof(packet));
    return allocation_ok ? packet : 0;
}
static void command_send(uint8_t *p) { assert(!depth && p == packet); ++reads; }
static void set_phy(uint8_t id, uint8_t all, uint8_t tx, uint8_t rx, uint16_t options) {
    assert(depth && id == 1 && !all && tx && rx && !options); ++sets;
}
static unsigned radio_notifications;
static void radio_notify(void) { assert(!depth); ++radio_notifications; }
#define VIDEO_RADIO_NOTIFY radio_notify
#define VIDEO_RADIO_HERE() 1
#define VIDEO_RADIO_CONTEXT mock_context
#define VIDEO_RADIO_TICK tick
#define VIDEO_RADIO_LOCK lock
#define VIDEO_RADIO_UNLOCK unlock
#define VIDEO_RADIO_CCB mock_ccb
#define VIDEO_RADIO_STOCK_DM stock_dm
#define VIDEO_RADIO_STOCK_APP stock_app
#define VIDEO_RADIO_POOL_ROOM available
#define VIDEO_RADIO_HANDLER_ID() 7
#define VIDEO_RADIO_COMMAND_ROOM() room
#define VIDEO_RADIO_ALLOC allocate
#define VIDEO_RADIO_SEND send
#define VIDEO_RADIO_CMD_ALLOC command_allocate
#define VIDEO_RADIO_CMD_SEND command_send
#define VIDEO_RADIO_SET_PHY set_phy
#include "../../patches/video/radio.c"

static void dispatch(void) {
    assert(mail); void *p = mail; mail = 0;
    video_radio_app_handler(0, p); free(p); /* WSF dispatcher owns this free. */
}
static void live(uint8_t id, uint16_t handle, uint8_t role) {
    uint8_t *p = ccbs[id - 1]; memset(p, 0, 48);
    p[12] = handle; p[13] = handle >> 8; p[16] = id; p[22] = 1; p[25] = role;
}
static void event(uint8_t kind, uint8_t id, uint16_t handle, uint8_t status, uint8_t tx, uint8_t rx) {
    uint8_t data[10] = {id, 0, kind, status, status, 0, handle, handle >> 8, tx, rx};
    video_radio_dm_event(data);
}
static void check_donor_role_binding(void) {
    static const uint8_t roles[] = {0, 1, 2, 0xff};
    unsigned old_allocations = allocations, old_reads = reads, old_sets = sets;
    for (unsigned r = 0; r < sizeof(roles); ++r) {
        for (uint8_t address_type = 0; address_type < 2; ++address_type) {
            memset(&context, 0, sizeof(context)); memset(ccbs, 0, sizeof(ccbs));
            live(1, 0x123, roles[r]); ccbs[0][19] = address_type;
            event(VIDEO_RADIO_DM_OPEN, 1, 0x123, 0, 0, 0);
            video_radio_link *link = &context.video_radio.links[0];
            assert(link->connected == (roles[r] == 1) && !link->bound);
            video_radio_bind(1);
            assert(link->connected == (roles[r] == 1));
            assert(link->bound == (roles[r] == 1));
            assert(!mail && !depth);
        }
    }
    memset(&context, 0, sizeof(context)); memset(ccbs, 0, sizeof(ccbs));
    live(1, 0x123, 1); ccbs[0][22] = 0;
    event(VIDEO_RADIO_DM_OPEN, 1, 0x123, 0, 0, 0); video_radio_bind(1);
    assert(!context.video_radio.links[0].connected && !context.video_radio.links[0].bound);
    assert(allocations == old_allocations && reads == old_reads && sets == old_sets);
    assert(!mail && !depth);
}
static void check_refusal_evidence(void) {
    memset(&context, 0, sizeof(context)); memset(ccbs, 0, sizeof(ccbs));
    live(1, 0x123, 1); video_radio_bind(1); tick = 500;
    video_radio_state *s = &context.video_radio;
    uint8_t report[VIDEO_RADIO_REPORT_BYTES], frozen[VIDEO_RADIO_REPORT_BYTES];
    unsigned old_allocations = allocations, old_reads = reads, old_sets = sets;
    assert(video_radio_refusal_snapshot(report) && report[3] == 2 && report[12] == 1);
    assert(report[9] == 32 && report[10] == 2 && report[13] == 3 && report[14] == 1);
    assert(!s->attempt_counter && !mail && allocations == old_allocations);
    pool_room = 0;
    assert(!video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    assert(s->attempt_counter == 1 && s->last_refusal.reason == VIDEO_RADIO_REFUSAL_POOL_RESERVE);
    assert(!s->serial && !s->completed && !s->pending && allocations == old_allocations);
    assert(video_radio_refusal_snapshot(report));
    assert(report[72] == 1 && report[87] == VIDEO_RADIO_REFUSAL_POOL_RESERVE && report[93] == 3);
    memcpy(frozen, report, sizeof(report));
    pool_room = 1; allocation_ok = 0;
    assert(!video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    assert(s->attempt_counter == 2 && s->last_refusal.reason == VIDEO_RADIO_REFUSAL_ALLOC);
    assert(!s->serial && !s->pending && allocations == old_allocations + 1);
    assert(s->last_refusal.first.free == 4 && frozen[87] == VIDEO_RADIO_REFUSAL_POOL_RESERVE);
    allocation_ok = 1;
    assert(video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    assert(s->last_attempt.attempt == 3 && !s->last_attempt.reason && s->last_refusal.attempt == 2);
    assert(!video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    assert(s->last_refusal.attempt == 4 && s->last_refusal.reason == VIDEO_RADIO_REFUSAL_BUSY);
    dispatch();
    assert(s->last_attempt.attempt == 3 && s->last_attempt.site == VIDEO_RADIO_CONSUMER_SITE);
    assert(s->last_refusal.attempt == 4 && s->attempt_counter == 4);
    event(VIDEO_RADIO_DM_READ, 1, 0x123, 0, 1, 1);
    assert(!video_radio_request(1, 2, VIDEO_RADIO_READ, 0, 0, 0));
    assert(s->last_refusal.reason == VIDEO_RADIO_REFUSAL_EPOCH);
    assert(!video_radio_request(1, 1, VIDEO_RADIO_SET, 2, 2, 0));
    assert(s->last_refusal.reason == VIDEO_RADIO_REFUSAL_PERMISSION);
    assert(!video_radio_request(0, 1, VIDEO_RADIO_READ, 0, 0, 0));
    assert(s->last_refusal.reason == VIDEO_RADIO_REFUSAL_ARGUMENT);
    s->serial = UINT32_MAX; s->attempt_counter = UINT32_MAX;
    assert(!video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    assert(s->attempt_counter == UINT32_MAX && s->last_refusal.reason == VIDEO_RADIO_REFUSAL_SERIAL);
    visible = 0;
    assert(video_radio_refusal_snapshot(report) && !report[12]);
    for (unsigned i = 16; i < sizeof(report); ++i) assert(!report[i]);
    assert(!video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    visible = 1;
    assert(!mail && !depth && reads == old_reads + 1 && sets == old_sets);
}
#ifdef VIDEO_RADIO_PLATFORM_TEST
static video_radio_trace platform_trace;
static int check_pool(uint32_t bytes) {
    platform_trace = video_radio_attempt(1, 1, 1, VIDEO_RADIO_READ, VIDEO_RADIO_REQUEST_SITE);
    return video_radio_pool_room(bytes, &platform_trace);
}
static void pool_free(uint8_t *pool, uint32_t base, uint32_t count) {
    *(uint32_t *)(pool + 8) = count ? base : 0;
    for (uint32_t n = 0; n < count; ++n)
        *(uint32_t *)(uintptr_t)(base + n * 16) = n + 1 < count ? base + (n + 1) * 16 : 0;
}
#endif
int main(void) {
    visible = 0; live(1, 0x123, 1);
    video_radio_bind(1); event(VIDEO_RADIO_DM_OPEN, 1, 0x123, 0, 0, 0);
    assert(dm_calls == 1 && !allocations && !reads && !sets);
    visible = 1;
    video_radio_bind(0); video_radio_bind(4);
    live(2, 0x456, 0); video_radio_bind(2); /* Central/ring is not an owned host. */
    assert(!context.video_radio.links[1].connected);
    video_radio_bind(1);
    video_radio_link *link = &context.video_radio.links[0];
    assert(link->connected && link->bound && link->epoch == 1 && link->handle == 0x123);
    assert(!video_radio_request(1, 0, VIDEO_RADIO_READ, 0, 0, 0));
    assert(!video_radio_request(1, 1, VIDEO_RADIO_SET, 2, 2, 7));
    assert(video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    assert(!video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    dispatch(); assert(reads == 1 && !sets && packet[3] == 0x23 && packet[4] == 1);
    event(VIDEO_RADIO_DM_READ, 1, 0x999, 0, 1, 2);
    assert(context.video_radio.pending == 2 && !link->valid);
    tick = 77; event(VIDEO_RADIO_DM_READ, 1, 0x123, 0, 1, 2);
    assert(link->valid && link->tx == 1 && link->rx == 2 && link->tick == 77 &&
           !context.video_radio.pending && context.video_radio.completed == 1);
    link->permission = 7;
    assert(!video_radio_request(1, 1, VIDEO_RADIO_SET, 0, 2, 7));
    assert(!video_radio_request(1, 1, VIDEO_RADIO_SET, 2, 2, 8));
    assert(video_radio_request(1, 1, VIDEO_RADIO_SET, 2, 2, 7));
    dispatch(); assert(sets == 1 && context.video_radio.pending == 2);
    event(VIDEO_RADIO_DM_UPDATE, 1, 0x123, 0, 2, 2);
    assert(link->valid && link->tx == 2 && link->rx == 2 && !context.video_radio.pending);
    assert(video_radio_request(1, 1, VIDEO_RADIO_SET, 2, 2, 7));
    link->permission = 0; dispatch(); assert(sets == 1 && !context.video_radio.pending);
    assert(video_radio_request(1, 1, VIDEO_RADIO_READ, 0, 0, 0));
    event(VIDEO_RADIO_DM_CLOSE, 1, 0x123, 0, 0, 0);
    live(1, 0x123, 1); event(VIDEO_RADIO_DM_OPEN, 1, 0x123, 0, 0, 0); video_radio_bind(1);
    assert(link->epoch == 2 && !link->valid && !link->permission);
    dispatch(); assert(reads == 1); /* Even handle reuse cannot inherit mail. */
    assert(video_radio_request(1, 2, VIDEO_RADIO_READ, 0, 0, 0));
    room = 0; dispatch(); room = 1; assert(reads == 1 && context.video_radio.result == 2);
    assert(context.video_radio.last_refusal.reason == VIDEO_RADIO_REFUSAL_COMMAND_CREDIT);
    pool_room = 0; assert(!video_radio_request(1, 2, VIDEO_RADIO_READ, 0, 0, 0)); pool_room = 1;
    allocation_ok = 0; assert(!video_radio_request(1, 2, VIDEO_RADIO_READ, 0, 0, 0)); allocation_ok = 1;
    assert(video_radio_request(1, 2, VIDEO_RADIO_READ, 0, 0, 0));
    allocation_ok = 0; dispatch(); allocation_ok = 1; assert(!context.video_radio.pending);
    assert(context.video_radio.last_refusal.reason == VIDEO_RADIO_REFUSAL_COMMAND_ALLOC);
    context.video_radio.serial = UINT32_MAX;
    assert(!video_radio_request(1, 2, VIDEO_RADIO_READ, 0, 0, 0));
    event(VIDEO_RADIO_DM_CLOSE, 1, 0x123, 0, 0, 0); link->epoch = UINT32_MAX;
    video_radio_bind(1); assert(!link->connected);
    video_radio_app_handler(1, 0);
    uint8_t unrelated[20] = {0, 0, 0xf3}; video_radio_app_handler(0, unrelated);
    assert(app_calls == 2 && !depth && !mail && sets == 1);
    memset(&context, 0, sizeof(context)); video_radio_bind(1);
    reads = sets = 0; tick = 123;
    event(VIDEO_RADIO_DM_READ, 1, 0x123, 0, 1, 2);
    uint8_t report[VIDEO_RADIO_REPORT_BYTES];
    assert(video_radio_snapshot(report));
    assert(report[3] == 1 && report[8] == 1 && report[9] == 32 && report[10] == 3 && report[11] == 1);
    assert(report[12] == 1 && report[13] == 7 && report[34] == 1 && report[35] == 2);
    assert(!(report[13] & 24) && !report[38] && !report[39]);
    assert(mail && !reads && !sets); dispatch(); assert(reads == 1 && !sets);
    event(VIDEO_RADIO_DM_CLOSE, 1, 0x123, 0, 0, 0);
    assert(video_radio_snapshot(report) && !report[13] && !sets && !mail);
    check_donor_role_binding();
    check_refusal_evidence();
#ifdef VIDEO_RADIO_PLATFORM_TEST
    void *mapped = mmap((void *)0x20000000U, 0x100000, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    assert(mapped == (void *)0x20000000U);
    uint8_t *pools = (void *)0x2007d000U;
    *(uint32_t *)0x20077238U = (uint32_t)(uintptr_t)pools;
    *(uint8_t *)0x2007740aU = 3;
    for (uint32_t i = 0; i < 3; ++i) {
        *(uint16_t *)(pools + i * 12) = 16U << i;
        *(uint32_t *)(pools + i * 12 + 8) = 0x2007e000U + i * 64;
        for (uint32_t n = 0; n < 4; ++n)
            *(uint32_t *)(uintptr_t)(0x2007e000U + i * 64 + n * 16) =
                n < 3 ? 0x2007e000U + i * 64 + (n + 1) * 16 : 0;
    }
    assert(check_pool(20)); /* 20+8 = 28, selects the 32-byte pool. */
    *(uint32_t *)(pools + 12 + 8) += 16;
    assert(!check_pool(20)); /* Three remaining: preserve them. */
    assert(check_pool(5)); /* 5+8 = 13, Read uses the 16-byte pool. */
    assert(!check_pool(10)); /* 10+8 = 18, Set uses the 32-byte pool. */
    *(uint32_t *)(pools + 12 + 8) -= 16;
    assert(check_pool(10));
    for (uint32_t small = 0; small <= 6; ++small) {
        for (uint32_t large = 0; large <= 6; ++large) {
            pool_free(pools + 12, 0x2007e100U, small);
            pool_free(pools + 24, 0x2007e200U, large);
            /* Model the authenticated donor's first eligible nonempty class. */
            uint32_t chosen = small ? 1 : large ? 2 : UINT8_MAX;
            uint32_t free_count = small ? small : large;
            assert(check_pool(20) == (free_count >= 4));
            assert(platform_trace.first.index == 1 && platform_trace.first.free == (small < 4 ? small : 4));
            assert(platform_trace.selected.index == chosen);
            if (chosen != UINT8_MAX)
                assert(platform_trace.selected.free == (free_count < 4 ? free_count : 4));
            assert(platform_trace.skipped == (!small ? !large ? 2 : 1 : 0));
            assert(platform_trace.reason == (free_count >= 4 ? VIDEO_RADIO_REFUSAL_NONE :
                chosen == UINT8_MAX ? VIDEO_RADIO_REFUSAL_POOL_EMPTY : VIDEO_RADIO_REFUSAL_POOL_RESERVE));
            assert(check_pool(10) == (free_count >= 4));
        }
    }
    pool_free(pools, 0x2007e000U, 0); pool_free(pools + 12, 0x2007e100U, 0);
    pool_free(pools + 24, 0x2007e200U, 4);
    assert(check_pool(5) && platform_trace.first.index == 0 && platform_trace.selected.index == 2);
    assert(platform_trace.skipped == 2 && platform_trace.selected.free == 4);
    *(uint32_t *)(pools + 8) = 0x2007e001U;
    assert(!check_pool(5) && platform_trace.reason == VIDEO_RADIO_REFUSAL_POOL_METADATA);
    *(uint32_t *)(pools + 8) = 0x2007e000U;
    *(uint32_t *)0x2007e000U = 0x2007e000U;
    assert(!check_pool(5) && platform_trace.first.free == UINT8_MAX);
    pool_free(pools, 0x2007e000U, 4);
    assert(!check_pool(100) && platform_trace.reason == VIDEO_RADIO_REFUSAL_POOL_ELIGIBLE);
    *(uint32_t *)0x20077238U = 0x2029fff0U;
    assert(!check_pool(20) && platform_trace.reason == VIDEO_RADIO_REFUSAL_POOL_METADATA);
    *(uint32_t *)0x20077238U = (uint32_t)(uintptr_t)pools;
    *(uint8_t *)0x2007740aU = 17; assert(!check_pool(20));
    *(uint8_t *)0x2007740aU = 3;
    *(uint8_t *)0x20075b62U = 1; *(uint32_t *)0x20075c6cU = 4;
    assert(video_radio_command_room());
    *(uint32_t *)0x20075b58U = 1; assert(!video_radio_command_room());
    *(uint32_t *)0x20075b58U = 0; *(uint8_t *)0x20075b62U = 0;
    assert(!video_radio_command_room());
    *(uint8_t *)0x20075b62U = 1; *(uint32_t *)0x20075c68U = 4;
    assert(!video_radio_command_room());
    *(uint8_t *)0x20072ff2U = 7; assert(video_radio_handler_id() == 7);
    assert(!munmap(mapped, 0x100000));
#endif
    return 0;
}
