/* Real controller/decoder with fake RTOS and reserve-aware cached heaps.
 * SPDX-License-Identifier: GPL-3.0-only */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../patches/video/storage.h"
#include "../../patches/cfw_context.h"
#define VIDEO_OS_THREAD_NEW new_thread
#define VIDEO_POOL_DISPATCH pool_dispatch
#include "../../patches/video/platform.h"

/* Native threads need sanitizer stacks. The firmware's static stack is checked
 * as a separate sentinel buffer; these tests do not infer ARM stack usage. */
static customCfwContext context;
static pthread_mutex_t image = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static _Thread_local uint32_t image_depth, worker_thread, service_context;
static uint32_t task_live, creates, terminates, allocation_calls, allocations_live;
static uint32_t fail_at, fail_task, bad_tcb, fail_terminate, fail_view;
static uint32_t cancel_new, expire_new, fail_publication, defer_reclaimed;
static uint32_t feed, fed, stream_ok, corrupt_guard, suppress_park;
static uint32_t mutex_calls, release_calls, runtime_at_release;
static uint32_t event_calls, fail_event, inject_cancel, inject_renew;
static uint32_t worker_waits, longest_wait, renewed;
static pthread_mutex_t preparation = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t preparation_changed = PTHREAD_COND_INITIALIZER;
static uint32_t pause_at, preparing_paused, resume_preparation, start_result, stop_result;
typedef struct {
    void *control;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint32_t bits, waiters;
} fake_event;
static fake_event events[4];
static video_heap_view views[3] = {{382756, 376832}, {285680, 278528}, {133924, 131072}};
static uint32_t cached_used, cached_peak;
static video_pool_item pool_jobs[8];
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t pool_count, fail_pool, pool_calls;
static uint8_t control_reply[30];
static uint32_t control_reply_size;
static const uint32_t shadow_present = 1;

static void test_zero(uint8_t *p, uint32_t n) { memset(p, 0, n); }
static customCfwContext *peekCustomCfwContext(void) { return &context; }
static customCfwContext *getCustomCfwContext(void) { return &context; }
static uint32_t tick(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}
static int take(uint32_t m, uint32_t timeout) {
    assert(!service_context && m == 1 && (!timeout || !image_depth) && timeout <= 2000);
    if (fail_publication && creates && !worker_thread) {
        --fail_publication; return -1;
    }
    if (defer_reclaimed && !task_live && creates && !allocations_live) {
        --defer_reclaimed; return -1;
    }
    if (timeout) {
        struct timespec limit; assert(!clock_gettime(CLOCK_REALTIME, &limit));
        limit.tv_sec += timeout / 1000;
        limit.tv_nsec += (timeout % 1000) * 1000000u;
        if (limit.tv_nsec >= 1000000000) { ++limit.tv_sec; limit.tv_nsec -= 1000000000; }
        if (pthread_mutex_timedlock(&image, &limit)) return -1;
    } else if (pthread_mutex_trylock(&image)) return -1;
    assert(!image_depth); ++image_depth; return 0;
}
static int give(uint32_t m) {
    assert(m == 1 && image_depth == 1); --image_depth;
    assert(!pthread_mutex_unlock(&image)); return 0;
}
static uint32_t mutex_new(void *p) { assert(!p && !image_depth); ++mutex_calls; return 1; }
static int mutex_delete(uint32_t m) { assert(m == 1 && !image_depth); return 0; }
static uint32_t thread_id(void) { return worker_thread ? 2 : 3; }
static int pool_dispatch(const video_pool_item *item) {
    assert(!pthread_mutex_lock(&pool_mutex));
    ++pool_calls;
    assert(item && !item->data && !item->event && item->size && item->callback &&
           item->app_id == VIDEO_MESSAGE_ID);
    if (fail_pool) { assert(!pthread_mutex_unlock(&pool_mutex)); return 0; }
    assert(pool_count < 8); pool_jobs[pool_count++] = *item;
    assert(!pthread_mutex_unlock(&pool_mutex)); return 1;
}
int cfw_message_video_reply(const cfw_message_route *route, const uint8_t *p, uint16_t n) {
    assert(!image_depth && route && n >= 9 && n <= route->reply_capacity &&
           n <= sizeof(control_reply) && p[0] == VIDEO_MESSAGE_ID && p[1] == route->here);
    memcpy(control_reply, p, n); control_reply_size = n; return 0;
}
static uint32_t new_thread(void (*fn)(void *), void *arg, const video_thread_attr *attr);
static int terminate(uint32_t id);
static int delay(uint32_t ms);
static uint32_t event_new(const video_event_attr *);
static uint32_t event_set(uint32_t, uint32_t);
static uint32_t event_wait(uint32_t, uint32_t, uint32_t, uint32_t);
static int video_platform_event_quiescent(uint32_t);
static void *allocate(uint32_t size) {
    assert(!service_context && !image_depth);
    ++allocation_calls;
    if (allocation_calls == fail_at) return 0;
    if (allocation_calls == pause_at) {
        assert(!pthread_mutex_lock(&preparation)); preparing_paused = 1;
        assert(!pthread_cond_broadcast(&preparation_changed));
        while (!resume_preparation)
            assert(!pthread_cond_wait(&preparation_changed, &preparation));
        assert(!pthread_mutex_unlock(&preparation));
    }
    uint32_t charge = ((size + 3u) & ~3u) + 4u;
    assert(size <= views[0].max_alloc && charge <= views[0].free_bytes - cached_used);
    uint32_t *raw = malloc(size + 8); assert(raw); *raw = charge;
    cached_used += charge; if (cached_used > cached_peak) cached_peak = cached_used;
    ++allocations_live; return raw + 2;
}
static void release(void *p) {
    assert(!service_context && !image_depth && p);
    /* Publication and binding must survive C++ destruction. Only the final
     * raw reclaim is allowed after the controller clears the provider. */
    if (worker_thread) {
        assert(context.video_runtime); ++runtime_at_release;
    }
    uint32_t *raw = (uint32_t *)p - 2;
    for (uint32_t i = 0; i < 4; ++i) {
        uintptr_t control = (uintptr_t)events[i].control;
        if (control >= (uintptr_t)p && control < (uintptr_t)p + *raw) {
            assert(!events[i].waiters);
            assert(!pthread_cond_destroy(&events[i].condition));
            assert(!pthread_mutex_destroy(&events[i].mutex));
            memset(&events[i], 0, sizeof(events[i]));
        }
    }
    assert(*raw <= cached_used && allocations_live);
    cached_used -= *raw; --allocations_live; ++release_calls; free(raw);
}
static int video_platform_heap_view(void *p, uint32_t heap, video_heap_view *out) {
    assert(!service_context && !p && !image_depth);
    uint32_t i = heap == 20 ? 0 : heap == 13 ? 1 : 2;
    if (fail_view) return 0;
    *out = views[i];
    if (!i) {
        out->free_bytes -= cached_used;
        if (out->max_alloc > out->free_bytes) out->max_alloc = out->free_bytes;
    }
    return 1;
}
static void *video_platform_allocate(void *p, uint32_t n) { assert(!p); return allocate(n); }
static void video_platform_release(void *p, void *v) { assert(!p); release(v); }
#define VIDEO_OS_THREAD_NEW new_thread
#define VIDEO_OS_THREAD_ID thread_id
#define VIDEO_OS_THREAD_TERMINATE terminate
#define VIDEO_OS_DELAY delay
#define VIDEO_OS_MUTEX_NEW mutex_new
#define VIDEO_OS_MUTEX_TAKE take
#define VIDEO_OS_MUTEX_GIVE give
#define VIDEO_OS_MUTEX_DELETE mutex_delete
#define VIDEO_OS_EVENT_NEW event_new
#define VIDEO_OS_EVENT_SET event_set
#define VIDEO_OS_EVENT_WAIT event_wait
#define VIDEO_TICK tick()
#define VIDEO_OWNER_ALLOC allocate
#define VIDEO_OWNER_FREE release
#define bzero test_zero
#include "../../patches/video/runtime_provider.c"
#include "../../patches/video/storage.c"
#include "../../patches/video/lifecycle.c"
#include "../../patches/video/queue.c"
#include "../../patches/video/worker.c"
#include "../../patches/video/controller.c"
#include "../../patches/video/control.c"

