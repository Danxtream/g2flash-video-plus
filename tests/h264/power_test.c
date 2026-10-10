/* Production paired-power policy and bounded adapter with fake stock services.
 * SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include "../../patches/video/storage.h"
#include "../../patches/cfw_context.h"

static customCfwContext contexts[2];
static uint32_t here, now = 100, locks, service, schedules, starts[2], awake[2];
static uint32_t drop, refuse, delayed, cancels, refreshes, bootstrap, shortage, cancel_delay;
static customCfwContext *peekCustomCfwContext(void) { return bootstrap ? 0 : &contexts[here]; }
static customCfwContext *getCustomCfwContext(void) { return &contexts[here]; }
static int video_control_generation_valid(customCfwContext *ctx, uint32_t generation) {
    return ctx->video_control.start_guard == generation && generation &&
        (int32_t)(ctx->video_control.active_deadline - now) > 0;
}
static int video_lease_valid(customCfwContext *ctx) { return (int32_t)(ctx->direct_lease_deadline - now) > 0; }
static int video_control_wait(customCfwContext *ctx, uint32_t timeout) {
    (void)ctx; (void)timeout; assert(!service && !locks); locks = 1; return 1;
}
static void video_control_give(customCfwContext *ctx) { (void)ctx; assert(locks); locks = 0; }
void video_control_cancel_locked(void) { assert(locks); ++cancels; contexts[here].video_control.start_guard = 0; }
int video_controller_request_locked(uint32_t reasons) {
    assert(reasons == VIDEO_CONTROLLER_POWER); ++schedules; return 1;
}
int video_worker_ensure_mutex(void) { assert(!service && !locks); bootstrap = 0; contexts[here].image_mutex = 1; return 1; }
static void delay(uint32_t milliseconds) {
    assert(!service && !locks); now += milliseconds;
    if (cancel_delay) contexts[here].video_control.start_guard = 0;
}
static int heap_view(void *argument, uint32_t heap, video_heap_view *view) {
    (void)argument; assert(!service && !locks && heap == VIDEO_HEAP_CACHED);
    *view = (video_heap_view){shortage ? 32768 : 300000, 200000};
    if (shortage) view->max_alloc = view->free_bytes;
    return 1;
}
static uint32_t refresh(uint32_t app, uint32_t event, const void *data, uint32_t size) {
    assert(app == 1 && event == 4 && !data && !size); ++refreshes; return 0x1234;
}
#define VIDEO_OS_THREAD_NEW 0
static uint32_t clock_tick(void) { return now; }
static uint32_t lens_here(void) { return here + 1; }
#define VIDEO_TICK clock_tick()
#define VIDEO_OS_DELAY delay
#define VIDEO_POWER_NATIVE 1
#define VIDEO_POWER_HERE lens_here
#define VIDEO_POWER_PLATFORM_REFRESH refresh
#define video_platform_heap_view heap_view
#define VIDEO_POOL_DISPATCH pool
#include "../../patches/video/platform.h"
static video_pool_item pending_bootstrap;
static int pool(const video_pool_item *item) { assert(item && item->callback); pending_bootstrap = *item; ++schedules; return 1; }

#ifdef POWER_ADAPTER
static uint32_t allocations, released, fail_alloc, fail_put, queue_puts;
static void *allocate(uint32_t size) {
    assert(!locks && !service); ++allocations; return allocations == fail_alloc ? 0 : malloc(size);
}
static void release(void *p) { assert(!locks && !service); ++released; free(p); }
static int put(uint32_t, const void *, uint8_t, uint32_t);
static uint32_t event_set(uint32_t event, uint32_t bits) { assert(event == 1 && bits == 2 && !service && !locks); return bits; }
#define cfw_malloc allocate
#define FW_FREE release
#define VIDEO_POWER_QUEUE_PUT put
#define VIDEO_OS_EVENT_SET event_set
#else
static int ready(void) { return awake[here]; }
static int start(void) {
    assert(!service && !locks); ++starts[here];
    if (!refuse) awake[here] = 1;
    return !refuse;
}
static int send(const uint8_t *);
#define VIDEO_POWER_PLATFORM_READY ready
#define VIDEO_POWER_PLATFORM_START start
#define VIDEO_POWER_PLATFORM_SEND send
#endif
#include "../../patches/video/power.c"

#ifdef POWER_ADAPTER
static int put(uint32_t queue, const void *item, uint8_t priority, uint32_t timeout) {
    assert(!service && !locks && !priority && !timeout);
    assert(queue == 0x20074d20 || queue == 0x20074cd0);
    video_power_packet *p = *(video_power_packet *const *)item; ++queue_puts;
    assert(!p->callback);
    if (p->bytes == 8) {
        const uint8_t startup[] = {2, 1, 1, 0, 0, 0, 0, 0};
        assert(!memcmp(p->payload, startup, 8));
        assert(p->kind == (here == 1 ? 2 : 0));
    } else {
        const uint8_t bridge[] = {1, 9, CFW_MESSAGE_SID, 0, 0, 0, VIDEO_POWER_BYTES, 0};
        assert(p->kind == 1 && p->bytes == sizeof(bridge) + VIDEO_POWER_BYTES);
        assert(queue == 0x20074cd0 && !memcmp(p->payload, bridge, sizeof(bridge)));
        for (uint32_t i = 0; i < VIDEO_POWER_BYTES; ++i) assert(p->payload[8 + i] == i);
    }
    if (fail_put) return -1;
    release(p->payload); release(p); /* Accepted consumer owns both. */
    return 0;
}
int main(void) {
    void *memory = mmap((void *)0x20074000, 0x4000, PROT_READ|PROT_WRITE,
        MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE, -1, 0); assert(memory == (void *)0x20074000);
    *(uint32_t *)0x20076d18 = 0x20074cd0; *(uint32_t *)0x20076d1c = 0x20074d20;
    *(uint32_t *)0x20076e6c = 1;
    for (uint32_t q = 0x20074cd0; q <= 0x20074d20; q += 0x50) {
        *(uint32_t *)(uintptr_t)(q + 60) = 150; *(uint32_t *)(uintptr_t)(q + 64) = 4;
    }
    for (here = 0; here < 2; ++here) {
        allocations = released = queue_puts = fail_alloc = fail_put = 0;
        assert(video_power_platform_start() && allocations == 2 && released == 2 && queue_puts == 1);
        uint8_t record[VIDEO_POWER_BYTES];
        for (uint32_t i = 0; i < sizeof(record); ++i) record[i] = i;
        assert(video_power_platform_send(record) && allocations == 4 && released == 4 && queue_puts == 2);
        allocations = released = 0; fail_alloc = 1;
        assert(!video_power_platform_start() && !released);
        allocations = released = 0; fail_alloc = 2;
        assert(!video_power_platform_start() && released == 1);
        allocations = released = 0; fail_alloc = 0; fail_put = 1;
        assert(!video_power_platform_start() && released == 2);
        allocations = 0; fail_put = 0; shortage = 1;
        assert(!video_power_platform_start() && !allocations); shortage = 0;
    }
    here = 0; *(uint32_t *)0x20076d18 = 0; allocations = 0;
    assert(!video_power_platform_start() && !allocations);
    assert(!munmap(memory, 0x4000));
    puts("bounded stock packet, both queue identities, allocation rollback and zero-timeout refusal PASS");
}
#else
static int send(const uint8_t *record) {
    assert(!service && !locks);
    if (drop) return 0;
    if (delayed && record[3] == VIDEO_POWER_ACK) { --delayed; return 1; }
    uint32_t saved = here; here ^= 1;
    uint32_t result = video_power_received(record, VIDEO_POWER_BYTES);
    if (!result) video_power_step();
    here = saved; return !result;
}
static void reset(void) {
    memset(contexts, 0, sizeof(contexts)); memset(awake, 0, sizeof(awake));
    memset(starts, 0, sizeof(starts)); now = 100; here = service = locks = drop = refuse = delayed = bootstrap = shortage = cancel_delay = 0;
    for (uint32_t i = 0; i < 2; ++i) {
        contexts[i].image_mutex = 1; contexts[i].direct_lease_deadline = now + 90000;
        contexts[i].video_control.start_guard = 1; contexts[i].video_control.stream = 1;
        contexts[i].video_control.active_deadline = now + 30000;
    }
}
int main(void) {
    reset(); assert(!starts[0] && !starts[1] && !schedules);
    for (uint32_t count = 0; count < 5; ++count) assert(video_dashboard_idle_count(count) == count - 1);
    for (uint32_t ingress = 0; ingress < 2; ++ingress) {
        reset(); here = ingress; delayed = 2;
        assert(video_power_acquire(1) && awake[0] && awake[1] && starts[0] == 1 && starts[1] == 1);
        assert(video_power_deadline(1) && video_dashboard_idle_count(1) == 1);
        /* The peer also starts an independent session. Either lease alone
         * keeps the dashboard awake; releasing one must not clear the other. */
        here ^= 1; assert(video_power_acquire(1));
        uint32_t peer_deadline = contexts[here].video_power.peer.deadline;
        video_power_release(1);
        assert(contexts[here].video_power.peer.deadline == peer_deadline);
        assert(video_dashboard_idle_count(1) == 1);
        here ^= 1; now += VIDEO_POWER_RENEW_MS; video_power_step();
        assert(video_power_deadline(1));
        service = 1; assert(video_ui_refresh(1, 4, 0, 0) == 0x1234); service = 0;
        video_power_release(1); here ^= 1;
        assert(video_dashboard_idle_count(1) == 0 && !contexts[here].video_power.peer.deadline);
    }
    reset(); refuse = 1; assert(!video_power_acquire(1) && !contexts[0].video_power.deadline);
    reset(); drop = 1; uint32_t begin = now;
    assert(!video_power_acquire(1) && now - begin <= VIDEO_POWER_START_MS + 10);
    reset(); drop = cancel_delay = 1;
    assert(!video_power_acquire(1) && !contexts[0].video_power.deadline && !contexts[0].video_power.acquired);
    reset(); drop = 1; contexts[0].video_control.active_deadline = now + 20;
    assert(!video_power_acquire(1) && !contexts[0].video_power.deadline);
    reset(); assert(video_power_acquire(1)); awake[1] = 0;
    now += VIDEO_POWER_RENEW_MS; video_power_step(); video_power_step();
    assert(cancels && !contexts[0].video_control.start_guard); /* No automatic re-wake. */
    here = 1; assert(starts[1] == 1); now += VIDEO_POWER_LEASE_MS;
    assert(video_dashboard_idle_count(1) == 0);
    reset(); uint8_t stale[VIDEO_POWER_BYTES];
    here = 1; video_power_encode(stale, VIDEO_POWER_ACQUIRE, 0, 1, 1, 1, 1000, 1);
    here = 0; service = 1; assert(!video_power_received(stale, sizeof(stale))); service = 0;
    video_power_step(); now += 1001;
    service = 1; assert(!video_power_received(stale, sizeof(stale))); service = 0;
    video_power_step(); assert(video_dashboard_idle_count(1) == 0); /* Replay never renews. */
    assert(video_power_received(stale, sizeof(stale)-1) == 0xa);
    stale[5] = 1; assert(video_power_received(stale, sizeof(stale)) == 0xa); stale[5] = 0;
    bootstrap = 1; service = 1; assert(video_power_received(stale, sizeof(stale)) == 6); service = 0;
    assert(pending_bootstrap.callback); pending_bootstrap.callback(31, 0, pending_bootstrap.size, 0);
    assert(!bootstrap);
    puts("paired asleep wake, delayed/lost mail, independent owners, expiry/replay, manual sleep and nonblocking callbacks PASS");
}
#endif
