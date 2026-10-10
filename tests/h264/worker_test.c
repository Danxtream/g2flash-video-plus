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
static uint32_t consume_enabled, decoded_calls, consumed_frames, decode_paused, refuse_extension;
static uint32_t upload_limit;
static uint32_t cycle_reads, clock_inits, calibrations, crc_calls, anomalous_clock;
static uint32_t forced_cycles, force_cycles;
static uint32_t evidence_drain, evidence_next, evidence_checked, evidence_was_full;
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
static void (*heap_sample_hook)(void);
static const uint32_t shadow_present = 1;
typedef struct { uint32_t direct_submitted, direct_failed; } cfw_rectlist;
static void present_shadow(uint32_t, uint32_t, cfw_rectlist *);
static uint8_t *cfw_shadow_buffer(void);
void display_copy_hook(void);
static void display_pump(void);
static void *display_thread_entry(void *);
static uint32_t display_gate, display_job, fail_gate, fail_queue, missing_fb;
static uint32_t display_allocated, fail_shadow, copied_frames, overlay_calls, flush_calls, stock_copies;
static uint8_t physical_frame[VIDEO_PANEL_BYTES];
static uint8_t *owned_shadow;
static int display_take(void) {
    assert(image_depth && !service_context);
    if (fail_gate) return 0;
    assert(!display_gate); display_gate = 1; return 1;
}
static void display_signal(void) { assert(display_gate); display_gate = 0; }
static void *shadow_allocate(uint32_t bytes) {
    assert(!image_depth && !service_context && bytes == VIDEO_PANEL_BYTES && !owned_shadow);
    if (fail_shadow) return 0;
    owned_shadow = malloc(bytes); assert(owned_shadow); display_allocated = bytes + 4;
    return owned_shadow;
}
static void shadow_free(void *pointer) {
    assert(!image_depth && !service_context && pointer == owned_shadow && !context.direct_pending);
    free(pointer); owned_shadow = 0; display_allocated = 0;
}
#define VIDEO_DISPLAY_TAKE display_take
#define VIDEO_DISPLAY_SIGNAL display_signal
#define VIDEO_SHADOW_ALLOC shadow_allocate
#define VIDEO_SHADOW_FREE shadow_free

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
static uint32_t controller_used = 512, controller_samples;
static int stack_sample(uint32_t *task, uint32_t *used, uint32_t *bytes) {
    assert(!service_context && !image_depth && !worker_thread);
    ++controller_samples;
    *task = 3; *used = controller_used; *bytes = 4096; return 1;
}
#define VIDEO_PLATFORM_STACK_SAMPLE stack_sample
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
    if (heap_sample_hook) { void (*hook)(void) = heap_sample_hook; heap_sample_hook = 0; hook(); }
    uint32_t i = heap == 20 ? 0 : heap == 13 ? 1 : 2;
    if (fail_view) return 0;
    *out = views[i];
    if (!i) {
        out->free_bytes -= cached_used;
        if (out->max_alloc > out->free_bytes) out->max_alloc = out->free_bytes;
    }
    if (i == 1) {
        out->free_bytes -= display_allocated;
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
#include "../../patches/h264/g2_h264.h"
static uint32_t test_cycles(void) {
    __atomic_fetch_add(&cycle_reads, 1, __ATOMIC_RELAXED);
    return force_cycles ? forced_cycles : tick() * 250000;
}
static void test_clock_init(void) { __atomic_fetch_add(&clock_inits, 1, __ATOMIC_RELAXED); }
static uint32_t test_calibrate(void) {
    uint32_t n = __atomic_fetch_add(&calibrations, 1, __ATOMIC_RELAXED);
    return anomalous_clock ? (n % 2 ? 300000000 : 0) : 250000000;
}
static uint32_t test_crc(const g2_h264_frame_info *);
#define VIDEO_CYCLES test_cycles()
#define VIDEO_CLOCK_INIT test_clock_init
#define VIDEO_CLOCK_CALIBRATE test_calibrate
#define VIDEO_FRAME_CRC test_crc
static g2_h264_result worker_decode(void *, const uint8_t *, uint32_t);
#include "../../patches/video/runtime_provider.c"
#include "../../patches/video/storage.c"
#include "../../patches/video/lifecycle.c"
#include "../../patches/video/queue.c"
#define g2_h264_decode worker_decode
#include "../../patches/video/decode_timing.c"
#include "../../patches/video/worker.c"
#undef g2_h264_decode
#include "../../patches/video/pack.c"
#include "../../patches/video/present.c"
#include "../../patches/video/evidence.c"
#include "../../patches/video/controller.c"
#include "../../patches/video/diagnostics.c"
#include "../../patches/video/control.c"
static uint32_t test_crc(const g2_h264_frame_info *frame) {
    __atomic_fetch_add(&crc_calls, 1, __ATOMIC_RELAXED); return video_frame_crc(frame);
}

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

/* Pause the real ABI call, not the queue/owner logic, so ingress regression
 * cases can inspect immutable slots while the consumer holds one borrowed
 * input. New consumer cases release this seam and exercise the real decoder. */
static g2_h264_result worker_decode(void *handle, const uint8_t *nal, uint32_t bytes) {
    assert(worker_thread && !image_depth);
    while (!__atomic_load_n(&consume_enabled, __ATOMIC_ACQUIRE) &&
           !video_lifecycle_cancelled(&context.video, context.video.token)) {
        __atomic_store_n(&decode_paused, 1, __ATOMIC_RELEASE);
        delay(1);
    }
    if (video_lifecycle_cancelled(&context.video, context.video.token)) return G2_H264_CONSUMED;
    ++decoded_calls;
    g2_h264_result result = g2_h264_decode(handle, nal, bytes);
    if (result == G2_H264_FRAME_READY) {
        g2_h264_frame_info frame;
        assert(g2_h264_frame(handle, &frame) == G2_H264_FRAME_READY);
        assert(frame.width == 320 && frame.height == 192 && frame.count == consumed_frames + 1);
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < frame.width; ++x)
                assert(frame.y[y * frame.stride + x] == 128);
        ++consumed_frames;
        if (refuse_extension && consumed_frames == 2) views[0].max_alloc = 8000;
    }
    return result;
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
    assert(!display_job && !display_gate && !owned_shadow);
    fail_gate = fail_queue = missing_fb = fail_shadow = copied_frames = overlay_calls = flush_calls = stock_copies = 0;
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
    controller_used = 512; controller_samples = 0; heap_sample_hook = 0;
    consume_enabled = decoded_calls = consumed_frames = decode_paused = refuse_extension = 0;
    upload_limit = 0;
    cycle_reads = clock_inits = calibrations = crc_calls = anomalous_clock = 0;
    forced_cycles = force_cycles = 0;
    evidence_drain = evidence_checked = evidence_was_full = 0; evidence_next = 1;
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
static uint8_t control_response(const uint8_t *p, uint32_t n, uint8_t *snapshot, uint32_t length) {
    int result = video_control_received(p, n, &control_route);
    assert(control_reply_size == 9 && control_reply[7] == length);
    uint8_t page[9] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_PAGE, VIDEO_PROTOCOL_VERSION};
    memcpy(page + 4, p + 4, 4);
    for (uint32_t i = 0; i < length; ++i) {
        page[8] = i;
        assert(!video_control_received(page, sizeof(page), &control_route));
        assert(control_reply[6] == i && control_reply[7] == length);
        snapshot[i] = control_reply[8];
    }
    assert((result == 0) == (snapshot[2] == VIDEO_CONTROL_ACCEPTED));
    return snapshot[2];
}
static uint8_t control_snapshot(const uint8_t *p, uint32_t n, uint8_t *snapshot) {
    memset(snapshot, 0, VIDEO_STATUS_BYTES);
    return control_response(p, n, snapshot, p[1] == VIDEO_CONTROL_STATUS ?
                            VIDEO_STATUS_BYTES : VIDEO_BASE_STATUS_BYTES);
}
static uint8_t control_variable_pages(const uint8_t *p, uint32_t n, uint8_t *snapshot,
                                      uint32_t length, uint8_t lens, uint8_t capacity,
                                      uint8_t page_capacity) {
    if (length == VIDEO_STATUS_BYTES && p[1] != VIDEO_CONTROL_STATUS) {
        memset(snapshot, 0, length); length = VIDEO_BASE_STATUS_BYTES;
    }
    cfw_message_route route = {lens, lens, lens, capacity, 0};
    control_reply_size = 0;
    int result = video_control_received(p, n, &route);
    assert(control_reply_size > VIDEO_REPLY_HEADER_BYTES && control_reply[6] == 0);
    uint32_t count = control_reply_size - VIDEO_REPLY_HEADER_BYTES;
    uint32_t pages = (length + count - 1) / count;
    assert(control_reply[7] == pages);
    memcpy(snapshot, control_reply + VIDEO_REPLY_HEADER_BYTES, count);
    uint8_t page[9] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_PAGE, VIDEO_PROTOCOL_VERSION};
    memcpy(page + 4, p + 4, 4); route.reply_capacity = page_capacity;
    for (uint32_t i = 1; i < pages; ++i) {
        page[8] = i; control_reply_size = 0;
        assert(!video_control_received(page, sizeof(page), &route));
        uint32_t offset = i * count, bytes = length - offset;
        if (bytes > count) bytes = count;
        assert(control_reply_size == VIDEO_REPLY_HEADER_BYTES + bytes &&
               control_reply[6] == i && control_reply[7] == pages);
        memcpy(snapshot + offset, control_reply + VIDEO_REPLY_HEADER_BYTES, bytes);
    }
    assert((result == 0) == (snapshot[2] == VIDEO_CONTROL_ACCEPTED));
    return snapshot[2];
}
static int credit_response(const uint8_t *p, cfw_message_route route, uint8_t snapshot[6]) {
    int result = video_control_received(p, VIDEO_CREDITS_REQUEST_BYTES, &route);
    uint32_t bytes = control_reply_size - VIDEO_REPLY_HEADER_BYTES;
    assert(bytes && bytes <= VIDEO_CREDITS_BYTES && control_reply[6] == 0);
    uint32_t pages = (VIDEO_CREDITS_BYTES + bytes - 1) / bytes;
    assert(control_reply[7] == pages && control_reply[1] == route.here);
    memcpy(snapshot, control_reply + VIDEO_REPLY_HEADER_BYTES, bytes);
    uint8_t page[9] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_PAGE, VIDEO_PROTOCOL_VERSION};
    memcpy(page + 4, p + 4, 4);
    route.reply_capacity = route.reply_capacity < VIDEO_PAGE_REPLY_CAPACITY ?
        route.reply_capacity : VIDEO_PAGE_REPLY_CAPACITY;
    for (uint32_t i = 1; i < pages; ++i) {
        page[8] = i;
        assert(!video_control_received(page, sizeof(page), &route));
        uint32_t n = VIDEO_CREDITS_BYTES - i * bytes;
        if (n > bytes) n = bytes;
        memcpy(snapshot + i * bytes, control_reply + VIDEO_REPLY_HEADER_BYTES, n);
    }
    assert((result == 0) == (snapshot[5] == VIDEO_CONTROL_ACCEPTED));
    return snapshot[5];
}
static void credits(void) {
    /* Separate recipients select their own token; the same bridge origin is
     * retained. Test asymmetric state without treating a local ACK as paired. */
    for (uint8_t here = 1; here <= 2; ++here) for (uint8_t origin = 1; origin <= 2; ++origin)
    for (uint8_t budget = 9; budget <= 21; budget += 12) {
        configure(); request_next = 0;
        uint8_t p[VIDEO_START_BYTES], frozen[6], current[6], saved[VIDEO_START_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        cfw_message_route route = {here, origin, CFW_MESSAGE_BOTH, budget, 0};
        assert(!video_control_received(p, 24, &route));
        pool_drain(); await_ready();
        uint32_t token = context.video.token;
        control_command(p, VIDEO_CONTROL_CREDITS, 1);
        video_write32(p + 12, here == 1 ? token : token + 19);
        video_write32(p + 16, here == 2 ? token : token + 19);
        memcpy(saved, p, sizeof(saved));
        assert(!credit_response(p, route, frozen));
        assert(!video_read32(frozen) && (frozen[4] & 7) == 4 &&
               (frozen[4] >> VIDEO_CREDIT_STATE_SHIFT & 7) == VIDEO_READY);
        assert(control_reply[7] == (budget == 9 ? 6 : 1));
        uint32_t deadline = context.video_control.active_deadline;
        assert(!take(1, 1000));
        const uint8_t header[] = {0x67};
        video_owner *owner = context.video_owner;
        assert(video_queue_push(&owner->queue, 0, header, sizeof(header)) == 1);
        video_nal_view view;
        /* Claim before a fresh credit query wakes the actual worker. */
        assert(video_queue_claim(&owner->queue, token, &view));
        assert(!give(1));
        assert(!credit_response(saved, route, current) && !memcmp(current, frozen, 6));
        assert(context.video_control.active_deadline == deadline);
        control_command(p, VIDEO_CONTROL_CREDITS, 1);
        video_write32(p + (here == 1 ? 12 : 16), token);
        assert(!credit_response(p, route, current) && (current[4] & 7) == 3);
        assert(!take(1, 1000));
        assert(video_queue_release(&owner->queue, &view));
        assert(!give(1));
        control_command(p, VIDEO_CONTROL_CREDITS, 1);
        video_write32(p + (here == 1 ? 12 : 16), token);
        assert(!credit_response(p, route, current) && video_read32(current) == 1 &&
               (current[4] & 7) == 4);
        assert(!take(1, 1000));
        uint8_t extension[2 * VIDEO_SLOT_BYTES];
        assert(video_queue_extend(&owner->queue, extension));
        assert(video_queue_push(&owner->queue, 2, header, sizeof(header)) == 1);
        assert(video_queue_watch(&owner->queue, tick()));
        assert(!give(1));
        control_command(p, VIDEO_CONTROL_CREDITS, 1);
        video_write32(p + (here == 1 ? 12 : 16), token);
        uint32_t allocations = allocation_calls;
        assert(!credit_response(p, route, current) && video_read32(current) == 1 &&
               (current[4] & 7) == 5 && (current[4] & VIDEO_CREDIT_CAPACITY_SIX) &&
               (current[4] & VIDEO_CREDIT_GAP) && allocation_calls == allocations);
        memcpy(saved, p, sizeof(saved));
        saved[12] ^= 1; saved[16] ^= 1;
        assert(video_control_received(saved, 20, &route) == -1); /* Conflicting cached request. */
        control_command(p, VIDEO_CONTROL_CREDITS, 1);
        video_write32(p + (here == 1 ? 12 : 16), token + 1);
        assert(credit_response(p, route, current) == VIDEO_CONTROL_STALE && !(current[4] & 7));
        control_command(p, VIDEO_CONTROL_CREDITS, 2);
        video_write32(p + (here == 1 ? 12 : 16), token);
        assert(credit_response(p, route, current) == VIDEO_CONTROL_STALE && !(current[4] & 7));
        control_command(p, VIDEO_CONTROL_STOP, 1);
        assert(!video_control_received(p, 12, &route));
        pool_drain(); finish();
        control_command(p, VIDEO_CONTROL_CREDITS, 1);
        video_write32(p + (here == 1 ? 12 : 16), token);
        assert(credit_response(p, route, current) == VIDEO_CONTROL_STALE && !(current[4] & 7));
    }
    puts("owner-bound six-byte credits, both origins/recipients, frozen retries and MTU23 PASS");
}
static void control_paging(void) {
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES], retry[VIDEO_STATUS_BYTES];
    /* Uncompressed records add five bytes. At a large MTU the transport
     * infers capacity 29 for START, but only 14 for each nine-byte PAGE.
     * MTU23 fragmentation instead leaves all these capacities at nine. */
    for (uint8_t lens = 1; lens <= 2; ++lens) {
        for (uint32_t large_mtu = 0; large_mtu <= 1; ++large_mtu) {
            configure(); request_next = 0;
            uint8_t page_capacity = large_mtu ? 14 : 9;
            control_command(p, VIDEO_CONTROL_CAPABILITIES, 0);
            assert(control_variable_pages(p, 8, snapshot, sizeof(snapshot), lens,
                large_mtu ? 13 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            assert(snapshot[3] == VIDEO_IDLE);
            control_command(p, VIDEO_CONTROL_START, 1);
            assert(control_variable_pages(p, 24, snapshot, sizeof(snapshot), lens,
                large_mtu ? 29 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            assert(snapshot[3] == VIDEO_STARTING && pool_calls == 1 && !allocations_live);
            assert(control_variable_pages(p, 24, retry, sizeof(retry), lens,
                large_mtu ? 29 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            assert(!memcmp(snapshot, retry, sizeof(snapshot)) && pool_calls == 1);
            control_command(p, VIDEO_CONTROL_FRAME_READ, 1);
            control_variable_pages(p, 20, snapshot, VIDEO_FRAME_RESULT_BYTES, lens,
                large_mtu ? 25 : 9, page_capacity);
            control_command(p, VIDEO_CONTROL_STOP, 1);
            assert(control_variable_pages(p, 12, snapshot, sizeof(snapshot), lens,
                large_mtu ? 17 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            pool_drain(); finish();
        }
    }
    puts("request-sized replies, smaller PAGE budgets, exact START retries and both lenses PASS");
}
static void controls(void) {
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    configure(); request_next = 0;
    control_command(p, VIDEO_CONTROL_CAPABILITIES, 0);
    assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
    assert(snapshot[1] == 7 && snapshot[3] == VIDEO_IDLE && !snapshot[50] &&
           !allocations_live && !creates && !pool_calls);
    control_command(p, VIDEO_CONTROL_START, 1);
    context.texture_cache = (void *)(uintptr_t)1;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_LEASE && !pool_calls);
    context.texture_cache = 0;
    control_command(p, VIDEO_CONTROL_START, 1);
    p[18] = 2;
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

static void evict_diagnostics(void) {
    uint8_t request[VIDEO_START_BYTES];
    for (uint32_t i = 0; i < VIDEO_CONTROL_REPLAYS; ++i) {
        control_command(request, VIDEO_CONTROL_STATUS, 0);
        assert(!video_control_received(request, 8, &control_route));
    }
}
static void diagnostics(void) {
    configure(); request_next = 0;
    for (uint32_t i = 0; i < VIDEO_POOL_TASKS; ++i) {
        uint32_t id = VIDEO_POOL_TCBS + i * VIDEO_POOL_TCB_BYTES;
        uint32_t base = VIDEO_POOL_STACKS + i * VIDEO_POOL_STACK_BYTES;
        assert(video_stack_pool_index(id) == (int)i);
        assert(video_stack_pool_valid(id, id, base, 1024, 2));
        assert(!video_stack_pool_valid(id, id + 1, base, 1024, 2));
        assert(!video_stack_pool_valid(id, id, base + 4, 1024, 2));
        assert(!video_stack_pool_valid(id, id, base, 1025, 2));
        assert(!video_stack_pool_valid(id, id, base, 1024, 1));
    }
    assert(video_stack_pool_index(VIDEO_POOL_TCBS - 1) < 0);
    assert(video_stack_pool_index(VIDEO_POOL_TCBS + 1) < 0);
    assert(video_stack_pool_index(VIDEO_POOL_TCBS + 8 * VIDEO_POOL_TCB_BYTES) < 0);
    uint8_t stack[16]; memset(stack, VIDEO_STACK_FILL, sizeof(stack));
    assert(video_stack_unused(stack, sizeof(stack)) == sizeof(stack));
    stack[7] = 0; assert(video_stack_unused(stack, sizeof(stack)) == 4);
    stack[0] = 0; assert(!video_stack_unused(stack, sizeof(stack)));
    assert(!video_stack_unused(0, 0));
    uint8_t request[VIDEO_START_BYTES], frozen[VIDEO_DIAGNOSTICS_BYTES];
    control_command(request, VIDEO_CONTROL_DIAGNOSTICS, 0);
    assert(!video_control_received(request, 8, &control_route));
    assert(!allocation_calls && !creates && control_reply[7] == VIDEO_DIAGNOSTICS_BYTES);
    uint8_t page[9] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_PAGE, VIDEO_PROTOCOL_VERSION};
    memcpy(page + 4, request + 4, 4);
    for (uint32_t i = 0; i < sizeof(frozen); ++i) {
        page[8] = i; assert(!video_control_received(page, sizeof(page), &control_route));
        frozen[i] = control_reply[8];
    }
    assert(frozen[0] == 1 && frozen[1] == VIDEO_CONTROL_DIAGNOSTICS && !frozen[2] &&
           frozen[3] == 7 && video_read32(frozen + 16) == views[0].free_bytes &&
           !video_read32(frozen + 124));
    views[0].free_bytes = views[0].max_alloc = 99999;
    assert(!video_control_received(request, 8, &control_route));
    page[8] = 16; assert(!video_control_received(page, sizeof(page), &control_route));
    assert(control_reply[8] == frozen[16]); /* Frozen replay does not resample. */
    fail_view = 1;
    control_command(request, VIDEO_CONTROL_DIAGNOSTICS, 0);
    assert(!video_control_received(request, 8, &control_route));
    video_control_replay *last = &context.video_control.replay[0][1];
    assert(!(last->snapshot[3] & 7) && video_read32(last->snapshot + 16) == UINT32_MAX);
    fail_view = 0;
    controller_used = 1200; video_controller_stack_sample();
    controller_used = 800; video_controller_stack_sample();
    assert(context.video_control.stack_used == 1200 && context.video_control.stack_task == 3);
    control_command(request, VIDEO_CONTROL_DIAGNOSTICS, 0);
    assert(!video_control_received(request, 8, &control_route));
    last = &context.video_control.replay[0][2];
    assert(last->snapshot[3] & 16 && video_read32(last->snapshot + 120) == 1200 &&
           video_read32(last->snapshot + 124) == 4096);
    control_command(request, VIDEO_CONTROL_DIAGNOSTICS, 0);
    heap_sample_hook = evict_diagnostics;
    assert(video_control_received(request, 8, &control_route) == -1);
    assert(!allocation_calls && !creates && !image_depth);
    puts("bounded stack scan, live invalid heaps, frozen MTU23 and concurrent replay eviction PASS");
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
    uint32_t pause_limit = tick() + 3000;
    while (!__atomic_load_n(&decode_paused, __ATOMIC_ACQUIRE)) {
        assert((int32_t)(pause_limit - tick()) > 0); delay(1);
    }
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

int worker_enqueue_stream(void);
static void drain_frame_results(void) {
    if (!evidence_drain) return;
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    control_command(p, VIDEO_CONTROL_STATUS, 0);
    assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
    uint32_t high = video_read32(snapshot + 100), acked = video_read32(snapshot + 104);
    if (!evidence_was_full) {
        if (high < VIDEO_EVIDENCE_ROWS) return;
        evidence_was_full = 1;
        assert(high == VIDEO_EVIDENCE_ROWS && !acked && copied_frames == VIDEO_EVIDENCE_ROWS);
        delay(5); display_pump(); pool_drain();
        assert(copied_frames == VIDEO_EVIDENCE_ROWS);
    }
    while (evidence_next <= high) {
        control_command(p, VIDEO_CONTROL_FRAME_READ, 1);
        video_write32(p + 12, context.video.token); video_write32(p + 16, evidence_next);
        assert(control_response(p, 20, snapshot, VIDEO_FRAME_RESULT_BYTES) == VIDEO_CONTROL_ACCEPTED);
        assert(video_read32(snapshot + 4) == 1 && video_read32(snapshot + 8) == context.video.token &&
               video_read32(snapshot + 12) == evidence_next && video_read32(snapshot + 16) == evidence_next);
        uint8_t pixels[VIDEO_FRAME_WIDTH * VIDEO_FRAME_HEIGHT]; memset(pixels, 128, sizeof(pixels));
        g2_h264_frame_info frame = {0};
        frame.y = pixels; frame.width = VIDEO_FRAME_WIDTH; frame.height = VIDEO_FRAME_HEIGHT;
        frame.stride = VIDEO_FRAME_WIDTH;
        assert(video_read32(snapshot + 28) == (VIDEO_FRAME_WIDTH | VIDEO_FRAME_HEIGHT << 16) &&
               video_read32(snapshot + 32) == video_frame_crc(&frame));
        assert(video_read32(snapshot + 20) == (evidence_next == 1 ? 0 : evidence_next + 1) &&
               video_read32(snapshot + 24) == evidence_next + 1 &&
               video_read32(snapshot + 64) == (evidence_next == 1 ? 3 : 1));
        assert(!!video_read32(snapshot + 52) == !!anomalous_clock);
        ++evidence_next; ++evidence_checked;
    }
    if (high > acked) {
        control_command(p, VIDEO_CONTROL_FRAME_ACK, 1);
        video_write32(p + 12, context.video.token); video_write32(p + 16, high);
        assert(control_response(p, 20, snapshot, VIDEO_FRAME_ACK_BYTES) == VIDEO_CONTROL_ACCEPTED &&
               video_read32(snapshot + 12) == high);
        uint32_t deadline = context.video_control.active_deadline;
        assert(control_response(p, 20, snapshot, VIDEO_FRAME_ACK_BYTES) == VIDEO_CONTROL_ACCEPTED &&
               context.video_control.active_deadline == deadline);
    }
}
int worker_upload_nal(const uint8_t *nal, uint32_t bytes, uint32_t sequence) {
    assert(bytes <= VIDEO_RAW_NAL_BYTES && !worker_thread);
    if (upload_limit && sequence >= upload_limit) return 0;
    uint8_t record[VIDEO_SLOT_BYTES] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_NAL};
    video_write32(record + 2, 1); video_write32(record + 6, sequence);
    memcpy(record + VIDEO_NAL_HEADER_BYTES, nal, bytes);
    uint32_t limit = tick() + 3000;
    for (;;) {
        display_pump(); pool_drain(); drain_frame_results();
        if (!video_control_received(record, VIDEO_NAL_HEADER_BYTES + bytes, &control_route)) break;
        assert((int32_t)(limit - tick()) > 0 && state() == VIDEO_READY); delay(1);
    }
    /* Uploaded input is no longer borrowed. The same sequence can be retried
     * exactly even if consumption happened before the transport reply. */
    assert(!video_control_received(record, VIDEO_NAL_HEADER_BYTES + bytes, &control_route));
    memset(record + VIDEO_NAL_HEADER_BYTES, 0xee, bytes);
    return 1;
}

static void consumer(void) {
    for (uint32_t refused = 0; refused < 2; ++refused) {
        configure(); request_next = 0; consume_enabled = 1;
        context.framebuffer_shadow = 0;
        uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        p[18] = refused ? VIDEO_PRESENT_NATIVE : 0;
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready();
        refuse_extension = refused; // Refuse only after both real DPB planes were allocated.
        assert(worker_enqueue_stream());
        uint32_t limit = tick() + 5000;
        for (;;) {
            display_pump(); pool_drain();
            control_command(p, VIDEO_CONTROL_STATUS, 0);
            assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
            if (video_read32(snapshot + 60) == 34 && video_read32(snapshot + 64) == 32) break;
            assert((int32_t)(limit - tick()) > 0 && snapshot[3] == VIDEO_READY); delay(1);
        }
        assert(decoded_calls == 34 && consumed_frames == 32 && snapshot[50] == (refused ? 4 : 6));
        assert(video_read32(snapshot + 80) == 32 && copied_frames == 32 && flush_calls == 32 && !overlay_calls);
        assert(!((video_owner *)context.video_owner)->evidence && !video_read32(snapshot + 96) &&
               cycle_reads == 68 && clock_inits == 1 && !calibrations && !crc_calls);
        assert(snapshot[128] == VIDEO_DECODE_TOTAL_VERSION && video_read32(snapshot + 132) == 32 &&
               snapshot[130] == 2 && !snapshot[131]);
        video_decode_totals completed = context.video_decode_last;
        uint32_t buckets = 0;
        for (uint32_t i = 0; i < VIDEO_DECODE_BUCKETS; ++i) buckets += completed.buckets[i];
        assert(buckets == 32);
        control_command(p, VIDEO_CONTROL_STOP, 1);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish();
        assert(!memcmp(&completed, &context.video_decode_last, sizeof(completed)));
        control_command(p, VIDEO_CONTROL_STATUS, 0);
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED && video_read32(snapshot + 132) == 32);
        uint8_t frozen[VIDEO_STATUS_BYTES]; memcpy(frozen, snapshot, sizeof(frozen));
        context.video_decode_last.flags |= VIDEO_DECODE_LONG_SAMPLE;
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED && !memcmp(frozen, snapshot, sizeof(frozen)));
        control_command(p, VIDEO_CONTROL_START, 2);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED && !context.video_decode_last.pictures);
        control_command(p, VIDEO_CONTROL_STOP, 2);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish();
        assert(context.framebuffer_shadow == owned_shadow && owned_shadow);
        shadow_free(owned_shadow); context.framebuffer_shadow = 0;
    }
    configure(); request_next = 0; consume_enabled = 1;
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    assert(!receive_header(1, 0, 0x67)); // Semantically invalid SPS, unlike receipt-only header admission.
    uint32_t limit = tick() + 3000;
    while (!context.video.parked) { assert((int32_t)(limit - tick()) > 0); delay(1); }
    assert(context.video_owner && ((video_owner *)context.video_owner)->queue.consumed == 0);
    pool_drain(); finish();
    assert(context.video_control.error == VIDEO_CONTROL_INPUT);
    puts("64 queued Y planes match; NAL/picture counts differ; semantic error retires without credit PASS");
}
static void await_reclaimed(void) {
    uint32_t limit = tick() + 3000;
    while (state() != VIDEO_IDLE || pool_pending() || context.video_control.controller_job) {
        pool_drain(); assert((int32_t)(limit - tick()) > 0); delay(1);
    }
    assert(!allocations_live && !task_live);
}

static void evidence(void) {
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    uint8_t check[] = {'1','2','3','4','5','6','7','8','9',0xee,0xee};
    g2_h264_frame_info frame = {0};
    frame.y = check; frame.width = 9; frame.height = 1; frame.stride = sizeof(check);
    assert(video_frame_crc(&frame) == 0xcbf43926U);
    video_owner test_owner = {0}; video_evidence window = {0}; test_owner.evidence = &window;
    force_cycles = 1; forced_cycles = 3;
    test_owner.timing.start_cycles = UINT32_MAX - 4; test_owner.timing.start_tick = tick();
    video_decode_after(&test_owner, G2_H264_CONSUMED);
    assert(window.building.cycles == 8 && window.building.no_output_cycles == 8 &&
           !window.building.flags && window.building.calls == 1);
    window.building.cycles = UINT32_MAX - 4;
    video_decode_after(&test_owner, G2_H264_FRAME_READY);
    assert(window.building.flags & VIDEO_TIMING_INVALID);
    force_cycles = 0;
    for (uint32_t anomaly = 0; anomaly < 2; ++anomaly) {
        configure(); request_next = 0; consume_enabled = evidence_drain = 1;
        anomalous_clock = anomaly; context.framebuffer_shadow = 0;
        control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_VERIFY_FRAMES;
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready();
        video_owner *owner = context.video_owner;
        assert(owner->evidence && owner->storage.limit >= VIDEO_EVIDENCE_ALLOWANCE);
        assert(worker_enqueue_stream());
        uint32_t limit = tick() + 5000;
        while (evidence_checked < 32) {
            display_pump(); pool_drain(); drain_frame_results();
            assert((int32_t)(limit - tick()) > 0 && state() == VIDEO_READY); delay(1);
        }
        control_command(p, VIDEO_CONTROL_STATUS, 0);
        assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED &&
               video_read32(snapshot + 96) == VIDEO_EVIDENCE_ROWS &&
               video_read32(snapshot + 100) == 32 && video_read32(snapshot + 104) == 32 &&
               video_read32(snapshot + 108) && video_read32(snapshot + 120) == 2 &&
               video_read32(snapshot + 80) == 32 && !video_read32(snapshot + 24));
        assert(decoded_calls == 34 && copied_frames == 32 && crc_calls == 32 &&
               calibrations == 64 && clock_inits == 1 && cycle_reads == 68 && evidence_was_full);
        control_command(p, VIDEO_CONTROL_STOP, 1);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish(); assert(!context.video_control.error);
        shadow_free(owned_shadow); context.framebuffer_shadow = 0;
    }
    puts("off has bounded aggregates but no window/calibration/CRC/ACK waits; verified rows bounded, lossless and anomalous timing nonfatal PASS");
}

static void evidence_replays(void) {
    for (uint32_t unread = 0; unread < 2; ++unread) {
        configure(); request_next = 0; consume_enabled = 1; upload_limit = 3;
        context.framebuffer_shadow = 0;
        uint8_t p[VIDEO_START_BYTES], saved[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
        uint8_t frozen[VIDEO_FRAME_RESULT_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_VERIFY_FRAMES | VIDEO_PRESENT_NATIVE;
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready(); assert(!worker_enqueue_stream());
        uint32_t limit = tick() + 3000;
        for (;;) {
            display_pump(); pool_drain();
            assert(!take(1, 100));
            uint32_t high = ((video_owner *)context.video_owner)->evidence->high; give(1);
            if (high == 1) break;
            assert((int32_t)(limit - tick()) > 0); delay(1);
        }
        control_command(p, VIDEO_CONTROL_FRAME_ACK, 1);
        video_write32(p + 12, context.video.token); video_write32(p + 16, 2);
        assert(control_response(p, 20, snapshot, VIDEO_FRAME_ACK_BYTES) == VIDEO_CONTROL_FORMAT);
        control_command(p, VIDEO_CONTROL_FRAME_READ, 1);
        video_write32(p + 12, context.video.token + 1); video_write32(p + 16, 1);
        assert(control_response(p, 20, snapshot, VIDEO_FRAME_RESULT_BYTES) == VIDEO_CONTROL_STALE);
        control_command(p, VIDEO_CONTROL_FRAME_READ, 1);
        video_write32(p + 12, context.video.token); video_write32(p + 16, 2);
        assert(control_response(p, 20, snapshot, VIDEO_FRAME_RESULT_BYTES) == VIDEO_CONTROL_BUSY);
        control_command(p, VIDEO_CONTROL_FRAME_READ, 1);
        video_write32(p + 12, context.video.token); video_write32(p + 16, 1);
        cfw_message_route wrong = control_route; wrong.origin = 2; wrong.here = 2; wrong.reply_capacity = 30;
        assert(video_control_received(p, 20, &wrong) == -1 && control_reply[10] == VIDEO_CONTROL_STALE);
        assert(control_response(p, 20, frozen, VIDEO_FRAME_RESULT_BYTES) == VIDEO_CONTROL_ACCEPTED);
        memcpy(saved, p, sizeof(saved));
        if (!unread) {
            control_command(p, VIDEO_CONTROL_FRAME_ACK, 1);
            video_write32(p + 12, context.video.token); video_write32(p + 16, 1);
            assert(control_response(p, 20, snapshot, VIDEO_FRAME_ACK_BYTES) == VIDEO_CONTROL_ACCEPTED);
            assert(control_response(saved, 20, snapshot, VIDEO_FRAME_RESULT_BYTES) == VIDEO_CONTROL_ACCEPTED &&
                   !memcmp(snapshot, frozen, sizeof(frozen)));
            control_command(p, VIDEO_CONTROL_FRAME_READ, 1);
            video_write32(p + 12, context.video.token); video_write32(p + 16, 1);
            assert(control_response(p, 20, snapshot, VIDEO_FRAME_RESULT_BYTES) == VIDEO_CONTROL_STALE);
        }
        control_command(p, VIDEO_CONTROL_STOP, 1);
        assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish();
        assert(context.video_control.error == (unread ? VIDEO_CONTROL_INCOMPLETE : 0));
        shadow_free(owned_shadow); context.framebuffer_shadow = 0;
    }
    configure(); request_next = 0;
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_VERIFY_FRAMES;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready(); uint32_t allocations = allocation_calls;
    finish(); pool_drain();
    for (uint32_t failure = 1; failure <= allocations; ++failure) {
        configure(); request_next = 0; fail_at = failure;
        control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_VERIFY_FRAMES;
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); finish();
        assert(!context.video_owner && !allocations_live && !task_live);
    }
    puts("CRC padding golden, frozen READ/PAGE, owner/token/future ACK, unread STOP and allocation rollback PASS");
}

static void presentation_failures(void) {
    for (uint32_t variant = 0; variant < 7; ++variant) {
        configure(); request_next = 0; consume_enabled = 1; upload_limit = 3;
        context.framebuffer_shadow = 0;
        uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); await_ready();
        if (variant == 0) fail_shadow = 1;
        if (variant == 1) fail_gate = 1;
        if (variant == 2) fail_queue = 1;
        if (variant == 3) missing_fb = 1;
        if (variant == 6) fail_queue = 2;
        assert(!worker_enqueue_stream());
        uint32_t limit = tick() + 5000;
        if (variant == 4 || variant == 5) {
            while (!__atomic_load_n(&display_job, __ATOMIC_ACQUIRE)) {
                assert((int32_t)(limit - tick()) > 0); delay(1);
            }
            assert(context.video.output_pins == 1 && context.direct_pending && owned_shadow);
        }
        if (variant == 5) {
            control_command(p, VIDEO_CONTROL_STOP, 1);
            assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
            pool_drain();
            assert(state() == VIDEO_STOPPING && context.video_owner && context.video_presentation.pin);
            display_pump();
        }
        for (;;) {
            if (variant != 4) display_pump();
            pool_drain();
            uint32_t current = state();
            if (current == VIDEO_IDLE || current == VIDEO_QUARANTINED) break;
            assert((int32_t)(limit - tick()) > 0); delay(1);
        }
        if (variant == 4) {
            assert(state() == VIDEO_QUARANTINED && context.video_owner && allocations_live &&
                   context.direct_pending && owned_shadow && display_gate && context.video.output_pins == 1);
            /* Test-process disposal models external reset after proving retention;
             * production has no escape that clears an unknown submitted job. */
            clean_quarantine(); display_job = display_gate = 0;
            context.direct_pending = 0; context.direct_shadow = 0;
        } else {
            finish();
            assert(context.video_presentation.presented == (variant == 5 ? 1u : 0u));
            assert(!display_gate && !display_job && !context.video_presentation.pin);
            if (variant == 2 || variant == 3 || variant == 6)
                assert(context.video_presentation.failures == 1);
        }
        if (owned_shadow) { shadow_free(owned_shadow); context.framebuffer_shadow = 0; }
        assert(!overlay_calls);
    }
    puts("heap/gate/queue/copy refusal, STOP completion, claimed-queue failure and timeout retention PASS");
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
    if (ctx->framebuffer_shadow == owned_shadow && owned_shadow) {
        /* Upstream cleanup owns its display gate; the video paths must have
         * finished copying before this original synchronous free is reached. */
        assert(!ctx->direct_pending);
        free(owned_shadow); owned_shadow = 0; display_allocated = 0;
    }
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
#define CFW_IMAGE_ALLOC shadow_allocate
#define CFW_FRAMEBUFFER_BYTES VIDEO_PANEL_BYTES
#define IMAGE_W VIDEO_PANEL_WIDTH
#define IMAGE_H VIDEO_PANEL_HEIGHT
#define PANEL_W VIDEO_PANEL_WIDTH
#define PANEL_H VIDEO_PANEL_HEIGHT
#define PANEL_BYTES VIDEO_PANEL_BYTES
static void cfw_time_start(uint32_t *value) { *value = tick(); }
static uint32_t cfw_time_end(uint32_t *value) { return (tick() - *value) * 1000; }
static void cfw_draw_flags(uint8_t *fb, uint32_t w, uint32_t h) {
    assert(fb && w == 640 && h == 480); ++overlay_calls;
}
static int display_queue(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t w, uint32_t h) {
    assert(!a && !b && !c && !d && w == 640 && h == 480 && image_depth && display_gate);
    if (fail_queue == 2) {
        pthread_t copier;
        __atomic_store_n(&display_job, 1, __ATOMIC_RELEASE);
        assert(!pthread_create(&copier, 0, display_thread_entry, 0));
        assert(!pthread_join(copier, 0));
        return -1;
    }
    if (fail_queue) return -1;
    assert(!display_job); __atomic_store_n(&display_job, 1, __ATOMIC_RELEASE); return 0;
}
static void display_flush(void *descriptor) {
    assert(!image_depth);
    uint32_t *p = descriptor;
    assert(p[1] == VIDEO_PANEL_BYTES && owned_shadow);
    assert((context.direct_pending != 0) == (context.video_presentation.phase == VIDEO_DISPLAY_COPYING));
    ++flush_calls;
}
#define FW_DISPLAY_QUEUE display_queue
#define FW_DISPLAY_FB (missing_fb ? 0 : physical_frame)
#define FW_FLUSH display_flush
#define FW_DISPLAY_COPY() (++stock_copies)
#include "upstream_cleanup.h"

static void display_pump(void) {
    if (!__atomic_exchange_n(&display_job, 0, __ATOMIC_ACQ_REL)) return;
    assert(!image_depth && display_gate && context.direct_pending);
    display_copy_hook();
    if (!missing_fb) {
        int native = context.video_control.options & VIDEO_PRESENT_NATIVE;
        for (uint32_t y = 0; y < VIDEO_PANEL_HEIGHT; ++y)
            for (uint32_t x = 0; x < VIDEO_PANEL_STRIDE; ++x)
                assert(physical_frame[y * VIDEO_PANEL_STRIDE + x] ==
                    ((native ? y >= 144 && y < 336 && x >= 80 && x < 240 : y >= 48 && y < 432) ? 0x88 : 0));
        ++copied_frames;
    }
    display_signal();
}
static void *display_thread_entry(void *unused) {
    (void)unused; display_pump(); return 0;
}

static void original_display(void) {
    configure(); context.framebuffer_shadow = shadow_allocate(VIDEO_PANEL_BYTES);
    memset(owned_shadow, 0xff, VIDEO_PANEL_BYTES);
    context.direct_pending = 1; context.direct_shadow = owned_shadow; display_gate = 1;
    display_copy_hook(); display_signal();
    assert(!context.direct_pending && !context.direct_shadow && context.direct_active &&
           overlay_calls == 1 && flush_calls == 1 && !context.video_presentation.presented);
    for (uint32_t i = 0; i < VIDEO_PANEL_BYTES; ++i) assert(physical_frame[i] == 0xff);
    display_copy_hook(); assert(!stock_copies && context.direct_active);
    context.direct_lease_deadline = 0;
    display_copy_hook(); assert(stock_copies == 1 && !context.direct_active);
    shadow_free(owned_shadow); context.framebuffer_shadow = 0;
    puts("ordinary image overlay, physical copy, lease preservation and stock fallback PASS");
}

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
    else if (!strcmp(argv[1], "control-paging")) control_paging();
    else if (!strcmp(argv[1], "credits")) credits();
    else if (!strcmp(argv[1], "diagnostics")) diagnostics();
    else if (!strcmp(argv[1], "control-preparation")) control_preparation();
    else if (!strcmp(argv[1], "control-failures")) control_failures();
    else if (!strcmp(argv[1], "nal-input")) nal_input();
    else if (!strcmp(argv[1], "queue-extension")) queue_extension();
    else if (!strcmp(argv[1], "recovery")) recovery();
    else if (!strcmp(argv[1], "consumer")) consumer();
    else if (!strcmp(argv[1], "evidence")) evidence();
    else if (!strcmp(argv[1], "evidence-replays")) evidence_replays();
    else if (!strcmp(argv[1], "presentation-failures")) presentation_failures();
    else if (!strcmp(argv[1], "original-display")) original_display();
    else if (!strcmp(argv[1], "inactivity")) inactivity();
    else if (!strcmp(argv[1], "cleanup")) cleanup();
    else if (!strcmp(argv[1], "lease-notifications")) lease_notifications();
    else assert(0);
    assert(!allocations_live && !task_live && !image_depth);
    printf("worker %s PASS\n", argv[1]);
}