int worker_test_stream(void *);
void worker_finish_call(void) {
    video_owner *owner = video_current_owner(); assert(owner && worker_thread);
    video_storage_finish_call(&owner->storage);
}
struct Start { void (*fn)(void *); void *arg; };
static void *entry(void *p) {
    struct Start start = *(struct Start *)p; free(p); worker_thread = 1;
    start.fn(start.arg); assert(0); return 0;
}
static uint32_t new_thread(void (*fn)(void *), void *arg, const video_thread_attr *attr) {
    assert(!image_depth && attr->stack_size == 16384 && attr->cb_size == 112 &&
           attr->stack_mem && attr->cb_mem && attr->priority == 8 && !task_live);
    ++creates;
    if (fail_task) return 0;
    ((uint8_t *)attr->cb_mem)[109] = bad_tcb ? 0 : 2;
    /* Simulate the downward stack and kernel's initial saved register frame. */
    memset((uint8_t *)attr->stack_mem + attr->stack_size - 256, 0, 256);
    if (corrupt_guard) ((uint8_t *)attr->stack_mem)[-1] = 0;
    if (cancel_new) {
        assert(!take(1, 0)); video_worker_request_stop_locked(); give(1);
    }
    if (expire_new) __atomic_store_n(&context.direct_lease_deadline, tick() - 1, __ATOMIC_RELEASE);
    struct Start *start = malloc(sizeof(*start)); assert(start);
    *start = (struct Start){fn, arg};
    task_live = 1; assert(!pthread_create(&worker, 0, entry, start)); return 2;
}
static int delay(uint32_t ms) {
    assert(!image_depth);
    struct timespec t = {0, ms >= 1000 ? 1000000 : ms * 1000000u};
    nanosleep(&t, 0); return 0;
}
static uint32_t event_new(const video_event_attr *attr) {
    assert(!image_depth && attr && attr->cb_mem && attr->cb_size == 32);
    if (++event_calls == fail_event) return 0;
    for (uint32_t i = 0; i < 4; ++i) {
        if (events[i].control) continue;
        events[i].control = attr->cb_mem;
        assert(!pthread_mutex_init(&events[i].mutex, 0));
        assert(!pthread_cond_init(&events[i].condition, 0));
        return i + 1;
    }
    assert(0); return 0;
}
static uint32_t event_set(uint32_t id, uint32_t bits) {
    assert(!service_context && id && id <= 4 && events[id - 1].control && bits && !(bits & 0xff000000u));
    fake_event *event = &events[id - 1];
    assert(!pthread_mutex_lock(&event->mutex));
    event->bits |= bits;
    uint32_t result = event->bits;
    assert(!pthread_cond_broadcast(&event->condition));
    assert(!pthread_mutex_unlock(&event->mutex));
    return result;
}
static void cancelled_wait(void *argument) {
    fake_event *event = argument;
    assert(event->waiters); --event->waiters;
    assert(!pthread_mutex_unlock(&event->mutex));
}
static uint32_t event_wait(uint32_t id, uint32_t mask, uint32_t options, uint32_t timeout) {
    assert(!image_depth && id && id <= 4 && events[id - 1].control && !options);
    if (worker_thread) {
        ++worker_waits;
        if (timeout != UINT32_MAX && timeout > longest_wait) longest_wait = timeout;
    }
    /* Feed through the same wait seam that later input wakeups will use. */
    if (worker_thread && feed && !__atomic_load_n(&fed, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&context.video.ready, __ATOMIC_ACQUIRE) &&
        !video_lifecycle_cancelled(&context.video, context.video.token)) {
        video_owner *owner = video_current_owner(); assert(owner && owner->decoder);
        stream_ok = worker_test_stream(owner->decoder);
        if (!stream_ok) video_runtime_fail(VIDEO_FAULT_INIT);
        __atomic_store_n(&fed, 1, __ATOMIC_RELEASE);
    }
    if (worker_thread && inject_cancel && context.video.ready) {
        inject_cancel = 0;
        assert(!take(1, 0)); video_worker_request_stop_locked(); give(1);
    }
    if (worker_thread && inject_renew && context.video.ready) {
        inject_renew = 0; renewed = 1;
        assert(!take(1, 0));
        __atomic_store_n(&context.direct_lease_deadline, tick() + 5000, __ATOMIC_RELEASE);
        assert(video_worker_wake_locked(context.video.token, VIDEO_WAKE_LEASE)); give(1);
    }
    if (worker_thread && suppress_park &&
        __atomic_load_n(&context.video.parked, __ATOMIC_ACQUIRE))
        __atomic_store_n(&context.video.parked, 0, __ATOMIC_RELEASE);
    fake_event *event = &events[id - 1];
    assert(!pthread_mutex_lock(&event->mutex)); ++event->waiters;
    uint32_t result = 0;
    pthread_cleanup_push(cancelled_wait, event);
    struct timespec limit; assert(!clock_gettime(CLOCK_REALTIME, &limit));
    if (timeout != UINT32_MAX) {
        limit.tv_sec += timeout / 1000;
        limit.tv_nsec += (timeout % 1000) * 1000000u;
        if (limit.tv_nsec >= 1000000000) { ++limit.tv_sec; limit.tv_nsec -= 1000000000; }
    }
    while (!(event->bits & mask)) {
        if (!timeout) break;
        int status = timeout == UINT32_MAX ?
            pthread_cond_wait(&event->condition, &event->mutex) :
            pthread_cond_timedwait(&event->condition, &event->mutex, &limit);
        if (status) break;
    }
    result = event->bits & mask;
    event->bits &= ~result;
    pthread_cleanup_pop(0);
    --event->waiters;
    assert(!pthread_mutex_unlock(&event->mutex));
    return result ? result : 0xfffffffeu;
}
static int video_platform_event_quiescent(uint32_t id) {
    if (!id) return 1;
    assert(id <= 4 && events[id - 1].control);
    return !events[id - 1].waiters;
}
static int terminate(uint32_t id) {
    assert(!image_depth && !worker_thread && id == 2 && task_live &&
           __atomic_load_n(&context.video.parked, __ATOMIC_ACQUIRE));
    ++terminates; if (fail_terminate) return -1;
    assert(!pthread_cancel(worker)); assert(!pthread_join(worker, 0)); task_live = 0; return 0;
}
static void configure(void) {
    assert(!allocations_live && !task_live);
    memset(&context, 0, sizeof(context)); context.image_mutex = 1;
    context.direct_lease_deadline = tick() + 60000;
    if (shadow_present) context.framebuffer_shadow = (void *)(uintptr_t)1;
    cached_used = cached_peak = allocation_calls = creates = terminates = 0;
    fail_at = fail_task = bad_tcb = fail_terminate = fail_view = cancel_new = expire_new = 0;
    fail_publication = defer_reclaimed = feed = fed = stream_ok = corrupt_guard = suppress_park = 0;
    mutex_calls = release_calls = runtime_at_release = 0;
    event_calls = fail_event = inject_cancel = inject_renew = worker_waits = longest_wait = renewed = 0;
    pause_at = preparing_paused = resume_preparation = start_result = stop_result = 0;
    assert(!pool_count);
    fail_pool = pool_calls = control_reply_size = 0;
    for (uint32_t i = 0; i < 4; ++i) assert(!events[i].control);
    views[0] = (video_heap_view){382756, 376832};
    views[1] = (video_heap_view){285680, 278528};
    views[2] = (video_heap_view){133924, 131072};
}
static uint32_t state(void) {
    video_worker_report r;
    while (!video_worker_get_report(&r)) delay(1);
    return r.state;
}
static void await_ready(void) {
    uint32_t deadline = tick() + 3000;
    while (state() == VIDEO_STARTING) { assert((int32_t)(tick() - deadline) < 0); delay(1); }
}
static void finish(void) {
    for (uint32_t i = 0; i < 20 && !video_worker_stop(2000); ++i) delay(1);
    assert(state() == VIDEO_IDLE && !allocations_live && !cached_used && !task_live &&
           !context.video_owner && !context.video_runtime);
}
static void clean_quarantine(void) {
    /* Test process cleanup only: production deliberately retains quarantine. */
    fail_terminate = suppress_park = 0;
    if (task_live) {
        assert(!pthread_cancel(worker)); assert(!pthread_join(worker, 0)); task_live = 0;
    }
    video_owner *owner = context.video_owner;
    context.video_runtime = 0; context.video_owner = 0;
    if (owner) video_free_owner(owner);
    assert(!allocations_live);
}
static void normal(void) {
    configure();
    assert(!g2_h264_runtime_current() && !video_worker_get_report(0));
    assert(video_worker_stop(1) && !allocations_live && !creates && !mutex_calls);
    for (uint32_t i = 0; i < 20; ++i) {
        feed = 1; fed = stream_ok = 0;
        assert(video_worker_start(65536)); await_ready();
        uint32_t deadline = tick() + 3000;
        while (!__atomic_load_n(&fed, __ATOMIC_ACQUIRE)) {
            assert((int32_t)(tick() - deadline) < 0); delay(1);
        }
        assert(state() == VIDEO_READY && stream_ok);
        assert(!video_worker_start(0));
        finish();
        video_worker_report r; assert(video_worker_get_report(&r));
        assert(r.valid && r.storage_left == 0 && r.storage_peak <= VIDEO_STORAGE_LIMIT &&
               r.stack_used == 256 && r.guards_ok && r.object_bytes == g2_h264_size());
    }
    assert(runtime_at_release);
    printf("640 synthetic 320x192 Y planes match; cached peak %u; sentinel high-water 256/16384\n",
           cached_peak);
    assert(video_worker_reset(0, 1000)); await_ready(); finish();
}
static void allocation_failures(void) {
    configure(); feed = 1;
    assert(video_worker_start(0)); await_ready();
    while (!__atomic_load_n(&fed, __ATOMIC_ACQUIRE)) delay(1);
    uint32_t total = allocation_calls; finish();
    for (uint32_t fail = 1; fail <= total; ++fail) {
        configure(); fail_at = fail; feed = 1;
        int started = video_worker_start(0);
        if (started) {
            uint32_t deadline = tick() + 3000;
            while (!__atomic_load_n(&fed, __ATOMIC_ACQUIRE) &&
                   !__atomic_load_n(&context.video.parked, __ATOMIC_ACQUIRE)) {
                assert((int32_t)(tick() - deadline) < 0); delay(1);
            }
        }
        finish(); assert(allocation_calls >= fail);
    }
    printf("all %u actual allocation failures safely reclaimed\n", total);
}
static void startup(void) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
        configure();
        if (!mode) fail_task = 1;
        else if (mode == 1) cancel_new = 1;
        else if (mode == 2) expire_new = 1;
        else fail_publication = 2;
        assert(!video_worker_start(0)); finish();
        assert(!context.video_runtime);
    }
    configure(); assert(video_worker_start(0)); await_ready();
    defer_reclaimed = 1; assert(!video_worker_stop(2000));
    assert(context.video_reclaimed_token && !allocations_live && !task_live);
    assert(state() == VIDEO_IDLE);
}
static void reserves(void) {
    configure(); context.texture_cache = (void *)(uintptr_t)1;
    assert(!video_worker_start(0) && !allocation_calls); context.texture_cache = 0;
    context.direct_lease_deadline = 0;
    assert(!video_worker_start(0) && !allocation_calls);
    for (uint32_t i = 0; i < 3; ++i) {
        configure(); views[i].free_bytes = views[i].max_alloc = 16000;
        assert(!video_worker_start(0) && !allocation_calls); finish();
    }
    configure(); fail_view = 1; assert(!video_worker_start(0) && !allocation_calls); finish();
    configure(); views[0].max_alloc = 1;
    assert(!video_worker_start(0) && !allocation_calls); finish();
    configure(); context.framebuffer_shadow = 0;
    assert(!video_worker_start(65536 + 40000) && !allocation_calls); finish();
    configure(); assert(!video_worker_start(UINT32_MAX) && !allocation_calls); finish();
}
static void pins(void) {
    configure(); assert(video_worker_start(0)); await_ready();
    assert(!take(1, 0));
    uint32_t token = context.video.token;
    assert(video_lifecycle_pin(&context.video, token, 0));
    assert(video_lifecycle_pin(&context.video, token, 1));
    video_worker_request_stop_locked();
    assert(!video_worker_stop(1) && task_live); give(1);
    delay(10); assert(!__atomic_load_n(&context.video.parked, __ATOMIC_ACQUIRE));
    assert(!take(1, 0));
    assert(video_worker_unpin_locked(token, 0));
    assert(video_worker_unpin_locked(token, 1)); give(1);
    finish(); assert(!video_lifecycle_pin(&context.video, token, 0));
}
static void wakeups(void) {
    for (uint32_t i = 1; i <= 2; ++i) {
        configure(); fail_event = i;
        assert(!video_worker_start(0)); finish();
        assert(event_calls == i && !creates);
    }
    configure(); inject_cancel = 1;
    assert(video_worker_start(0)); await_ready(); finish();
    assert(!inject_cancel); /* cancel exactly between predicate and wait */
    configure(); context.direct_lease_deadline = tick() + 100;
    inject_renew = 1;
    assert(video_worker_start(0)); await_ready();
    uint32_t limit = tick() + 3000;
    while (!renewed || longest_wait < 4000) { assert((int32_t)(limit - tick()) > 0); delay(1); }
    delay(120); assert(!context.video.parked && state() == VIDEO_READY);
    uint32_t waits = worker_waits; delay(30); assert(worker_waits == waits);
    assert(!take(1, 0));
    assert(!video_worker_wake_locked(context.video.token + 1, VIDEO_WAKE_INPUT));
    assert(!video_worker_wake_locked(context.video.token, 0x80000000u));
    assert(!video_worker_unpin_locked(context.video.token + 1, 0)); give(1);
    finish();
    configure(); assert(video_worker_start(0)); await_ready();
    uint32_t old_token = context.video.token;
    finish();
    assert(video_worker_start(0)); await_ready();
    assert(video_stop_token(old_token, 100) && state() == VIDEO_READY);
    finish();
    configure(); context.direct_lease_deadline = tick() + 60;
    assert(video_worker_start(0)); await_ready();
    limit = tick() + 3000;
    while (!context.video.parked) { assert((int32_t)(limit - tick()) > 0); delay(1); }
    finish();
    puts("sticky cancel/renewal, idle blocking, stale wake, event rollback and lease deadline PASS");
}
static void *prepare_session(void *argument) {
    (void)argument; start_result = video_worker_start(0); return 0;
}
static void *stop_preparing_session(void *argument) {
    (void)argument; stop_result = video_worker_stop(2000); return 0;
}
static void preparation_cancel(void) {
    configure(); pause_at = 2;
    pthread_t starter, stopper;
    assert(!pthread_create(&starter, 0, prepare_session, 0));
    assert(!pthread_mutex_lock(&preparation));
    while (!preparing_paused)
        assert(!pthread_cond_wait(&preparation_changed, &preparation));
    assert(!pthread_mutex_unlock(&preparation));
    assert(!take(1, 0));
    assert(context.video.preparing && context.video_owner && !creates);
    video_worker_request_stop_locked(); give(1);
    assert(!pthread_create(&stopper, 0, stop_preparing_session, 0));
    delay(20);
    assert(allocations_live == 1 && context.video_owner && context.video.preparing);
    assert(!pthread_mutex_lock(&preparation)); resume_preparation = 1;
    assert(!pthread_cond_broadcast(&preparation_changed));
    assert(!pthread_mutex_unlock(&preparation));
    assert(!pthread_join(starter, 0)); assert(!pthread_join(stopper, 0));
    assert(!start_result && stop_result && !creates);
    finish();
    puts("completion remains reachable while private allocation is paused PASS");
}
static void quarantine(void) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
        configure();
        bad_tcb = mode == 0; fail_terminate = mode == 1;
        corrupt_guard = mode == 2;
        int started = video_worker_start(0);
        if (bad_tcb) assert(!started);
        else {
            assert(started); await_ready();
            if (mode == 3) {
                assert(!take(1, 0));
                assert(video_lifecycle_pin(&context.video, context.video.token, 0)); give(1);
            }
            assert(!video_worker_stop(10));
        }
        assert(state() == VIDEO_QUARANTINED && allocations_live && context.video_owner);
        if (!bad_tcb) assert(context.video_runtime);
        else assert(!context.video_runtime && !terminates);
        assert(!video_worker_start(0));
        video_worker_report r; assert(video_worker_get_report(&r) && !r.valid);
        if (corrupt_guard) assert(r.fault == VIDEO_FAULT_STACK);
        clean_quarantine();
    }
}

