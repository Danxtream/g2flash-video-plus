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
#include "../../patches/video/platform.h"

/* Native threads need sanitizer stacks. The firmware's static stack is checked
 * as a separate sentinel buffer; these tests do not infer ARM stack usage. */
static customCfwContext context;
static pthread_mutex_t image = PTHREAD_MUTEX_INITIALIZER;
static pthread_t worker;
static _Thread_local uint32_t image_depth, worker_thread;
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
static const uint32_t shadow_present = 1;

static void test_zero(uint8_t *p, uint32_t n) { memset(p, 0, n); }
static customCfwContext *peekCustomCfwContext(void) { return &context; }
static customCfwContext *getCustomCfwContext(void) { return &context; }
static uint32_t tick(void) {
    struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t));
    return (uint32_t)(t.tv_sec * 1000u + t.tv_nsec / 1000000u);
}
static int take(uint32_t m, uint32_t timeout) {
    assert(m == 1 && (!timeout || !image_depth) && timeout <= 2000);
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
static uint32_t new_thread(void (*fn)(void *), void *arg, const video_thread_attr *attr);
static int terminate(uint32_t id);
static int delay(uint32_t ms);
static uint32_t event_new(const video_event_attr *);
static uint32_t event_set(uint32_t, uint32_t);
static uint32_t event_wait(uint32_t, uint32_t, uint32_t, uint32_t);
static int video_platform_event_quiescent(uint32_t);
static void *allocate(uint32_t size) {
    assert(!image_depth);
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
    assert(!image_depth && p);
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
    assert(!p && !image_depth);
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
#include "../../patches/video/worker.c"

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
    assert(id && id <= 4 && events[id - 1].control && bits && !(bits & 0xff000000u));
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
    else assert(0);
    assert(!allocations_live && !task_live && !image_depth);
    printf("worker %s PASS\n", argv[1]);
}