static void pool_drain(void) {
    assert(!image_depth);
    for (;;) {
        assert(!pthread_mutex_lock(&pool_mutex));
        if (!pool_count) { assert(!pthread_mutex_unlock(&pool_mutex)); break; }
        video_pool_item item = pool_jobs[0];
        memmove(pool_jobs, pool_jobs + 1, --pool_count * sizeof(*pool_jobs));
        assert(!pthread_mutex_unlock(&pool_mutex));
        item.callback(item.app_id, item.data, item.size, item.event);
    }
}
static uint32_t pool_pending(void) {
    assert(!pthread_mutex_lock(&pool_mutex));
    uint32_t count = pool_count;
    assert(!pthread_mutex_unlock(&pool_mutex)); return count;
}
static const cfw_message_route control_route = {1, 1, 3, 9, 0};
static uint32_t request_next;
static void control_command(uint8_t *p, uint32_t op, uint32_t stream) {
    memset(p, 0, VIDEO_START_BYTES);
    p[0] = VIDEO_MESSAGE_ID; p[1] = op; p[2] = VIDEO_PROTOCOL_VERSION;
    video_write32(p + 4, ++request_next);
    video_write32(p + 8, stream);
    p[12] = VIDEO_FRAME_WIDTH & 255; p[13] = VIDEO_FRAME_WIDTH >> 8;
    p[14] = VIDEO_FRAME_HEIGHT; p[16] = 1; p[17] = 2;
    video_write32(p + 20, 63);
}
static uint8_t control_snapshot(const uint8_t *p, uint32_t n, uint8_t *snapshot) {
    int result = video_control_received(p, n, &control_route);
    assert(control_reply_size == 9 && control_reply[7] == VIDEO_STATUS_BYTES);
    uint8_t page[9] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_PAGE, VIDEO_PROTOCOL_VERSION};
    memcpy(page + 4, p + 4, 4);
    for (uint32_t i = 0; i < VIDEO_STATUS_BYTES; ++i) {
        page[8] = i;
        assert(!video_control_received(page, sizeof(page), &control_route));
        assert(control_reply[6] == i && control_reply[7] == VIDEO_STATUS_BYTES);
        snapshot[i] = control_reply[8];
    }
    assert((result == 0) == (snapshot[2] == VIDEO_CONTROL_ACCEPTED));
    return snapshot[2];
}
static void controls(void) {
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    configure(); request_next = 0;
    control_command(p, VIDEO_CONTROL_CAPABILITIES, 0);
    assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
    assert(snapshot[1] == 4 && snapshot[3] == VIDEO_IDLE && !snapshot[50] &&
           !allocations_live && !creates && !pool_calls);
    control_command(p, VIDEO_CONTROL_START, 1);
    context.texture_cache = (void *)(uintptr_t)1;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_LEASE && !pool_calls);
    context.texture_cache = 0;
    control_command(p, VIDEO_CONTROL_START, 1);
    p[18] = 1;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_FORMAT && !pool_calls);
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED && pool_count == 1);
    assert(snapshot[3] == VIDEO_STARTING && !allocations_live);
    assert(video_transport_room(65536));
    views[1] = (video_heap_view){VIDEO_DISPLAY_RESERVE + 100, 100};
    assert(video_transport_room(96) && !video_transport_room(97) &&
           !video_transport_room(UINT32_MAX));
    context.framebuffer_shadow = 0;
    assert(!video_transport_room(1));
    context.framebuffer_shadow = (void *)(uintptr_t)1;
    views[1] = (video_heap_view){285680, 278528};
    uint32_t calls = pool_calls;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED && pool_calls == calls);
    p[20] ^= 1; assert(video_control_received(p, 24, &control_route)); p[20] ^= 1;
    uint8_t custom[] = {18};
    assert(video_control_blocks_custom(custom, 1));
    /* STOP before dispatch invalidates even a not-yet-claimed owner. */
    control_command(p, VIDEO_CONTROL_STOP, 1);
    assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); assert(!allocations_live && !creates && !context.video_control.start_guard);
    control_command(p, VIDEO_CONTROL_START, 2);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready(); assert(state() == VIDEO_READY);
    control_command(p, VIDEO_CONTROL_STATUS, 0);
    assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED && snapshot[3] == VIDEO_READY);
    assert(video_read32(snapshot + 4) == 2 && !video_read32(snapshot + 52));
    cfw_message_route bad = control_route; bad.targets = 2;
    assert(video_control_received(p, 8, &bad));
    bad = control_route; bad.origin = 2; bad.reply_capacity = 30;
    control_command(p, VIDEO_CONTROL_STOP, 2);
    assert(video_control_received(p, 12, &bad));
    assert(context.video_control.replay[1][0].result == VIDEO_CONTROL_STALE);
    control_command(p, VIDEO_CONTROL_RESET, 2);
    assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); finish(); assert(!video_control_blocks_custom(custom, 1));
    control_command(p, VIDEO_CONTROL_START, 2);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_FORMAT);
    control_command(p, VIDEO_CONTROL_START, 3); fail_pool = 1;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_DISPATCH && !allocations_live);
    fail_pool = 0;
    control_command(p, VIDEO_CONTROL_START, 4);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    assert(!take(1, 0));
    context.direct_lease_deadline = tick() + 50;
    video_worker_wake_locked(context.video.token, VIDEO_WAKE_LEASE); give(1);
    uint32_t limit = tick() + 3000;
    while (!context.video.parked) { assert((int32_t)(limit - tick()) > 0); delay(1); }
    while (!pool_pending()) { assert((int32_t)(limit - tick()) > 0); delay(1); }
    pool_drain(); finish();
    context.direct_lease_deadline = tick() + 60000;
    context.video_control.controller_serial = UINT32_MAX;
    control_command(p, VIDEO_CONTROL_START, 5);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_DISPATCH && !pool_count);
    control_command(p, VIDEO_CONTROL_START, 6);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_DISPATCH && !pool_count);
    puts("MTU23 paging, idempotent controls, guarded startup, pool refusal and expiry reaping PASS");
}
static void *pool_run(void *argument) { (void)argument; pool_drain(); return 0; }
static void control_preparation(void) {
    for (uint32_t allocation = 1; allocation <= 2; ++allocation) {
        configure(); request_next = 0; pause_at = allocation;
        uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pthread_t controller;
        assert(!pthread_create(&controller, 0, pool_run, 0));
        assert(!pthread_mutex_lock(&preparation));
        while (!preparing_paused)
            assert(!pthread_cond_wait(&preparation_changed, &preparation));
        assert(!pthread_mutex_unlock(&preparation));
        assert(context.video_control.controller_job && !creates &&
               allocations_live == allocation - 1);
        control_command(p, VIDEO_CONTROL_STOP, 1);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        assert(!context.video_control.start_guard && !creates);
        assert(!pthread_mutex_lock(&preparation)); resume_preparation = 1;
        assert(!pthread_cond_broadcast(&preparation_changed));
        assert(!pthread_mutex_unlock(&preparation));
        assert(!pthread_join(controller, 0));
        assert(!allocations_live && !creates && !pool_pending() && state() == VIDEO_IDLE);
        control_command(p, VIDEO_CONTROL_START, 2);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready();
        control_command(p, VIDEO_CONTROL_STOP, 2);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish();
    }
    puts("control cancellation before claim and during private allocation PASS");
}
static void control_failures(void) {
    for (uint32_t unknown_abi = 0; unknown_abi < 2; ++unknown_abi) {
        configure(); request_next = 0;
        if (unknown_abi) bad_tcb = 1;
        else fail_at = 8; /* Four receive slots precede the constructor. */
        uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain();
        uint32_t limit = tick() + 3000;
        while (!__atomic_load_n(&context.video.parked, __ATOMIC_ACQUIRE)) {
            assert((int32_t)(limit - tick()) > 0); delay(1);
        }
        while (context.video_control.controller_job || pool_pending()) {
            pool_drain(); assert((int32_t)(limit - tick()) > 0);
        }
        control_command(p, VIDEO_CONTROL_STATUS, 0);
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
        if (unknown_abi) {
            assert(snapshot[3] == VIDEO_QUARANTINED &&
                   video_read32(snapshot + 24) == VIDEO_CONTROL_QUARANTINE && !terminates);
            clean_quarantine();
        } else {
            assert(snapshot[3] == VIDEO_IDLE &&
                   video_read32(snapshot + 24) == VIDEO_CONTROL_MEMORY && !allocations_live);
            finish();
        }
    }
    puts("asynchronous constructor refusal and unknown-ABI quarantine are reported PASS");
}
static void nal_input(void) {
    configure(); request_next = 0;
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    uint8_t record[VIDEO_SLOT_BYTES + 1];
    memset(record, 0x55, sizeof(record));
    record[0] = VIDEO_MESSAGE_ID; record[1] = VIDEO_CONTROL_NAL;
    video_write32(record + 2, 1); video_write32(record + 6, 0);
    record[VIDEO_NAL_HEADER_BYTES] = 0x67;
    assert(video_control_received(record, VIDEO_SLOT_BYTES, &control_route));
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    video_owner *owner = context.video_owner;
    assert(owner->queue.capacity == 4 && !owner->queue.count &&
           !owner->storage.cached_allowance && owner->storage.limit == 223288);
    uint32_t allocations = allocation_calls;
    uint32_t request_high = context.video_control.request_high[0];
    for (uint32_t n = 2; n <= VIDEO_NAL_HEADER_BYTES; ++n)
        assert(video_control_received(record, n, &control_route));
    assert(context.video_control.request_high[0] == request_high);
    for (uint32_t i = 0; i < 4; ++i) {
        video_write32(record + 6, i); record[11] = i;
        assert(!video_control_received(record, VIDEO_SLOT_BYTES, &control_route));
    }
    memset(record + VIDEO_NAL_HEADER_BYTES, 0xee, VIDEO_RAW_NAL_BYTES);
    assert(owner->queue.count == 4 && owner->queue.accepted == 4 &&
           !owner->queue.consumed && allocation_calls == allocations);
    for (uint32_t i = 0; i < 4; ++i)
        assert(owner->queue.slots[i].bytes[0] == 0x67 &&
               owner->queue.slots[i].bytes[1] == i);
    video_write32(record + 6, 4); record[10] = 0x41;
    video_nal_queue before = owner->queue;
    assert(video_control_received(record, VIDEO_SLOT_BYTES, &control_route));
    assert(!memcmp(&before, &owner->queue, sizeof(before)));
    assert(video_control_received(record, VIDEO_SLOT_BYTES + 1, &control_route));
    assert(video_control_received(record, VIDEO_NAL_HEADER_BYTES, &control_route));
    cfw_message_route wrong = control_route; wrong.origin = 2;
    assert(video_control_received(record, VIDEO_SLOT_BYTES, &wrong));
    control_command(p, VIDEO_CONTROL_STATUS, 0);
    assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED &&
           snapshot[50] == 4 && !snapshot[51] && video_read32(snapshot + 56) == 4 &&
           !video_read32(snapshot + 60));
    control_command(p, VIDEO_CONTROL_STOP, 1);
    assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
    assert(video_control_received(record, VIDEO_SLOT_BYTES, &control_route));
    pool_drain(); finish();
    for (uint32_t allocation = 4; allocation <= 7; ++allocation) {
        configure(); request_next = 0; fail_at = allocation;
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish(); assert(!creates && allocation_calls == allocation);
    }
    puts("raw NAL snapshot, full rollback, owned four-slot admission and partial-start reclaim PASS");
}
static void queue_extension(void) {
    for (uint32_t mode = 0; mode < 3; ++mode) {
        configure(); request_next = 0;
        uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready();
        video_owner *owner = context.video_owner;
        assert(!video_worker_picture_complete(owner->token, 1)); /* Wrong task. */
        assert(owner->queue.capacity == 4 && !owner->extension_attempted);
        uint32_t used = owner->storage.used;
        worker_thread = 1; /* Fake consumer at its decoder-call boundary. */
        assert(video_worker_picture_complete(owner->token, 0));
        assert(owner->queue.pictures == 1 && owner->queue.capacity == 4 &&
               !owner->extension_attempted);
        if (mode == 1) fail_at = allocation_calls + 1;
        if (mode == 2) views[0].max_alloc = 8000;
        assert(video_worker_picture_complete(owner->token, 1));
        worker_thread = 0;
        if (!mode) {
            assert(owner->queue.capacity == 6 && owner->storage.limit == 231480 &&
                   owner->storage.used == used + 8228);
        } else assert(owner->queue.capacity == 4 && owner->storage.limit == 223288 &&
                      owner->storage.used == used);
        assert(owner->queue.pictures == 2 && owner->extension_attempted);
        uint32_t allocations = allocation_calls;
        worker_thread = 1;
        assert(video_worker_picture_complete(owner->token, 1));
        worker_thread = 0;
        assert(allocation_calls == allocations); /* At most one allocation attempt. */
        control_command(p, VIDEO_CONTROL_STATUS, 0);
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED &&
               snapshot[50] == (!mode ? 6 : 4) && video_read32(snapshot + 64) == 3 &&
               !video_read32(snapshot + 56) && !video_read32(snapshot + 60));
        control_command(p, VIDEO_CONTROL_STOP, 1);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish();
    }
    puts("explicit picture/DPB signal, single extension attempt and fragmented/OOM four-slot fallback PASS");
}
static int receive_header(uint32_t stream, uint32_t sequence, uint8_t header) {
    uint8_t record[VIDEO_NAL_HEADER_BYTES + 2] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_NAL};
    video_write32(record + 2, stream); video_write32(record + 6, sequence);
    record[10] = header; record[11] = 0x55;
    return video_control_received(record, sizeof(record), &control_route);
}
static void await_reclaimed(void) {
    uint32_t limit = tick() + 3000;
    while (state() != VIDEO_IDLE || pool_pending() || context.video_control.controller_job) {
        pool_drain(); assert((int32_t)(limit - tick()) > 0); delay(1);
    }
    assert(!allocations_live && !task_live);
}
static void recovery(void) {
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    for (uint32_t mode = 0; mode < 5; ++mode) {
        configure(); request_next = 0;
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready();
        video_owner *owner = context.video_owner;
        assert(!receive_header(1, 2, 0x65));
        uint32_t deadline = owner->queue.gap_deadline;
        assert(deadline && owner->queue.gap_sequence == 0);
        assert(!receive_header(1, 2, 0x65) && owner->queue.accepted == 1);
        assert(!receive_header(1, 3, 0x41) && owner->queue.gap_deadline == deadline);
        control_command(p, VIDEO_CONTROL_STATUS, 0);
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED && snapshot[68] == 1 &&
               video_read32(snapshot + 72) == deadline && !video_read32(snapshot + 76));
        uint32_t fault;
        if (!mode) {
            assert(!receive_header(1, 0, 0x67));
            assert(!receive_header(1, 1, 0x68));
            assert(owner->queue.header_progress == 7 && !owner->queue.gap_deadline);
            assert(!owner->queue.pictures && !owner->queue.consumed);
            control_command(p, VIDEO_CONTROL_STOP, 1);
            assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
            pool_drain(); finish(); continue;
        } else if (mode == 1) {
            assert(receive_header(1, 2, 0x64)); fault = VIDEO_CONTROL_CONFLICT;
        } else if (mode == 2) {
            assert(!take(1, 0));
            __atomic_store_n(&owner->queue.gap_deadline, tick() - 1, __ATOMIC_RELEASE);
            assert(!video_worker_receive_nal_locked(1, 0, (uint8_t[]){0x67}, 1, 1));
            give(1); fault = VIDEO_CONTROL_GAP;
        } else if (mode == 3) {
            assert(receive_header(1, 0, 0x41)); fault = VIDEO_CONTROL_INPUT;
        } else {
            assert(receive_header(1, UINT32_MAX, 0x67)); fault = VIDEO_CONTROL_SEQUENCE;
        }
        await_reclaimed();
        control_command(p, VIDEO_CONTROL_STATUS, 0);
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED && snapshot[68] == 2 &&
               video_read32(snapshot + 24) == fault && !snapshot[50]);
        assert(receive_header(1, 0, 0x67));
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_FORMAT);
        control_command(p, VIDEO_CONTROL_START, 2);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready();
        assert(!receive_header(2, 0, 0x67) && !receive_header(2, 1, 0x68) &&
               !receive_header(2, 2, 0x65) && !receive_header(2, 3, 0x41));
        assert(!receive_header(2, 0, 0x67)); /* Exact retry after new stream. */
        owner = context.video_owner;
        assert(owner->queue.header_progress == 7 && !owner->queue.consumed && !owner->queue.pictures);
        control_command(p, VIDEO_CONTROL_STOP, 2);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish();
    }
    puts("gap repair, conflicts, late input, header recovery, wrap and fresh stream PASS");
}
static void inactivity(void) {
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    for (uint32_t kind = 0; kind < 3; ++kind) {
        configure(); request_next = 0;
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        if (!kind) {
            __atomic_store_n(&context.video_control.active_deadline, tick() - 1, __ATOMIC_RELEASE);
            pool_drain(); assert(!creates && !allocations_live);
        } else {
            pool_drain(); await_ready();
            control_command(p, VIDEO_CONTROL_STATUS, 0);
            assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
            uint32_t deadline = tick() + 40;
            assert(!take(1, 0));
            __atomic_store_n(&context.video_control.active_deadline, deadline, __ATOMIC_RELEASE);
            video_worker_wake_locked(context.video.token, VIDEO_WAKE_INPUT); give(1);
            if (kind == 1) {
                assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
                assert(context.video_control.active_deadline == deadline); /* Cached status replay. */
            } else {
                control_command(p, VIDEO_CONTROL_STATUS, 0);
                assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED &&
                       (int32_t)(context.video_control.active_deadline - deadline) > 20000);
                assert(!take(1, 0));
                video_owner *owner = context.video_owner;
                __atomic_store_n(&owner->queue.gap_deadline, tick() + 20, __ATOMIC_RELEASE);
                video_worker_wake_locked(context.video.token, VIDEO_WAKE_INPUT); give(1);
            }
            await_reclaimed();
        }
        control_command(p, VIDEO_CONTROL_STATUS, 0);
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED && snapshot[68] == 2 &&
               video_read32(snapshot + 24) == (kind == 2 ? VIDEO_CONTROL_GAP : VIDEO_CONTROL_INACTIVITY));
        finish();
    }
    puts("delayed START, nonrenewing replay, fresh activity and event-only deadlines PASS");
}

static uint32_t stock_cache_releases, stock_shadow_releases, timer_stops, timer_deletes;
static uint32_t buzzer_resets, microphone_cleanups, als_cleanups, compass_stops, dashboard_starts;
static uint32_t delete_fail;
static void cfw_texture_cache_release(customCfwContext *ctx) {
    assert(!service_context);
    ++stock_cache_releases; ctx->texture_cache = 0;
}
static void cfw_shadow_release(customCfwContext *ctx) {
    ++stock_shadow_releases; ctx->framebuffer_shadow = 0;
}
static int stock_timer_stop(uint32_t timer) { assert(timer); ++timer_stops; return 0; }
static int stock_timer_delete(uint32_t timer) {
    ++timer_deletes; return timer == 77 && delete_fail ? -1 : 0;
}
static void mic_cleanup_session(void) { ++microphone_cleanups; }
static void als_cleanup_session(void) { ++als_cleanups; }
static customCfwContext *faceclaw_context_if_valid(void) { return &context; }
static void faceclaw_launch_pending_dashboard(customCfwContext *ctx) { (void)ctx; assert(0); }
static void faceclaw_arm_fallback(customCfwContext *ctx, uint32_t ms) { (void)ctx; (void)ms; assert(0); }
static void faceclaw_send_wear_event(uint32_t worn) { (void)worn; assert(0); }
int cfw_wake_lease_active(void) { return 0; }
#undef FW_MS_TICK
#define FW_MS_TICK tick()
#define FW_TIMER_STOP stock_timer_stop
#define FW_TIMER_DELETE stock_timer_delete
#define FW_BUZZ_RESET() (++buzzer_resets)
#define FW_SIDE() 1
#define FW_COMPASS_STOP() (++compass_stops)
#define FW_APP_START(a, b, c, d) (++dashboard_starts)
#define FW_DISPLAY_START(a, b, c, d) ((void)0)
#define FW_WEAR_STATUS() 1u
#define FACECLAW_PROTO_VERSION 1u
#define FACECLAW_LEASE_MS 90000u
#define FACECLAW_CLAIMED_MS 5000u
#define FACECLAW_OP_ACQUIRE 1u
#define FACECLAW_OP_RELEASE 2u
#define FACECLAW_OP_CLAIM 3u
#define FACECLAW_OP_READY 4u
#define FACECLAW_OP_FB_ACQUIRE 5u
#define FACECLAW_OP_FB_RELEASE 6u
#define FACECLAW_OP_WEAR_QUERY 7u
#include "upstream_cleanup.h"

static void cleanup(void) {
    uint8_t p[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    for (uint32_t pending = 0; pending < 2; ++pending) {
        configure(); request_next = 0;
        stock_cache_releases = stock_shadow_releases = timer_stops = timer_deletes = 0;
        buzzer_resets = microphone_cleanups = als_cleanups = compass_stops = dashboard_starts = 0;
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_ACCEPTED);
        if (!pending) { pool_drain(); await_ready(); }
        uint32_t allocations = allocations_live, freed = release_calls, stopped = terminates;
        context.texture_cache = (void *)(uintptr_t)1;
        context.seq_timer = 77; context.wake_fallback_timer = 78;
        context.seq_count = context.seq_cursor = context.compass_forward = context.wake_dashboard_pending = 1;
        context.direct_pending = context.direct_active = context.direct_failed = context.wake_nonce = 1;
        context.direct_shadow = (void *)(uintptr_t)1;
        delete_fail = 1;
        assert(!take(1, 0)); assert(!cfw_cleanup_session()); give(1);
        assert(!context.video_control.start_guard && allocations_live == allocations &&
               release_calls == freed && terminates == stopped);
        assert(!context.direct_lease_deadline && !context.direct_active && !context.direct_pending &&
               !context.direct_shadow && !context.direct_failed && !context.texture_cache &&
               !context.framebuffer_shadow && !context.seq_count && !context.seq_cursor &&
               !context.compass_forward && !context.wake_dashboard_pending && !context.wake_nonce &&
               !context.wake_fallback_timer && context.seq_timer == 77 && context.diag_hide);
        assert(stock_cache_releases == 1 && stock_shadow_releases == 1 && timer_stops == 2 &&
               timer_deletes == 2 && buzzer_resets == 1 && microphone_cleanups == 1 &&
               als_cleanups == 1 && compass_stops == 1 && dashboard_starts == 1);
        await_reclaimed();
        delete_fail = 0;
        assert(!take(1, 0)); assert(!cfw_cleanup_session()); give(1);
        assert(!context.seq_timer && timer_deletes == 3 && compass_stops == 1 && dashboard_starts == 1);
        finish();
    }
    for (uint32_t allocation = 1; allocation <= 2; ++allocation) {
        configure(); request_next = 0; pause_at = allocation;
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_ACCEPTED);
        pthread_t controller;
        assert(!pthread_create(&controller, 0, pool_run, 0));
        assert(!pthread_mutex_lock(&preparation));
        while (!preparing_paused)
            assert(!pthread_cond_wait(&preparation_changed, &preparation));
        assert(!pthread_mutex_unlock(&preparation));
        assert(!take(1, 0)); assert(!cfw_cleanup_session()); give(1);
        assert(!context.video_control.start_guard && !creates);
        assert(!pthread_mutex_lock(&preparation)); resume_preparation = 1;
        assert(!pthread_cond_broadcast(&preparation_changed));
        assert(!pthread_mutex_unlock(&preparation));
        assert(!pthread_join(controller, 0));
        await_reclaimed(); finish();
    }
    puts("actual mode-11 effects, failed timer delete retry and deferred pending/live reclaim PASS");
}
static void lease_notifications(void) {
    uint8_t p[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    configure(); request_next = 0;
    service_context = 1;
    video_control_notify_lease(1); video_control_notify_lease(0);
    service_context = 0;
    assert(!pool_calls && !allocations_live); /* Boot/idle has no video work. */
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    uint32_t freed = release_calls, stopped = terminates, allocations = allocation_calls;
    service_context = 1;
    video_control_notify_lease(0); video_control_notify_lease(1);
    service_context = 0;
    assert(release_calls == freed && terminates == stopped && allocation_calls == allocations &&
           context.video_control.start_guard); /* Notification only posts stable signals. */
    assert(receive_header(1, 0, 0x67)); /* Refuse before deferred controller executes. */
    assert(release_calls == freed && terminates == stopped);
    await_reclaimed();
    control_command(p, VIDEO_CONTROL_START, 2);
    assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    uint32_t current = context.video_control.control_generation;
    __atomic_store_n(&context.video_control.notify_release_generation, current - 1, __ATOMIC_RELEASE);
    __atomic_store_n(&context.video_control.notify_failed_generation, current - 1, __ATOMIC_RELEASE);
    assert(!take(1, 0)); assert(video_controller_request_locked(VIDEO_CONTROLLER_LEASE)); give(1);
    pool_drain(); assert(state() == VIDEO_READY); /* An older tag cannot stop this owner. */
    const uint8_t acquire[] = {'F','C',1,5,0,0}, release[] = {'F','C',1,6,0,0};
    faceclaw_apply_control(acquire, 6); pool_drain(); assert(state() == VIDEO_READY);
    faceclaw_apply_control(release, 6); await_reclaimed();
    assert(!context.direct_lease_deadline && !cfw_fb_lease_active());
    context.direct_lease_deadline = tick() + 60000;
    control_command(p, VIDEO_CONTROL_START, 3);
    assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    __atomic_store_n(&context.direct_lease_deadline, tick() - 1, __ATOMIC_RELEASE);
    faceclaw_apply_control(acquire, 6); await_reclaimed(); /* Renewing expired lease is a fresh session. */
    control_command(p, VIDEO_CONTROL_START, 4);
    assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    __atomic_store_n(&context.direct_lease_deadline, tick() - 1, __ATOMIC_RELEASE);
    assert(!cfw_fb_lease_active()); await_reclaimed();
    finish();
    configure(); request_next = 0;
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    freed = release_calls; stopped = terminates;
    fail_pool = 1; service_context = 1;
    video_control_notify_lease(0);
    service_context = 0;
    assert(release_calls == freed && terminates == stopped);
    fail_pool = 0;
    control_command(p, VIDEO_CONTROL_STATUS, 0);
    assert(control_snapshot(p, 8, report) == VIDEO_CONTROL_ACCEPTED && report[3] == VIDEO_QUARANTINED &&
           video_read32(report + 24) == VIDEO_CONTROL_DISPATCH && allocations_live && !terminates);
    uint32_t limit = tick() + 3000;
    while (!context.video.parked) { assert((int32_t)(limit - tick()) > 0); delay(1); }
    pool_drain(); clean_quarantine();
    puts("service-only atomic signals, old-generation refusal and actual FB lease release/expiry PASS");
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "normal")) normal();
    else if (!strcmp(argv[1], "allocations")) allocation_failures();
    else if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "reserves")) reserves();
    else if (!strcmp(argv[1], "pins")) pins();
    else if (!strcmp(argv[1], "quarantine")) quarantine();
    else if (!strcmp(argv[1], "wakeups")) wakeups();
    else if (!strcmp(argv[1], "preparation")) preparation_cancel();
    else if (!strcmp(argv[1], "controls")) controls();
    else if (!strcmp(argv[1], "control-preparation")) control_preparation();
    else if (!strcmp(argv[1], "control-failures")) control_failures();
    else if (!strcmp(argv[1], "nal-input")) nal_input();
    else if (!strcmp(argv[1], "queue-extension")) queue_extension();
    else if (!strcmp(argv[1], "recovery")) recovery();
    else if (!strcmp(argv[1], "inactivity")) inactivity();
    else if (!strcmp(argv[1], "cleanup")) cleanup();
    else if (!strcmp(argv[1], "lease-notifications")) lease_notifications();
    else assert(0);
    assert(!allocations_live && !task_live && !image_depth);
    printf("worker %s PASS\n", argv[1]);
}
