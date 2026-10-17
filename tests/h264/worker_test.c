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
static pthread_t presenter_thread;
static uint32_t presenter_live;
static _Thread_local uint32_t picture_thread;
static _Thread_local uint32_t image_depth, worker_thread, service_context;
static uint32_t task_live, creates, terminates, allocation_calls, allocations_live;
static uint32_t fail_at, fail_task, bad_tcb, fail_terminate, fail_view;
static uint32_t cancel_new, expire_new, fail_publication, defer_reclaimed;
static uint32_t feed, fed, stream_ok, corrupt_guard, suppress_park;
static uint32_t consume_enabled, decoded_calls, consumed_frames, decode_paused, refuse_extension;
static uint32_t upload_limit;
static uint32_t queued_upload, dither_fixture, timed_upload;
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
static fake_event events[8];
static video_heap_view views[3] = {{382756, 376832}, {285680, 278528}, {133924, 131072}};
static uint32_t cached_used, cached_peak;
static video_pool_item pool_jobs[8];
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t pool_count, fail_pool, pool_calls;
static uint8_t control_reply[CFW_VIDEO_REPLY_MAX_SIZE];
static uint32_t fail_reply, reply_live;
void *cfw_message_video_reply_allocate(uint32_t size) {
    (void)size; assert(0 && "Reply pages must not allocate"); return 0;
}
void cfw_message_video_reply_free(void *reply) {
    (void)reply; assert(0 && "Reply pages must not free temporary heap storage");
}
static uint32_t control_reply_size;
static void (*heap_sample_hook)(void);
static const uint32_t shadow_present = 1;
typedef struct { uint32_t direct_submitted, direct_failed; } cfw_rectlist;
static void present_shadow(uint32_t, uint32_t, cfw_rectlist *);
static uint8_t *cfw_shadow_buffer(void);
void display_copy_hook(void);
static void display_pump(void);
static void pool_drain(void);
static void *display_thread_entry(void *);
static uint32_t display_gate, display_job, fail_gate, fail_queue, missing_fb;
static uint32_t picture_relock_refusals;
static uint32_t playback_pause_fixture, playback_pause_count;
/* Phone pacing: 1 = coordinator steps through refusal/far/delay/catch-up, 2 = paired LEFT ignores. */
static uint32_t pace_fixture, pace_state, pace_ordinal, pace_due, pace_from, pace_released, pace_holds;
static void exercise_pause(void);
static uint32_t handoff_record[VIDEO_HANDOFF_RECORD_WORDS], handoff_queues, handoff_hold, fail_handoff;
static uint32_t power_awake = 1, power_starts;
static uint32_t display_allocated, fail_shadow, copied_frames, overlay_calls, flush_calls, stock_copies;
static uint8_t physical_frame[VIDEO_PANEL_BYTES];
static uint8_t *owned_shadow;
static int display_take(void) {
    assert((picture_thread ? !image_depth : image_depth) && !service_context);
    if (fail_gate) return 0;
    assert(!display_gate); display_gate = 1; return 1;
}
static void display_signal(void) { assert(display_gate); display_gate = 0; }
static int handoff_queue(const uint32_t *);
static void *shadow_allocate(uint32_t bytes) {
    assert(!image_depth && !service_context && bytes == VIDEO_PANEL_BYTES && !owned_shadow);
    if (queued_upload) assert(worker_thread); /* Shadow admission precedes concurrent decode/copy. */
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
    if (picture_thread && !timeout && picture_relock_refusals) {
        --picture_relock_refusals; return -1;
    }
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
static uint32_t thread_id(void) { return worker_thread ? 2 : picture_thread ? 4 : 3; }
static uint32_t controller_used = 512, controller_samples;
static int stack_sample(uint32_t *task, uint32_t *used, uint32_t *bytes) {
    assert(!service_context && !image_depth && !worker_thread);
    ++controller_samples;
    *task = 3; *used = controller_used; *bytes = 4096; return 1;
}
#define VIDEO_PLATFORM_STACK_SAMPLE stack_sample
static uint32_t diagnostic_display_state = 1;
static uint32_t diagnostic_display(void) { return diagnostic_display_state; }
#define VIDEO_DIAG_DISPLAY_STATE diagnostic_display
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
    memcpy(control_reply, p, n); control_reply_size = n; return fail_reply ? -1 : 0;
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
    for (uint32_t i = 0; i < 8; ++i) {
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
/* System control registers exist only on the core: CCR reads with both caches on. */
#define VIDEO_CORE_REGISTER(address) ((address) == 0xE000ED14u ? 0x30000u : 0u)
#define VIDEO_CLOCK_INIT test_clock_init
#define VIDEO_CLOCK_CALIBRATE test_calibrate
#define VIDEO_FRAME_CRC test_crc
static int power_ready(void) { return power_awake; }
static int power_start(void) { assert(!image_depth && !service_context); ++power_starts; return 1; }
static int power_send(const uint8_t *record) {
    assert(!image_depth && !service_context);
    if (record[3] == VIDEO_POWER_ACK) return 1;
    uint8_t reply[VIDEO_POWER_BYTES]; memcpy(reply, record, sizeof(reply));
    reply[1] ^= CFW_MESSAGE_BOTH; reply[3] = VIDEO_POWER_ACK; reply[4] = VIDEO_POWER_OK;
    uint32_t status = video_power_received(reply, sizeof(reply));
    assert(status == 0 || status == 6); return status == 0;
}
static uint32_t power_refresh(uint32_t app, uint32_t event, const void *data, uint32_t size) {
    (void)app; (void)event; (void)data; (void)size; return 0;
}
#define VIDEO_POWER_NATIVE 1
#define VIDEO_POWER_PLATFORM_READY power_ready
#define VIDEO_POWER_PLATFORM_START power_start
#define VIDEO_POWER_PLATFORM_SEND power_send
#define VIDEO_POWER_HERE() CFW_MESSAGE_LEFT
#define VIDEO_POWER_PLATFORM_REFRESH power_refresh
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
#include "../../patches/video/profile.c"
static uint32_t radio_lost, radio_active, radio_acquires, radio_releases, radio_off, deny_radio;
int video_radio_acquire(uint32_t generation, int off) {
    assert(!image_depth && !service_context && context.video_control.start_guard == generation);
    ++radio_acquires; radio_off = off;
    if (deny_radio) return 0;
    radio_active = generation; return 1;
}
void video_radio_release(uint32_t generation) {
    assert(!image_depth && !service_context);
    if (generation && radio_active == generation) { radio_active = 0; ++radio_releases; }
}
int video_radio_generation_lost(uint32_t generation) { return radio_lost == generation; }
void video_radio_step(void) { assert(!image_depth && !service_context); }
int video_radio_maintenance(void) { return 0; }
#include "../../patches/video/power.c"
#include "../../patches/video/controller.c"
#include "../../patches/video/diagnostics.c"
static uint32_t radio_samples;
static int worker_radio_snapshot(uint8_t *out) {
    ++radio_samples;
    assert(!image_depth); memset(out, 0, VIDEO_RADIO_REPORT_BYTES);
    out[0] = 1; out[1] = 7; out[3] = VIDEO_RADIO_DIAGNOSTICS;
    out[8] = VIDEO_RADIO_REPORT_VERSION; out[9] = VIDEO_RADIO_RECORD_BYTES;
    out[10] = VIDEO_RADIO_LINKS; return 1;
}
#define VIDEO_RADIO_SNAPSHOT worker_radio_snapshot
static unsigned refusal_samples;
static int worker_radio_refusal_snapshot(uint8_t *out) {
    ++refusal_samples;
    assert(!image_depth); memset(out, 0, VIDEO_RADIO_REPORT_BYTES);
    out[0] = 1; out[1] = 7; out[3] = VIDEO_RADIO_REFUSAL_DIAGNOSTICS;
    out[8] = 1; out[9] = VIDEO_RADIO_TRACE_BYTES; out[10] = 2;
    return 1;
}
#define VIDEO_RADIO_REFUSAL_SNAPSHOT worker_radio_refusal_snapshot
#include "../../patches/video/control.c"
static uint32_t mock_clock_reg(uint32_t address) { return address == 0xe0001000U ? 1 : address; }
#define VIDEO_CLOCK_READ32 mock_clock_reg
#define VIDEO_CLOCK_CACHED() 2
#define VIDEO_CLOCK_CYCLES (tick() * 250000u)
#include "../../patches/video/telemetry.c"
#include "../../patches/video/rate.c"
#include "../../patches/video/timeline.c"
#include "../../patches/video/decoded_queue.c"
#include "../../patches/video/pacing.c"
#include "../../patches/video/sync.c"
static uint32_t paired_fixture, partner_anchor, partner_native;
static video_sync partner;
static int test_sync_send(const uint8_t *body) {
    assert(!image_depth && !service_context && body[0] == VIDEO_SYNC_BRIDGE);
    if (!paired_fixture) return 1;
    uint32_t source = body[1], operation = body[3], stream = video_read32(body + 4);
    uint32_t ordinal = video_read32(body + 8), pts = video_read32(body + 12);
    uint8_t reply[VIDEO_SYNC_BYTES]; memcpy(reply, body, sizeof(reply));
    reply[1] ^= CFW_MESSAGE_BOTH;
    if (operation == VIDEO_SYNC_PAUSE || operation == VIDEO_SYNC_RESUME || operation == VIDEO_SYNC_MENU) {
        assert(source == CFW_MESSAGE_RIGHT && !body[20]);
        reply[20] = 1;
        assert(!video_sync_received(reply, sizeof(reply)));
        return 1;
    }
    if (operation == VIDEO_SYNC_READY) {
        assert(video_sync_ready(&partner, ordinal, pts, body[20], body[21]));
        if (source == CFW_MESSAGE_RIGHT) {
            reply[3] = VIDEO_SYNC_READY;
            assert(!video_sync_received(reply, sizeof(reply)));
        } else {
            assert(video_sync_receive(&partner, source, stream, operation, ordinal, pts, body[20], body[21]));
            if (video_sync_trigger(&partner)) {
                if (!partner_anchor) partner_anchor = tick();
                uint32_t due = partner_anchor + (ordinal - 1) * 1000 / 15;
                while ((int32_t)(due - tick()) > 0) delay(1);
                reply[3] = VIDEO_SYNC_TRIGGER; reply[20] = 0; reply[21] = partner_native;
                assert(video_sync_complete(&partner, ordinal));
                assert(!video_sync_received(reply, sizeof(reply)));
            }
        }
    } else if (operation == VIDEO_SYNC_TRIGGER) {
        assert(video_sync_receive(&partner, source, stream, operation, ordinal, pts, 0, 0));
        assert(video_sync_complete(&partner, ordinal));
        reply[3] = VIDEO_SYNC_DONE;
        video_write32(reply + 16, 0); reply[20] = reply[21] = 0;
        assert(!video_sync_received(reply, sizeof(reply)));
    } else if (operation == VIDEO_SYNC_DONE) {
        assert(video_sync_receive(&partner, source, stream, operation, ordinal, pts, 0, 0));
    }
    return 1;
}
#define VIDEO_SYNC_SEND test_sync_send
#include "../../patches/video/sync_wire.c"
#include "../../patches/video/shown.c"
#include "../../patches/video/shown_wire.c"
#include "../../patches/video/menu.c"
static int test_menu_text(uint8_t *shadow, const uint8_t *text, uint32_t bytes, cfw_rectlist *rectangles) {
    assert(picture_thread && image_depth && display_gate && bytes == 6u + text[5] && rectangles);
    uint32_t x = text[0] | (uint32_t)text[1] << 8, y = text[2] | (uint32_t)text[3] << 8;
    assert(x < VIDEO_PANEL_WIDTH && y < VIDEO_MENU_HEIGHT && text[5] < 34);
    assert(text[4] == VIDEO_MENU_TEXT_LEVEL);
    shadow[y * VIDEO_PANEL_STRIDE + x / 2] = 0xff;
    return 0;
}
#define VIDEO_MENU_TEXT(shadow, text, bytes, rectangles) test_menu_text(shadow, text, bytes, rectangles)
#include "../../patches/video/menu_draw.c"
#include "../../patches/video/presenter.c"
static uint32_t test_crc(const g2_h264_frame_info *frame) {
    __atomic_fetch_add(&crc_calls, 1, __ATOMIC_RELAXED); return video_frame_crc(frame);
}

int worker_test_stream(void *);
void worker_finish_call(void) {
    video_owner *owner = video_current_owner(); assert(owner && worker_thread);
    video_storage_finish_call(&owner->storage);
}
struct Start { void (*fn)(void *); void *arg; uint32_t picture; };
static void *entry(void *p) {
    struct Start start = *(struct Start *)p; free(p);
    picture_thread = start.picture; worker_thread = !start.picture;
    start.fn(start.arg); assert(0); return 0;
}
static uint32_t new_thread(void (*fn)(void *), void *arg, const video_thread_attr *attr) {
    int picture = attr->stack_size == VIDEO_PRESENTER_STACK_BYTES;
    assert(!image_depth && (picture || attr->stack_size == 16384) && attr->cb_size == 112 &&
           attr->stack_mem && attr->cb_mem && attr->priority == 8 &&
           (picture ? !presenter_live : !task_live));
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
    *start = (struct Start){fn, arg, picture};
    if (picture) {
        presenter_live = 1;
        assert(!pthread_create(&presenter_thread, 0, entry, start)); return 4;
    }
    task_live = 1; assert(!pthread_create(&worker, 0, entry, start)); return 2;
}
static int delay(uint32_t ms) {
    if (!worker_thread && !handoff_hold &&
        __atomic_load_n(&display_job, __ATOMIC_ACQUIRE) == 2) display_pump();
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
    for (uint32_t i = 0; i < 8; ++i) {
        if (events[i].control) continue;
        events[i].control = attr->cb_mem;
        assert(!pthread_mutex_init(&events[i].mutex, 0));
        assert(!pthread_cond_init(&events[i].condition, 0));
        return i + 1;
    }
    assert(0); return 0;
}
static uint32_t event_set(uint32_t id, uint32_t bits) {
    assert(!service_context && id && id <= 8 && events[id - 1].control && bits && !(bits & 0xff000000u));
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
    assert(!image_depth && id && id <= 8 && events[id - 1].control && !options);
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
        /* The controller may briefly hold the mutex while reading READY. */
        assert(!take(1, 1000)); video_worker_request_stop_locked(); give(1);
    }
    if (worker_thread && inject_renew && context.video.ready) {
        inject_renew = 0; renewed = 1;
        assert(!take(1, 1000));
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
    assert(id <= 8 && events[id - 1].control);
    return !events[id - 1].waiters;
}
static int terminate(uint32_t id) {
    if (id == 4) {
        assert(!image_depth && !worker_thread && !picture_thread && presenter_live);
        assert(context.video_owner && ((video_owner *)context.video_owner)->presenter->parked);
        if (fail_terminate) return -1;
        assert(!pthread_cancel(presenter_thread)); assert(!pthread_join(presenter_thread, 0));
        presenter_live = 0; ++terminates; return 0;
    }
    assert(!image_depth && !worker_thread && id == 2 && task_live &&
           __atomic_load_n(&context.video.parked, __ATOMIC_ACQUIRE));
    ++terminates; if (fail_terminate) return -1;
    assert(!pthread_cancel(worker)); assert(!pthread_join(worker, 0)); task_live = 0; return 0;
}
static void configure(void) {
    assert(!display_job && !display_gate && !owned_shadow);
    fail_gate = fail_queue = missing_fb = fail_shadow = copied_frames = overlay_calls = flush_calls = stock_copies = 0;
    picture_relock_refusals = 0;
    handoff_queues = handoff_hold = fail_handoff = 0;
    assert(!radio_active);
    power_awake = 1; power_starts = 0; radio_lost = 0;
    radio_acquires = radio_releases = radio_off = deny_radio = 0;
    assert(!allocations_live && !task_live && !presenter_live);
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
    upload_limit = queued_upload = 0;
    cycle_reads = clock_inits = calibrations = crc_calls = anomalous_clock = 0;
    forced_cycles = force_cycles = 0;
    evidence_drain = evidence_checked = evidence_was_full = 0; evidence_next = 1;
    for (uint32_t i = 0; i < 8; ++i) assert(!events[i].control);
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
    pool_drain();
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
    pool_drain();
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
void worker_stream_geometry(uint32_t width, uint32_t height);
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
    uint8_t page[195] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_PAGE, VIDEO_PROTOCOL_VERSION};
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
    /* Padded PAGE proves room without changing a previously frozen stride. */
    for (uint8_t lens = 1; lens <= 2; ++lens) {
        for (uint32_t large_mtu = 0; large_mtu <= 2; ++large_mtu) {
            configure(); request_next = 0;
            uint8_t page_capacity = large_mtu ? 200 : 9;
            control_command(p, VIDEO_CONTROL_CAPABILITIES, 0);
            assert(control_variable_pages(p, 8, snapshot, sizeof(snapshot), lens,
                large_mtu == 2 ? 200 : large_mtu ? 13 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            assert(snapshot[3] == VIDEO_IDLE);
            control_command(p, VIDEO_CONTROL_START, 1);
            assert(control_variable_pages(p, 24, snapshot, sizeof(snapshot), lens,
                large_mtu == 2 ? 200 : large_mtu ? 29 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            assert(snapshot[3] == VIDEO_STARTING && pool_calls == 1 && !allocations_live);
            assert(control_variable_pages(p, 24, retry, sizeof(retry), lens,
                large_mtu == 2 ? 200 : large_mtu ? 29 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            assert(!memcmp(snapshot, retry, sizeof(snapshot)) && pool_calls == 1);
            control_command(p, VIDEO_CONTROL_FRAME_READ, 1);
            control_variable_pages(p, 20, snapshot, VIDEO_FRAME_RESULT_BYTES, lens,
                large_mtu == 2 ? 200 : large_mtu ? 25 : 9, page_capacity);
            control_command(p, VIDEO_CONTROL_STOP, 1);
            assert(control_variable_pages(p, 12, snapshot, sizeof(snapshot), lens,
                large_mtu == 2 ? 200 : large_mtu ? 17 : 9, page_capacity) == VIDEO_CONTROL_ACCEPTED);
            pool_drain(); finish();
        }
    }
    configure(); request_next = 0;
    control_command(p, VIDEO_CONTROL_STATUS, 0);
    cfw_message_route route = {1, 1, 1, 200, 0};
    fail_reply = 1;
    assert(video_control_received(p, 8, &route) && context.video_control.request_high[0]);
    assert(context.video_telemetry.refusals[0].rows[0][14] == VIDEO_REFUSAL_SEND);
    fail_reply = 0;
    assert(!video_control_received(p, 8, &route) && control_reply_size == 200 && control_reply[7] == 1);
    uint8_t page[195] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_PAGE, VIDEO_PROTOCOL_VERSION};
    memcpy(page + 4, p + 4, 4);
    assert(!video_control_received(page, sizeof(page), &route));
    route.reply_capacity = 199;
    assert(video_control_received(page, sizeof(page), &route));
    route.reply_capacity = 200; page[8] = 1;
    assert(video_control_received(page, sizeof(page), &route));
    page[8] = 0; page[4]++;
    assert(video_control_received(page, sizeof(page), &route) && !reply_live);
    puts("heap-free frozen pages, padded fetches, send failure and both lenses PASS");
}
static void receive_evidence(void) {
    configure(); request_next = 0;
    uint8_t command[VIDEO_START_BYTES], out[128], page[9] = {31, 5, 1};
    cfw_message_route route = {1, 1, 1, 200, 0};
    control_command(command, VIDEO_CONTROL_STATUS, 0);
    assert(!video_control_received(command, 8, &route));
    memcpy(page + 4, command + 4, 4);
    page[8] = 1; assert(video_control_received(page, 9, &route));
    page[8] = 0; route.reply_capacity = 199; assert(video_control_received(page, 9, &route));
    route.reply_capacity = 200; page[4]++; assert(video_control_received(page, 9, &route));
    context.video_telemetry.reply_busy[0] = 1;
    assert(video_control_received(command, 8, &route));
    context.video_telemetry.reply_busy[0] = 0;
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_REFUSAL_DIAGNOSTICS);
    assert(!control_response(command, 12, out, sizeof(out)));
    assert(out[3] == 4 && out[4] == 1 && out[5] == 1 && out[7] == 4 && out[16] == 24);
    uint8_t expected[] = {VIDEO_REFUSAL_PENDING, VIDEO_REFUSAL_MISSING, VIDEO_REFUSAL_CAPACITY, VIDEO_REFUSAL_PAGE};
    for (uint32_t i = 0; i < 4; ++i) assert(out[32 + i * 24 + 14] == expected[i]);
    assert(!allocation_calls && !allocations_live && !context.video_telemetry.reply_busy[0]);
    video_refusal_ring *ring = &context.video_telemetry.refusals[1];
    ring->busy = 1; route.origin = 2;
    assert(video_refused(&context, page, 9, &route, VIDEO_REFUSAL_LOCK, 0, 0, 0));
    assert(ring->dropped == 1); ring->busy = 0;
    assert(video_refused(&context, page, 9, &route, VIDEO_REFUSAL_LOCK, 0, 0, 0));
    assert(!video_refusal_snapshot(&context, 2, 1, out) && out[7] == 1 && video_read32(out + 12) == 1);

    context.video_control.stream = 42; context.video_control.control_generation = 7;
    context.video_control.owner_here = 1;
    uint32_t inits = clock_inits;
    video_clock_start_locked(&context);
    video_clock_state *s = &context.video_telemetry.clock;
    assert(s->rows[0][7] & VIDEO_CLOCK_VALID);
    s->anchor.tick -= 300; s->anchor.after -= 300; s->anchor.cycles -= 75000000u;
    video_clock_step_locked(&context);
    assert((s->rows[1][7] & (VIDEO_CLOCK_VALID | VIDEO_CLOCK_LONG | VIDEO_CLOCK_COUNTER)) == 7);
    video_clock_stop_locked(&context, 6); assert(!s->rows[2][7]);
    video_clock_stop_locked(&context, 7); assert(s->rows[2][7] & VIDEO_CLOCK_LONG);
    assert(video_read32(s->rows[1] + 40) == 0x40021000 && video_read32(s->rows[1] + 76) == 2);
    assert(clock_inits == inits); /* Sampling never enables or changes a clock. */
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_CLOCK_DIAGNOSTICS); command[9] = 2;
    assert(!control_response(command, 12, out, sizeof(out)) && out[3] == 5 && out[4] == 2 && video_read32(out + 8) == 42);
    video_clock_pair wrapped = {VIDEO_TICK - 300, (uint32_t)VIDEO_CLOCK_CYCLES - 75000000u, VIDEO_TICK - 300, 0};
    video_clock_capture(&context, 1, wrapped); assert(s->rows[1][7] & VIDEO_CLOCK_CHANGED);
    wrapped.tick = wrapped.after = VIDEO_TICK - 1001;
    video_clock_capture(&context, 1, wrapped); assert(!(s->rows[1][7] & VIDEO_CLOCK_LONG));
    puts("per-origin refusal evidence, contention drops, read-only bracketed clocks and heap-free replies PASS");
}
static void rate_contract(void) {
    configure(); request_next = 0;
    uint8_t command[24], out[144];
    context.video_control.owner_origin = 1;
    video_rate_start(&context.video_rate, 42, 7, 3, 67);
    video_rate_copy(&context.video_rate, 100, 1, 0);
    video_rate_present(&context.video_rate, 1100, 0, 0);
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_RATE_DIAGNOSTICS);
    video_write32(command + 12, 42); video_write32(command + 16, 1); video_write32(command + 20, 1);
    assert(!control_response(command, 24, out, 144));
    assert(out[3] == 6 && out[4] == 1 && out[6] == 1 && out[28] == 1 && out[29] == 56);
    assert(video_read32(out + 32) == 1 && video_read32(out + 40) == 1 && !allocation_calls);
    command[9] = 1; video_write32(command + 4, ++request_next);
    assert(!control_response(command, 24, out, 128) && out[29] == 32);
    video_write32(command + 12, 41); video_write32(command + 4, ++request_next);
    assert(control_response(command, 24, out, 128) == VIDEO_CONTROL_STALE);
    assert(!context.video_telemetry.reply_busy[0] && !allocations_live);
    puts("bounded owner-scoped window diagnostics and stale stream refusal PASS");
}
static void controls(void) {
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    configure(); request_next = 0;
    control_command(p, VIDEO_CONTROL_CAPABILITIES, 0);
    assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED);
    assert(snapshot[1] == 7 && snapshot[3] == VIDEO_IDLE && !snapshot[50] &&
           !allocations_live && !creates && !pool_calls);
    context.video_power.acquire_step = VIDEO_POWER_STEP_LOCAL_APP;
    control_command(p, VIDEO_CONTROL_STATUS, 0);
    assert(control_snapshot(p, 8, snapshot) == VIDEO_CONTROL_ACCEPTED &&
           snapshot[69] >> VIDEO_POWER_STEP_SHIFT == VIDEO_POWER_STEP_LOCAL_APP &&
           !(snapshot[69] & VIDEO_POWER_HEADER_MASK));
    context.video_power.acquire_step = VIDEO_POWER_STEP_NONE;
    control_command(p, VIDEO_CONTROL_START, 1);
    context.texture_cache = (void *)(uintptr_t)1;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_LEASE && !pool_calls);
    context.texture_cache = 0;
    control_command(p, VIDEO_CONTROL_START, 1);
    p[18] = 4;
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
        assert(video_control_received(request, 8, &control_route));
    }
}
static void sleep_during_heap_sample(void) { diagnostic_display_state = 0; }
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
           frozen[3] == (7 | 64 | 128) && video_read32(frozen + 16) == views[0].free_bytes &&
           !video_read32(frozen + 124));
    uint8_t state_sample[VIDEO_DIAGNOSTICS_BYTES];
    diagnostic_display_state = 0;
    assert(video_diagnostics_snapshot(state_sample) && (state_sample[3] & 192) == 64);
    diagnostic_display_state = 2;
    assert(video_diagnostics_snapshot(state_sample) && !(state_sample[3] & 192));
    diagnostic_display_state = 1; heap_sample_hook = sleep_during_heap_sample;
    assert(video_diagnostics_snapshot(state_sample) && !(state_sample[3] & 192));
    heap_sample_hook = 0; diagnostic_display_state = 1;
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
    assert(!video_control_received(request, 8, &control_route));
    assert(!allocation_calls && !creates && !image_depth);
    heap_sample_hook = 0; radio_samples = 0;
    control_command(request, VIDEO_CONTROL_DIAGNOSTICS, 0); request[8] = VIDEO_RADIO_DIAGNOSTICS;
    assert(!video_control_received(request, 12, &control_route));
    memcpy(page + 4, request + 4, 4);
    for (uint32_t i = 0; i < sizeof(frozen); ++i) {
        page[8] = i; assert(!video_control_received(page, sizeof(page), &control_route));
        frozen[i] = control_reply[8];
    }
    assert(frozen[3] == VIDEO_RADIO_DIAGNOSTICS && frozen[8] == VIDEO_RADIO_REPORT_VERSION &&
           frozen[9] == VIDEO_RADIO_RECORD_BYTES && frozen[10] == VIDEO_RADIO_LINKS);
    assert(!video_control_received(request, 12, &control_route) && radio_samples == 1);
    request[8] = 2; assert(video_control_received(request, 12, &control_route) == -1);
    control_command(request, VIDEO_CONTROL_DIAGNOSTICS, 0); request[8] = VIDEO_RADIO_REFUSAL_DIAGNOSTICS;
    refusal_samples = 0;
    assert(!control_response(request, 12, frozen, sizeof(frozen)));
    assert(frozen[3] == 2 && frozen[9] == 32 && frozen[10] == 2 && refusal_samples == 1);
    assert(!video_control_received(request, 12, &control_route) && refusal_samples == 1);
    control_command(request, VIDEO_CONTROL_DIAGNOSTICS, 0); request[8] = 3;
    assert(video_control_received(request, 12, &control_route) == -1);
    assert(radio_samples == 1 && !allocation_calls && !creates && !image_depth);
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
    if (playback_pause_fixture && sequence == 12) exercise_pause();
    uint8_t record[VIDEO_SLOT_BYTES + 8] = {VIDEO_MESSAGE_ID, VIDEO_CONTROL_NAL};
    video_write32(record + 2, 1); video_write32(record + 6, sequence);
    uint32_t header = VIDEO_NAL_HEADER_BYTES;
    if (queued_upload && timed_upload && sequence >= 2) {
        /* The terminating slice carries its own frame time instead of a
         * separate offer; a full timeline refuses it and the loop retries. */
        record[1] = VIDEO_CONTROL_NAL_TIMED; header = VIDEO_NAL_TIMED_HEADER_BYTES;
        video_write32(record + 10, sequence - 1); video_write32(record + 14, ((sequence - 2) * 1000 + 7) / 15);
        assert(header + bytes <= VIDEO_RECORD_LIMIT);
    }
    memcpy(record + header, nal, bytes);
    uint32_t limit = tick() + 3000;
    if (queued_upload && !timed_upload && sequence >= 2) {
        /* This fixture emits exactly one terminating slice per picture;
         * production clients obtain these identities from decoded references. */
        video_frame_time media = {sequence - 1, sequence, ((sequence - 2) * 1000 + 7) / 15};
        for (;;) {
            assert(!take(1, 1000));
            int offered = video_timeline_offer_preroll(&context.video_control.timeline, 1,
                context.video_control.control_generation, &media,
                (context.video_control.options >> VIDEO_PREROLL_SHIFT) & 255u);
            if (offered) video_worker_wake_locked(context.video.token, VIDEO_WAKE_INPUT);
            give(1);
            if (offered) break;
            display_pump(); pool_drain();
            if ((int32_t)(limit - tick()) <= 0) {
                assert(!take(1, 1000));
                video_owner *active = context.video_owner;
                video_presenter *picture = active ? active->presenter : 0;
                fprintf(stderr, "metadata deadline sequence=%u state=%u fault=%u error=%u shown=%u ready=%u peer=%u flight=%u completed=%u peer_done=%u\n",
                    sequence, context.video.state, context.video.fault, context.video_control.error,
                    picture ? picture->shown : 0, picture ? picture->sync.local_ordinal : 0,
                    picture ? picture->sync.peer_ordinal : 0, picture ? picture->sync.inflight : 0,
                    picture ? picture->sync.completed : 0, picture ? picture->sync.peer_completed : 0);
                give(1);
            }
            assert((int32_t)(limit - tick()) > 0); delay(1);
        }
    }
    for (;;) {
        display_pump(); pool_drain(); drain_frame_results();
        if (!video_control_received(record, header + bytes, &control_route)) break;
        assert((int32_t)(limit - tick()) > 0 && state() == VIDEO_READY); delay(1);
    }
    /* Uploaded input is no longer borrowed. The same sequence can be retried
     * exactly even if consumption happened before the transport reply. */
    assert(!video_control_received(record, header + bytes, &control_route));
    memset(record + header, 0xee, bytes);
    return 1;
}

static void menu_fixture_command(uint32_t operation, uint32_t value, uint32_t expected_view) {
    video_owner *owner = context.video_owner;
    uint8_t command[24] = {0}, report[VIDEO_MENU_STATE_BYTES] = {0};
    command[1] = operation; video_write32(command + 8, 1);
    video_write32(command + 12, owner->token); video_write32(command + 16, owner->token);
    video_write32(command + 20, value);
    assert(!take(1, 1000));
    assert(!video_menu_control_locked(command, 24, 1, report)); give(1);
    uint32_t limit = tick() + 3000;
    for (;;) {
        display_pump(); pool_drain();
        assert(!take(1, 1000));
        assert(context.video_owner == owner && !context.video.fault);
        video_presenter *p = owner->presenter;
        int done = !p->menu_command && !p->menu_dirty && p->menu_actual == expected_view &&
            p->control_ack == p->control_serial;
        give(1);
        if (done) return;
        delay(1); assert((int32_t)(limit - tick()) > 0);
    }
}

static void exercise_pause(void) {
    video_owner *owner = context.video_owner;
    uint32_t here = context.video_control.owner_here, limit = tick() + 3000;
    uint8_t command[20] = {0}, playback[VIDEO_PLAYBACK_STATE_BYTES] = {0};
    command[1] = VIDEO_CONTROL_PAUSE;
    video_write32(command + 8, 1);
    video_write32(command + (here == CFW_MESSAGE_LEFT ? 12 : 16), owner->token);
    assert(!take(1, 1000));
    assert(video_playback_control_locked(command, 19, 1, playback) == VIDEO_CONTROL_FORMAT);
    assert(video_playback_control_locked(command, 20, 2, playback) == VIDEO_CONTROL_STALE);
    assert(!video_playback_control_locked(command, 20, 1, playback)); give(1);
    for (;;) {
        display_pump(); pool_drain();
        assert(!take(1, 1000));
        int stopped = owner->presenter->paused && owner->presenter->control_ack == owner->presenter->control_serial;
        give(1);
        if (stopped) break;
        delay(1); assert((int32_t)(limit - tick()) > 0);
    }
    uint32_t frozen = copied_frames;
    assert(frozen > 0 && frozen < 32);
    if (playback_pause_fixture == 2) {
        assert(frozen == 1 && owner->presenter->media_shown == 8 && owner->presenter->done_pts == 467);
        assert(owner->presenter->queue.released == 8 && consumed_frames > frozen);
    }
    for (uint32_t i = 0; i < 150; ++i) { display_pump(); pool_drain(); delay(1); }
    assert(copied_frames == frozen && !context.video.fault && radio_active);
    if (context.video_control.owner_here != context.video_control.owner_origin) assert(radio_off);
    if (playback_pause_fixture == 3) {
        assert(physical_frame[434 * VIDEO_PANEL_STRIDE + 48] == 0xff);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_NEXT, 3);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_PREVIOUS, 1);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_NEXT, 3);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_APPLY, 3);
        assert(owner->presenter->menu.seek_serial == 1 && !owner->presenter->menu.seek_target);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_NEXT, 5);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_APPLY, 5);
        assert(owner->presenter->menu.seek_serial == 2 && owner->presenter->menu.seek_target == 2133);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_CLOSE, 0);
        for (uint32_t i = 432 * VIDEO_PANEL_STRIDE; i < VIDEO_PANEL_BYTES; ++i) assert(!physical_frame[i]);
        menu_fixture_command(VIDEO_CONTROL_MENU, VIDEO_MENU_OPEN, 1);
        menu_fixture_command(VIDEO_CONTROL_SCALE, VIDEO_SCALE_NATIVE, 1);
        assert(owner->presenter->scale_requested && !(context.video_control.options & VIDEO_PRESENT_NATIVE));
        assert(copied_frames == frozen && owner->presenter->shown_rows.high == frozen);
    }
    assert(!take(1, 1000));
    command[1] = VIDEO_CONTROL_PLAYBACK;
    assert(!video_playback_control_locked(command, 20, 1, playback));
    assert(video_read32(playback + 12) == 1 && video_read32(playback + 24) == owner->presenter->done_pts);
    command[1] = VIDEO_CONTROL_RESUME;
    assert(!video_playback_control_locked(command, 20, 1, playback)); give(1);
    ++playback_pause_count;
}

static int pace_command(video_owner *owner, uint32_t bytes, uint32_t ordinal, uint32_t due, uint32_t token_xor) {
    uint8_t command[VIDEO_PACE_BYTES] = {0}, playback[VIDEO_PLAYBACK_STATE_BYTES] = {0};
    command[1] = VIDEO_CONTROL_PACE; video_write32(command + 8, 1);
    video_write32(command + 12, owner->token ^ token_xor); video_write32(command + 16, owner->token ^ token_xor);
    video_write32(command + 20, ordinal); video_write32(command + 24, due); video_write32(command + 28, due);
    return video_pace_control_locked(command, bytes, 1, playback);
}

/* One step per playback poll, under the image mutex. */
static void pace_step(video_owner *owner) {
    video_presenter *p = owner->presenter;
    uint32_t now = tick();
    if (pace_state == 0 && p->shown >= 4) {
        assert(pace_command(owner, VIDEO_PACE_BYTES - 1, p->queue.released + 2, now, 0) == VIDEO_CONTROL_FORMAT);
        assert(pace_command(owner, VIDEO_PACE_BYTES, 0, now, 0) == VIDEO_CONTROL_FORMAT);
        assert(pace_command(owner, VIDEO_PACE_BYTES, p->queue.released + 2, now, 1) == VIDEO_CONTROL_STALE);
        assert(!pace_command(owner, VIDEO_PACE_BYTES, p->queue.released + 2, now + 20000, 0));
        if (pace_fixture == 2) { assert(!p->pace_serial && !p->phone_paced); pace_state = 4; return; }
        pace_released = p->queue.released; pace_state = 1;
    } else if (pace_state == 1 && p->queue.released > pace_released) {
        assert(!p->phone_paced && p->pace_applied == p->pace_serial); /* 20 s away: ignored */
        pace_ordinal = p->queue.released + 3; pace_due = now + 600;
        assert(!pace_command(owner, VIDEO_PACE_BYTES, pace_ordinal, pace_due, 0));
        pace_state = 2;
    } else if (pace_state == 2 && p->queue.released >= pace_ordinal) {
        /* The commanded picture waits for its due tick, not the old cadence. */
        assert(p->phone_paced && (int32_t)(now - pace_due) >= -2 && (int32_t)(now - pace_due) < 150);
        pace_ordinal = p->queue.released + 1; pace_due = now - 200; pace_holds = p->holds;
        pace_released = p->queue.released; pace_from = now;
        assert(!pace_command(owner, VIDEO_PACE_BYTES, pace_ordinal, pace_due, 0));
        pace_state = 3;
    } else if (pace_state == 3 && p->queue.released >= pace_released + 3) {
        /* Overdue pictures catch up faster than the cadence, and a late picture
         * no longer re-anchors the schedule to the newest picture. */
        assert(now - pace_from < 3 * 1000 / 15 && p->holds > pace_holds && p->pacing.ordinal <= pace_released + 2);
        pace_state = 4;
    }
}

static void queued_consumer_mode(uint32_t mode, int with_pause) {
    configure(); request_next = 0; consume_enabled = queued_upload = 1;
    if (dither_fixture) views[0] = (video_heap_view){410024, 400000}; /* Admission model, not a hardware claim. */
    paired_fixture = mode == 3 ? 1 : mode; partner_anchor = 0; partner_native = mode == 3;
    uint32_t here = mode == 2 ? CFW_MESSAGE_RIGHT : CFW_MESSAGE_LEFT;
    assert(video_sync_init(&partner, 1, here ^ CFW_MESSAGE_BOTH));
    uint32_t prefix = with_pause == 2 ? 7 : 0, visible = 32 - prefix;
    partner.next = prefix + 1;
    context.framebuffer_shadow = 0;
    assert(video_timeline_configure(&context.video_control.timeline, 1, (32000 + 7) / 15, (prefix * 1000 + 7) / 15, 15, 1));
    context.video_control.owner_origin = 1;
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_QUEUED_PRESENTATION;
    if (dither_fixture) p[18] |= VIDEO_DITHER_PRESENTATION;
    if (partner_native) p[18] |= VIDEO_PRESENT_NATIVE;
    if (prefix) { p[18] |= VIDEO_START_PAUSED; p[19] = prefix; }
    cfw_message_route route = control_route; route.targets = mode ? CFW_MESSAGE_BOTH : CFW_MESSAGE_LEFT;
    assert(!video_control_received(p, 24, &route));
    context.video_control.owner_here = here;
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    video_owner *owner = context.video_owner;
    assert(owner->presenter && creates == 2 && owner->presenter->storage.used <=
           (dither_fixture ? VIDEO_PRESENTER_Y8_LIMIT : VIDEO_PRESENTER_A4_LIMIT));
    assert(owner->presenter->queue.format == (dither_fixture ? VIDEO_SOURCE_Y8 | VIDEO_SOURCE_DITHER : VIDEO_SOURCE_A4));
    picture_relock_refusals = 3;
    playback_pause_fixture = with_pause; playback_pause_count = 0;
    uint32_t started = tick();
    assert(worker_enqueue_stream());
    uint32_t limit = tick() + 6000;
    for (;;) {
        display_pump(); pool_drain();
        assert(!take(1, 1000));
        if (pace_fixture) pace_step(owner);
        int done = copied_frames == visible && owner->presenter->shown == visible;
        give(1);
        if (done) break;
        assert((int32_t)(limit - tick()) > 0 && !context.video.fault); delay(1);
    }
    assert(tick() - started >= (visible - 1) * 1000 / 15 && consumed_frames == 32 && decoded_calls == 34);
    assert(!picture_relock_refusals);
    assert(playback_pause_count == (uint32_t)(with_pause != 0)); playback_pause_fixture = 0;
    assert(owner->presenter->queue.released == 32);
    uint32_t first_report = visible > VIDEO_SHOWN_ROWS ? visible - VIDEO_SHOWN_ROWS + 1 : 1;
    assert(owner->presenter->shown_rows.high == visible && owner->presenter->shown_rows.lost == first_report - 1);
    uint8_t request[20] = {0}, shown_report[VIDEO_SHOWN_BYTES] = {0};
    video_write32(request + 8, 1); video_write32(request + 12, owner->token);
    video_write32(request + 16, 1);
    assert(!take(1, 1000));
    uint32_t reply_before = tick();
    assert(video_shown_snapshot_locked(&context, request, 1, shown_report) == VIDEO_CONTROL_ACCEPTED);
    uint32_t reply_tick = video_read32(shown_report + 56);
    /* The reply tick is read during the snapshot and never precedes the reported copy. */
    assert((uint32_t)(reply_tick - reply_before) <= (uint32_t)(tick() - reply_before) &&
           (uint32_t)(reply_tick - video_read32(shown_report + 24)) < 0x80000000u);
    assert(video_read32(shown_report + 12) == first_report && video_read32(shown_report + 16) == prefix + first_report &&
           ((video_read32(shown_report + 36) & 4) != 0) == (first_report > 1) &&
           video_read32(shown_report + 48) == first_report - 1);
    assert((video_read32(shown_report + 36) & VIDEO_SHOWN_DITHER) == (dither_fixture ? VIDEO_SHOWN_DITHER : 0));
    assert((uint32_t)(video_read32(shown_report + 68) - video_read32(shown_report + 64)) <= VIDEO_DISPLAY_LIMIT_MS);
    uint8_t timing_request[20] = {0}, timing_report[VIDEO_TRIGGER_TIMING_BYTES] = {0};
    video_write32(timing_request + 8, 1);
    video_write32(timing_request + (here == CFW_MESSAGE_LEFT ? 12 : 16), owner->token);
    assert(!video_trigger_snapshot_locked(&context, timing_request, 1, timing_report));
    assert(video_read32(timing_report + 80) == 1 &&
           video_read32(timing_report + 88) <= video_read32(timing_report + 92));
    if (here == CFW_MESSAGE_RIGHT) assert(video_read32(timing_report + 52) + video_read32(timing_report + 56) > 0);
    timing_request[8] ^= 1;
    assert(video_trigger_snapshot_locked(&context, timing_request, 1, timing_report) == VIDEO_CONTROL_STALE);
    request[12] ^= 1;
    assert(video_shown_snapshot_locked(&context, request, 1, shown_report) == VIDEO_CONTROL_STALE);
    give(1);
    control_command(p, VIDEO_CONTROL_STOP, 1);
    assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); finish(); assert(!presenter_live && terminates == 2);
    shadow_free(owned_shadow); context.framebuffer_shadow = 0;
    paired_fixture = 0;
    for (uint32_t fail = 8; fail <= 10; ++fail) {
        configure(); request_next = 0; fail_at = fail;
        assert(video_timeline_configure(&context.video_control.timeline, 1, 2000, 0, 15, 1));
        context.video_control.owner_origin = 1;
        control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_QUEUED_PRESENTATION;
        assert(!video_control_received(p, 24, &route));
        pool_drain(); finish(); assert(!allocations_live && !presenter_live);
    }
    puts("independent guarded presenter, owned queue, fixed cadence and teardown PASS");
}

static void queued_consumer(void) { queued_consumer_mode(0, 0); }
static void phone_pacing(void) {
    for (pace_fixture = 1; pace_fixture <= 2; ++pace_fixture) {
        pace_state = 0;
        queued_consumer_mode(pace_fixture == 1 ? 0 : 1, 0);
        assert(pace_state == 4);
    }
    pace_fixture = 0;
    puts("phone PACE re-anchors the coordinator, catches up and is ignored by paired LEFT PASS");
}
static void timed_consumer(void) {
    timed_upload = 1;
    queued_consumer_mode(0, 0); queued_consumer_mode(1, 0); queued_consumer_mode(2, 0);
    timed_upload = 0;
    puts("timed NAL records feed single, paired left and paired right presenters PASS");
}
static void timed_record(uint8_t *record, uint32_t sequence, uint32_t ordinal, uint32_t pts, uint8_t nal) {
    record[0] = VIDEO_MESSAGE_ID; record[1] = VIDEO_CONTROL_NAL_TIMED;
    video_write32(record + 2, 1); video_write32(record + 6, sequence);
    video_write32(record + 10, ordinal); video_write32(record + 14, pts);
    record[VIDEO_NAL_TIMED_HEADER_BYTES] = nal; record[VIDEO_NAL_TIMED_HEADER_BYTES + 1] = (uint8_t)sequence;
}
static void timed_input(void) {
    configure(); request_next = 0;
    assert(video_timeline_configure(&context.video_control.timeline, 1, 2000, 0, 15, 1));
    context.video_control.owner_origin = 1;
    uint8_t p[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    uint8_t record[VIDEO_NAL_TIMED_HEADER_BYTES + 2], plain[VIDEO_NAL_HEADER_BYTES + 2];
    timed_record(record, 2, 1, 0, 0x65);
    assert(video_control_received(record, sizeof(record), &control_route)); /* No owner yet. */
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(control_snapshot(p, 24, snapshot) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    video_owner *owner = context.video_owner;
    const video_timeline *timeline = &context.video_control.timeline;
    for (uint32_t sequence = 0; sequence < 2; ++sequence) {
        memset(plain, 0, sizeof(plain)); plain[0] = VIDEO_MESSAGE_ID; plain[1] = VIDEO_CONTROL_NAL;
        video_write32(plain + 2, 1); video_write32(plain + 6, sequence);
        plain[VIDEO_NAL_HEADER_BYTES] = sequence ? 0x68 : 0x67;
        assert(!video_control_received(plain, sizeof(plain), &control_route));
    }
    assert(!timeline->count && owner->queue.accepted == 2);
    assert(!video_control_received(record, sizeof(record), &control_route));
    assert(timeline->count == 1 && timeline->accepted == 1 && owner->queue.accepted == 3);
    /* An exact replay is accepted without offering its frame time twice. */
    assert(!video_control_received(record, sizeof(record), &control_route));
    assert(timeline->count == 1 && owner->queue.accepted == 3);
    /* A refused frame time leaves the NAL unqueued even though it had room. */
    timed_record(record, 3, 3, 125, 0x41);
    assert(video_control_received(record, sizeof(record), &control_route));
    assert(timeline->count == 1 && owner->queue.accepted == 3);
    cfw_message_route wrong = control_route; wrong.origin = 2;
    timed_record(record, 3, 2, 62, 0x41);
    assert(video_control_received(record, sizeof(record), &wrong));
    assert(timeline->count == 1 && owner->queue.accepted == 3);
    assert(!video_control_received(record, sizeof(record), &control_route));
    assert(timeline->count == 2 && timeline->accepted == 2 && owner->queue.accepted == 4);
    /* Outside the four-slot window: the NAL refuses before any frame time. */
    timed_record(record, 4, 3, 125, 0x41);
    assert(video_control_received(record, sizeof(record), &control_route));
    assert(timeline->count == 2 && owner->queue.accepted == 4);
    /* The flagged credit request packs free timeline records with the result. */
    cfw_message_route wide = control_route; wide.reply_capacity = 21;
    for (uint32_t flagged = 0; flagged <= 2; ++flagged) {
        control_command(p, VIDEO_CONTROL_CREDITS, 1);
        video_write32(p + 12, context.video.token);
        if (flagged) video_write32(p + 20, flagged == 1 ? VIDEO_CREDITS_TIMELINE : 2);
        control_reply_size = 0;
        int result = video_control_received(p, flagged ? VIDEO_CREDITS_TIMELINE_REQUEST_BYTES :
                                            VIDEO_CREDITS_REQUEST_BYTES, &wide);
        assert(control_reply_size == VIDEO_REPLY_HEADER_BYTES + VIDEO_CREDITS_BYTES);
        uint8_t code = control_reply[VIDEO_REPLY_HEADER_BYTES + 5];
        if (flagged == 1) assert(!result && code == (VIDEO_TIMELINE_RECORDS - 2) << VIDEO_CREDIT_TIMELINE_SHIFT);
        else if (flagged == 2) assert(result && code == VIDEO_CONTROL_FORMAT);
        else assert(!result && !code);
    }
    context.video_control.timeline.count = 0;
    control_command(p, VIDEO_CONTROL_CREDITS, 1);
    video_write32(p + 12, context.video.token);
    video_write32(p + 20, VIDEO_CREDITS_TIMELINE);
    assert(!video_control_received(p, VIDEO_CREDITS_TIMELINE_REQUEST_BYTES, &wide));
    assert(control_reply[VIDEO_REPLY_HEADER_BYTES + 5] == 6 << VIDEO_CREDIT_TIMELINE_SHIFT);
    context.video_control.timeline.count = 2;
    control_command(p, VIDEO_PLAYBACK_CAPABILITIES, 0);
    memset(snapshot, 0, sizeof(snapshot));
    assert(control_response(p, VIDEO_CONTROL_HEADER_BYTES, snapshot, VIDEO_PLAYBACK_CAPABILITIES_BYTES) ==
           VIDEO_CONTROL_ACCEPTED && snapshot[7] == 255);
    control_command(p, VIDEO_CONTROL_STOP, 1);
    assert(control_snapshot(p, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
    timed_record(record, 4, 3, 125, 0x41);
    assert(video_control_received(record, sizeof(record), &control_route));
    pool_drain(); finish();
    puts("timed NAL records publish frame time and NAL together; credits report timeline room PASS");
}
static void dither_consumer(void) {
    dither_fixture = 1;
    queued_consumer_mode(0, 0); queued_consumer_mode(1, 0);
    queued_consumer_mode(2, 0); queued_consumer_mode(3, 0);
    dither_fixture = 0;
    queued_consumer_mode(0, 0); /* Fresh default generation returns to A4. */
    configure(); request_next = 0; views[0].max_alloc = 100000;
    assert(video_timeline_configure(&context.video_control.timeline, 1, 2000, 0, 15, 1));
    context.video_control.owner_origin = 1;
    uint8_t p[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_DITHER_PRESENTATION;
    assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_FORMAT);
    control_command(p, VIDEO_CONTROL_START, 1);
    p[18] = VIDEO_DITHER_PRESENTATION | VIDEO_QUEUED_PRESENTATION;
    assert(!control_snapshot(p, 24, report)); pool_drain(); finish();
    assert(!allocations_live && !presenter_live && !radio_active && !context.video.published);
    puts("owned Y8 dither both roles/scales, default-off restart and fragmented admission refusal PASS");
}
static void paired_consumer(void) {
    queued_consumer_mode(1, 0); queued_consumer_mode(2, 0);
    puts("LEFT triggered follower and RIGHT coordinator with actual display copies PASS");
}
static void playback_controls(void) {
    queued_consumer_mode(0, 1); queued_consumer_mode(2, 1);
    puts("owner-bound boundary pause, frozen position, paired ACK and re-anchored resume PASS");
}
static void seek_controls(void) {
    queued_consumer_mode(0, 2); queued_consumer_mode(2, 2);
    puts("hidden decoded preroll, paused target preview and distinct media/copy ordinals PASS");
}
static void menu_controls(void) {
    queued_consumer_mode(0, 3); queued_consumer_mode(2, 3); queued_consumer_mode(3, 0);
    puts("common menu actions, retained video, nonmedia overlay copies and immutable native triggers PASS");
}

static void sync_posting(void) {
    configure();
    uint8_t body[VIDEO_SYNC_BYTES] = {VIDEO_SYNC_BRIDGE, 2, 1, VIDEO_SYNC_READY};
    uint32_t allocations = allocation_calls, locks = mutex_calls, wakes = event_calls;
    service_context = 1;
    assert(video_sync_received(body, sizeof(body) - 1) == 6);
    for (uint32_t i = 0; i < VIDEO_SYNC_MAIL_ROWS; ++i) assert(!video_sync_received(body, sizeof(body)));
    assert(video_sync_received(body, sizeof(body)) == 6);
    assert(context.video_control.sync_mail.overflow);
    service_context = 0;
    assert(allocation_calls == allocations && mutex_calls == locks && event_calls == wakes);
    pool_drain();
    context.video_control.sync_mail.high = context.video_control.sync_mail.low = UINT32_MAX;
    assert(video_sync_received(body, sizeof(body)) == 6); pool_drain();
    assert(!allocations_live && !task_live && !presenter_live);
    puts("bounded ordered callback copying, overflow and finite-counter refusal without waits/heap PASS");
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
            if (video_read32(snapshot + 60) == 34 && video_read32(snapshot + 64) == 32 &&
                video_read32(snapshot + 80) == 32) break;
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
            assert(context.video_power.deadline && context.video_power.acquired);
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
            assert(!context.video_power.deadline && !context.video_power.acquired);
            if (variant == 2 || variant == 3 || variant == 6)
                assert(context.video_presentation.failures == 1);
        }
        if (owned_shadow) { shadow_free(owned_shadow); context.framebuffer_shadow = 0; }
        assert(!overlay_calls);
    }
    puts("heap/gate/queue/copy refusal, STOP completion, claimed-queue failure and timeout retention PASS");
}
static void status_copy_tuple(void) {
    configure(); request_next = 0;
    uint8_t command[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    control_command(command, VIDEO_CONTROL_START, 1);
    assert(!control_snapshot(command, 24, report)); pool_drain(); await_ready();
    assert(!video_read32(report + 80) && !video_read32(report + 88));
    assert(!take(1, 1000));
    uint32_t token = context.video.token;
    video_presentation_state *p = &context.video_presentation;
    p->token = token; p->ordinal = 2; p->presented = 1; p->completed_tick = 100;
    p->copy_tick = 0; p->pin = 1; p->phase = VIDEO_DISPLAY_QUEUED;
    context.direct_shadow = context.framebuffer_shadow;
    assert(video_lifecycle_pin(&context.video, token, 0));
    video_control_replay entry = {0}; entry.command[1] = VIDEO_CONTROL_STATUS;
    video_snapshot(&context, &entry);
    assert(video_read32(entry.snapshot + 80) == 1 && video_read32(entry.snapshot + 88) == 100);
    give(1);
    service_context = 1;
    assert(video_display_claim() == token);
    video_display_complete(token, 1);
    service_context = 0;
    assert(!take(1, 1000));
    p->copy_tick = 0; /* An actual counter wrap is valid, not missing evidence. */
    video_snapshot(&context, &entry);
    assert(video_read32(entry.snapshot + 80) == 1 && video_read32(entry.snapshot + 88) == 100);
    assert(!video_display_fold_locked());
    video_snapshot(&context, &entry);
    assert(video_read32(entry.snapshot + 80) == 2 && !video_read32(entry.snapshot + 88));
    assert(video_lifecycle_pin(&context.video, token, 0));
    p->ordinal = 0; p->pin = 1; p->phase = VIDEO_DISPLAY_COMPLETE; p->copy_tick = 999;
    assert(!video_display_fold_locked());
    video_snapshot(&context, &entry);
    assert(video_read32(entry.snapshot + 80) == 2 && !video_read32(entry.snapshot + 88));
    give(1);
    control_command(command, VIDEO_CONTROL_STOP, 1);
    assert(!control_snapshot(command, 12, report)); pool_drain(); finish();
    assert(video_read32(report + 80) == 2 && !video_read32(report + 88));
    puts("STATUS before first copy, pending/callback/fold, genuine zero wrap and menu tuple PASS");
}

static void profile_mailboxes(void) {
    configure();
    video_owner owner = {.context = &context, .token = 3};
    video_presenter presenter = {.decoder = &owner, .token = 3, .generation = 7};
    presenter.sync.stream = 11;
    context.video_control.start_guard = context.video_control.control_generation = 7;
    context.video_control.owner_origin = context.video_control.owner_here = CFW_MESSAGE_LEFT;
    video_profile_start_locked(&presenter);
    video_profile_state *s = &context.video_profile;
    force_cycles = 1; forced_cycles = 4;
    video_profile_capture captured = video_profile_begin();
    captured.cycles = UINT32_MAX - 5;
    for (uint32_t i = 0; i < VIDEO_PROFILE_SLOTS + 1; ++i)
        video_profile_end(captured, VIDEO_PROFILE_PACK, 17);
    assert(!s->rows[VIDEO_PROFILE_PACK].count && s->dropped == 1);
    assert(s->flags & VIDEO_PROFILE_DROPPED);
    video_profile_fold_locked(s);
    assert(s->rows[VIDEO_PROFILE_PACK].count == 4 && s->rows[VIDEO_PROFILE_PACK].bytes == 68);
    assert(s->rows[VIDEO_PROFILE_PACK].cycles_lo == 40 && !s->rows[VIDEO_PROFILE_PACK].cycles_hi);
    s->rows[VIDEO_PROFILE_DECODE].cycles_lo = UINT32_MAX - 5;
    video_profile_end(captured, VIDEO_PROFILE_DECODE, 0); video_profile_fold_locked(s);
    assert(s->rows[VIDEO_PROFILE_DECODE].cycles_lo == 4 && s->rows[VIDEO_PROFILE_DECODE].cycles_hi == 1);
    captured.tick = VIDEO_TICK - 1002;
    video_profile_end(captured, VIDEO_PROFILE_QUEUE_WAIT, 0); video_profile_fold_locked(s);
    assert(s->flags & VIDEO_PROFILE_SPAN);
    assert(!s->rows[VIDEO_PROFILE_QUEUE_WAIT].cycles_lo && s->rows[VIDEO_PROFILE_QUEUE_WAIT].ticks >= 1002);
    s->rows[VIDEO_PROFILE_PACK].bytes = UINT32_MAX;
    video_profile_end(captured, VIDEO_PROFILE_PACK, 1); video_profile_fold_locked(s);
    assert(s->rows[VIDEO_PROFILE_PACK].bytes == UINT32_MAX && (s->flags & VIDEO_PROFILE_SATURATED));
    uint8_t out[VIDEO_DIAGNOSTICS_BYTES] = {0}, again[VIDEO_DIAGNOSTICS_BYTES] = {0};
    assert(!video_profile_snapshot_locked(&context, 0, CFW_MESSAGE_LEFT, out));
    assert(out[3] == 3 && out[5] == 1 && video_read32(out + 8) == 7 && video_read32(out + 16) == 3);
    video_profile_state before = *s;
    assert(!video_profile_snapshot_locked(&context, 0, CFW_MESSAGE_LEFT, again));
    assert(!memcmp(out, again, sizeof(out)) && !memcmp(s, &before, sizeof(before)));
    assert(video_profile_snapshot_locked(&context, 2, CFW_MESSAGE_LEFT, out) == VIDEO_CONTROL_FORMAT);
    assert(video_profile_snapshot_locked(&context, 0, CFW_MESSAGE_RIGHT, out) == VIDEO_CONTROL_STALE);
    uint8_t command[VIDEO_START_BYTES], frozen[VIDEO_STATUS_BYTES], replayed[VIDEO_STATUS_BYTES];
    request_next = 0;
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_PROFILE_DIAGNOSTICS);
    assert(!control_response(command, 12, frozen, VIDEO_DIAGNOSTICS_BYTES));
    assert(frozen[3] == 3 && video_read32(frozen + 8) == 7);
    s->rows[VIDEO_PROFILE_DECODE].count += 1;
    assert(!control_response(command, 12, replayed, VIDEO_DIAGNOSTICS_BYTES));
    assert(!memcmp(frozen, replayed, VIDEO_DIAGNOSTICS_BYTES));
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_PROFILE_DIAGNOSTICS);
    command[9] = 1;
    assert(!control_response(command, 12, replayed, VIDEO_DIAGNOSTICS_BYTES) && replayed[4] == 1);
    s->mail[0][0].phase = 1; /* A producer owns this slot across a generation transition. */
    presenter.generation = context.video_control.start_guard = context.video_control.control_generation = 8;
    video_profile_start_locked(&presenter);
    assert(s->mail[0][0].phase == 1);
    s->mail[0][0].generation = 7; s->mail[0][0].phase = 2;
    video_profile_fold_locked(s);
    assert(!s->rows[0].count && !s->mail[0][0].phase);
    video_profile_end(captured, 0, 1); assert(!s->mail[0][0].phase);
    context.video_control.start_guard = 0;
    assert(!video_profile_begin().generation);
    context.video_decode_last = (video_decode_totals){.pictures = 5, .cycles = 0x100000002ull, .max_cycles = 9,
                                                      .header_cycles = 21};
    context.video_sections = (g2_h264_sections_info){.intra_i = 1, .inter_p = 0x300000004ull, .intra_p = 5,
        .completion = 6, .motion = 7, .residual = 8, .transform = 9, .intra = 10, .motion_calls = 11,
        .residual_calls = 12, .transform_calls = 13, .intra_calls = 14, .zero_transforms = 15,
        .pictures = 5, .timer_cost = 3};
    uint8_t sections[VIDEO_DIAGNOSTICS_BYTES] = {0};
    assert(!video_sections_snapshot_locked(&context, CFW_MESSAGE_LEFT, sections));
    assert(sections[3] == VIDEO_SECTION_DIAGNOSTICS && sections[5] == 1 && sections[6] == CFW_MESSAGE_LEFT);
    assert(video_read32(sections + 8) == 8 && video_read32(sections + 12) == 5);
    assert(video_read32(sections + 16) == 2 && video_read32(sections + 20) == 1 && video_read32(sections + 24) == 9);
    assert(video_read32(sections + 28) == 5 && video_read32(sections + 32) == 1);
    assert(video_read32(sections + 40) == 4 && video_read32(sections + 44) == 3 && video_read32(sections + 88) == 10);
    assert(video_read32(sections + 96) == 11 && video_read32(sections + 112) == 15);
    assert(video_read32(sections + 116) == 3 && video_read32(sections + 120) == 21);
    assert(video_sections_snapshot_locked(&context, CFW_MESSAGE_RIGHT, sections) == VIDEO_CONTROL_STALE);
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_SECTION_DIAGNOSTICS);
    assert(!control_response(command, 12, frozen, VIDEO_DIAGNOSTICS_BYTES));
    assert(frozen[3] == VIDEO_SECTION_DIAGNOSTICS && video_read32(frozen + 40) == 4);
    static uint8_t picture[32 * 64];
    for (uint32_t i = 0; i < sizeof(picture); ++i) picture[i] = (uint8_t)i;
    const g2_h264_frame_info frame = {picture, 60, 32, 64, 2, 0};
    video_bench_result bench = {0};
    video_bench_run(&frame, &bench);
    assert(bench.done && bench.ccr == 0x30000u && bench.frame_address == (uint32_t)(uintptr_t)picture);
    assert(bench.stride == 64 && bench.frame_bytes == 60 * 32 && bench.block_bytes == 16 * 60);
    assert(bench.window_bytes == 32 * 21 * 24);
    const g2_h264_frame_info narrow = {picture, 24, 32, 64, 2, 0};
    video_bench_run(&narrow, &bench);
    assert(!bench.done && bench.ccr == 0x30000u && !bench.window_bytes);
    context.video_sections.slice_setup = 0x500000006ull; context.video_sections.mb_loop = 7;
    context.video_sections.input = 8;
    context.video_bench = (video_bench_result){.done = 1, .ccr = 0x30000u, .frame_address = 0x20200000u,
        .stride = 320, .alu = 41, .window_bytes = 16128};
    assert(!video_sections_detail_locked(&context, CFW_MESSAGE_LEFT, sections));
    assert(sections[3] == VIDEO_SECTION_DIAGNOSTICS && sections[4] == 1 && video_read32(sections + 8) == 8);
    assert(video_read32(sections + 12) == 6 && video_read32(sections + 16) == 5);
    assert(video_read32(sections + 20) == 7 && video_read32(sections + 28) == 8);
    assert(video_read32(sections + 36) == 1 && video_read32(sections + 40) == 0x30000u);
    assert(video_read32(sections + 48) == 0x20200000u && video_read32(sections + 64) == 41);
    assert(video_read32(sections + 92) == 16128);
    assert(video_sections_detail_locked(&context, CFW_MESSAGE_RIGHT, sections) == VIDEO_CONTROL_STALE &&
           sections[3] == VIDEO_SECTION_DIAGNOSTICS);
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_SECTION_DIAGNOSTICS);
    command[9] = 1;
    assert(!control_response(command, 12, replayed, VIDEO_DIAGNOSTICS_BYTES) && replayed[4] == 1);
    control_command(command, VIDEO_CONTROL_DIAGNOSTICS, VIDEO_SECTION_DIAGNOSTICS);
    command[9] = 2; /* Two pages only. */
    assert(control_response(command, 12, replayed, VIDEO_DIAGNOSTICS_BYTES) == VIDEO_CONTROL_FORMAT);
    puts("bounded profile posts, cycle carry, long-span uncertainty, saturation, stale mail and read-only pages PASS");
    puts("decoder section page: cumulative 64-bit counters, owner check and control route PASS");
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
    assert(p[1] == VIDEO_PANEL_BYTES && (owned_shadow ||
        context.video_handoff.phase == VIDEO_HANDOFF_COPYING));
    assert((context.direct_pending != 0) == (context.video_presentation.phase == VIDEO_DISPLAY_COPYING ||
        context.video_handoff.phase == VIDEO_HANDOFF_COPYING));
    ++flush_calls;
}
#define FW_DISPLAY_QUEUE display_queue
#define FW_DISPLAY_FB (missing_fb ? 0 : physical_frame)
#define FW_FLUSH display_flush
static void stock_display_copy(void) {
    ++stock_copies;
    for (uint32_t y = 96; y < 384; ++y)
        memset(physical_frame + y * VIDEO_PANEL_STRIDE + 16, 0x22, 288);
}
#define FW_DISPLAY_COPY stock_display_copy
#include "upstream_cleanup.h"
#define VIDEO_HANDOFF_NATIVE 1
#define VIDEO_HANDOFF_QUEUE handoff_queue
#include "../../patches/video/handoff.c"

static int handoff_queue(const uint32_t *record) {
    assert(!image_depth && !service_context && display_gate && !task_live && !allocations_live &&
           context.direct_pending && !context.direct_shadow && !context.direct_active);
    ++handoff_queues;
    if (fail_handoff == 1) return 0;
    assert(!display_job);
    memcpy(handoff_record, record, sizeof(handoff_record));
    assert(record[0] == 3 && !record[1] && !record[2] && !record[3] && !record[4] &&
           record[5] == 640 && record[6] == 480 && record[7] && !record[8]);
    __atomic_store_n(&display_job, 2, __ATOMIC_RELEASE);
    if (fail_handoff == 2) { display_pump(); return 0; }
    if (fail_handoff == 3) {
        uint32_t unrelated[VIDEO_HANDOFF_RECORD_WORDS]; memcpy(unrelated, record, sizeof(unrelated));
        unrelated[7] = 0; video_display_restore_copy(unrelated);
        assert(context.video_handoff.phase == VIDEO_HANDOFF_QUEUED && context.direct_pending && display_gate);
    }
    return 1;
}

static void display_pump(void) {
    uint32_t job = __atomic_exchange_n(&display_job, 0, __ATOMIC_ACQ_REL);
    if (!job) return;
    assert(!image_depth && display_gate && context.direct_pending);
    if (job == 2) {
        video_display_restore_copy(handoff_record);
        if (!missing_fb)
            for (uint32_t y = 0; y < VIDEO_PANEL_HEIGHT; ++y)
                for (uint32_t x = 0; x < VIDEO_PANEL_STRIDE; ++x)
                    assert(physical_frame[y * VIDEO_PANEL_STRIDE + x] ==
                        (y >= 96 && y < 384 && x >= 16 && x < 304 ? 0x22 : 0));
        display_signal(); return;
    }
    display_copy_hook();
    if (!missing_fb) {
        int native = context.video_control.options & VIDEO_PRESENT_NATIVE;
        for (uint32_t y = 0; y < VIDEO_PANEL_HEIGHT; ++y)
            for (uint32_t x = 0; x < VIDEO_PANEL_STRIDE; ++x)
                if (context.video_presentation.ordinal || y < 432) {
                    uint32_t gray = 0x88;
                    if (context.video_control.options & VIDEO_DITHER_PRESENTATION) {
                        /* Synthetic unsignalled TV Y128 normalizes to130. The
                         * centered rescale yields quarter32, with no promotion. */
                        const uint32_t threshold[2][2] = {{0, 2}, {3, 1}};
                        uint32_t t = (130 * 239 + 510) / 1020 + 2;
                        assert(t == 32);
                        gray = (((t >> 2) + ((t & 3) > threshold[y & 1][(x * 2) & 1])) << 4) |
                               ((t >> 2) + ((t & 3) > threshold[y & 1][(x * 2 + 1) & 1]));
                    }
                    assert(physical_frame[y * VIDEO_PANEL_STRIDE + x] ==
                        ((native ? y >= 144 && y < 336 && x >= 80 && x < 240 : y >= 48 && y < 432) ? gray : 0));
                }
        if (context.video_presentation.ordinal) ++copied_frames;
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

static void handback(void) {
    uint8_t command[VIDEO_START_BYTES], snapshot[VIDEO_STATUS_BYTES];
    for (uint32_t variant = 0; variant < 12; ++variant) {
        configure(); request_next = 0; context.framebuffer_shadow = 0;
        video_control_state *s = &context.video_control;
        s->stream = s->stream_high = s->control_generation = 1;
        s->owner_origin = CFW_MESSAGE_LEFT;
        context.video_presentation.token = 7; context.video_presentation.generation = 1;
        context.video_presentation.presented = 1; context.direct_active = 1;
        memset(physical_frame, 0x88, sizeof(physical_frame));
        video_display_note_output(7);
        context.video_power.generation = 1; context.video_power.deadline = tick() + 5000;
        context.video_power.stream = context.video_power.nonce = context.video_power.serial = 1;
        context.video_power.peer_deadline = tick() + 5000; context.video_power.acquired = 1;
        context.video_power.peer.deadline = tick() + 5000;
        uint32_t peer_deadline = context.video_power.peer.deadline;
        if (variant == 1) { memset(physical_frame, 0xaa, sizeof(physical_frame)); video_display_note_output(0); }
        if (variant == 3) fail_handoff = 1;
        if (variant == 4) handoff_hold = 1;
        if (variant == 5) fail_handoff = 2;
        if (variant == 6) {
            context.video_presentation.presented = context.video_presentation.output_epoch = 0;
        }
        if (variant == 7) power_awake = 0;
        if (variant == 8) fail_handoff = 3;
        if (variant == 9) context.video_presentation.generation = 2;
        if (variant == 10) fail_gate = 1;
        if (variant == 11) missing_fb = 1;
        if (variant == 2) {
            s->start_guard = 1;
            assert(!take(1, 0)); assert(!cfw_cleanup_session()); give(1);
        } else {
            control_command(command, VIDEO_CONTROL_STOP, 1);
            assert(control_snapshot(command, 12, snapshot) == VIDEO_CONTROL_ACCEPTED &&
                   snapshot[3] == VIDEO_STOPPING);
        }
        pool_drain();
        assert(!task_live && !allocations_live && context.video_power.peer.deadline == peer_deadline);
        if (variant == 4) {
            assert(context.video_handoff.phase == VIDEO_HANDOFF_QUEUED &&
                   context.direct_pending && display_gate && s->stop_generation);
            control_command(command, VIDEO_CONTROL_STATUS, 0);
            assert(control_snapshot(command, 8, snapshot) == VIDEO_CONTROL_ACCEPTED &&
                   snapshot[3] == VIDEO_STOPPING && video_read32(snapshot + 24) == VIDEO_CONTROL_HANDOFF);
            control_command(command, VIDEO_CONTROL_START, 2);
            assert(control_snapshot(command, 24, snapshot) == VIDEO_CONTROL_BUSY);
            display_copy_hook(); /* Periodic copy does not own this tagged queue job. */
            assert(context.video_handoff.phase == VIDEO_HANDOFF_QUEUED && display_gate);
            handoff_hold = 0; display_pump(); pool_drain();
        }
        assert(!s->stop_generation && !context.direct_pending && !display_gate && !display_job &&
               !context.video_power.acquired && !context.video_power.deadline && !power_starts);
        if (variant == 1 || variant == 6 || variant == 9) {
            assert(!handoff_queues);
            for (uint32_t i = 0; i < VIDEO_PANEL_BYTES; ++i)
                assert(physical_frame[i] == (variant == 1 ? 0xaa : 0x88));
        } else if (variant == 3 || variant == 10 || variant == 11) {
            assert(s->error == VIDEO_CONTROL_HANDOFF && !context.direct_active);
        } else {
            assert(handoff_queues == 1 && !context.direct_active &&
                   context.video_handoff.phase == VIDEO_HANDOFF_COMPLETE);
        }
        uint32_t previous = handoff_queues;
        control_command(command, VIDEO_CONTROL_STOP, 1);
        assert(control_snapshot(command, 12, snapshot) == VIDEO_CONTROL_ACCEPTED);
        pool_drain(); assert(handoff_queues == previous); /* No stale repaint on repeated STOP. */
        finish();
    }
    puts("stock screen/margins, mode-11, new output, partial start, sleep, refusal and late copy PASS");
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

static void radio_disconnect(void) {
    configure(); request_next = 0;
    uint8_t request[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    control_command(request, VIDEO_CONTROL_START, 1);
    assert(control_snapshot(request, 24, report) == VIDEO_CONTROL_ACCEPTED);
    pool_drain(); await_ready();
    uint32_t generation = context.video_control.control_generation;
    radio_lost = generation + 1; video_control_notify_radio(); pool_drain();
    assert(state() == VIDEO_READY);
    radio_lost = generation;
    uint32_t frees = release_calls, stopped = terminates;
    service_context = 1; video_control_notify_radio(); service_context = 0;
    assert(release_calls == frees && terminates == stopped && state() == VIDEO_READY);
    pool_drain(); finish();
    assert(!context.video_control.start_guard);
}

static void policy_contract(void) {
    configure(); request_next = 0;
    uint8_t p[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    control_command(p, VIDEO_CONTROL_RADIO_POLICY, 0); video_write32(p + 8, UINT32_MAX);
    assert(!control_response(p, 12, report, VIDEO_RADIO_POLICY_BYTES));
    assert(video_read32(report + 4) == 1 && video_read32(report + 8) == UINT32_MAX && !video_read32(report + 12));
    control_command(p, VIDEO_CONTROL_RADIO_POLICY, 0); video_write32(p + 8, 2);
    assert(control_response(p, 12, report, VIDEO_RADIO_POLICY_BYTES) == VIDEO_CONTROL_FORMAT);
    control_command(p, VIDEO_CONTROL_RADIO_POLICY, 0); video_write32(p + 8, 0);
    assert(!control_response(p, 12, report, VIDEO_RADIO_POLICY_BYTES));
    assert(!control_response(p, 12, report, VIDEO_RADIO_POLICY_BYTES) && !radio_acquires);
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(!control_snapshot(p, 24, report)); pool_drain(); await_ready();
    assert(radio_off && radio_acquires == 1 && (context.video_control.options & VIDEO_RADIO_OFF));
    control_command(p, VIDEO_CONTROL_RADIO_POLICY, 0); video_write32(p + 8, 1);
    assert(!control_response(p, 12, report, VIDEO_RADIO_POLICY_BYTES));
    assert(video_read32(report + 4) == 1 && !video_read32(report + 8) && video_read32(report + 12) == 1);
    assert(radio_off && radio_acquires == 1 && (context.video_control.options & VIDEO_RADIO_OFF));
    control_command(p, VIDEO_CONTROL_RESET, 1); assert(!control_snapshot(p, 12, report)); pool_drain(); finish();
    control_command(p, VIDEO_CONTROL_START, 2); assert(!control_snapshot(p, 24, report)); pool_drain(); await_ready();
    assert(!radio_off && radio_acquires == 2 && !(context.video_control.options & VIDEO_RADIO_OFF));
    control_command(p, VIDEO_CONTROL_STOP, 2); assert(!control_snapshot(p, 12, report)); pool_drain(); finish();
    assert(radio_releases == 2);
    puts("default/query/invalid/replay policy, pending selection and next generation latch PASS");
}

static void gray_contract(void) {
    configure(); request_next = 0;
    uint8_t p[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    enum { RETIRED_GRAY_CONTROL = 23 };
    const uint32_t selections[] = {0, 1, UINT32_MAX};
    for (uint32_t i = 0; i < 3; ++i) {
        control_command(p, RETIRED_GRAY_CONTROL, 0); video_write32(p + 8, selections[i]);
        assert(control_snapshot(p, 12, report) == VIDEO_CONTROL_FORMAT);
        assert(control_snapshot(p, 12, report) == VIDEO_CONTROL_FORMAT);
        assert(!context.video_control.options && !context.video_control.stream_high);
    }
    control_command(p, 13, 0);
    assert(!control_response(p, 8, report, 16) && (report[7] & 8)); /* Bit 3 now marks multi-slice pictures. */
    control_command(p, VIDEO_CONTROL_START, 1);
    assert(!control_snapshot(p, 24, report)); pool_drain(); await_ready();
    control_command(p, RETIRED_GRAY_CONTROL, 0); video_write32(p + 8, 1);
    assert(control_snapshot(p, 12, report) == VIDEO_CONTROL_FORMAT);
    assert(!context.video_control.options && context.video_control.stream == 1);
    control_command(p, VIDEO_CONTROL_STOP, 1); assert(!control_snapshot(p, 12, report)); pool_drain(); finish();
    puts("retired gray selections/query/replay refused; capability bit reused for multi-slice; playback unchanged PASS");
}

static void radio_provenance(void) {
    for (uint32_t origin = 1; origin <= 2; ++origin) for (uint32_t here = 1; here <= 2; ++here) {
        configure(); request_next = 0;
        cfw_message_route route = control_route;
        route.here = here; route.origin = origin; route.targets = CFW_MESSAGE_BOTH;
        uint8_t p[VIDEO_START_BYTES]; control_command(p, VIDEO_CONTROL_START, 1);
        assert(!video_control_received(p, 24, &route)); pool_drain(); await_ready();
        assert(state() == VIDEO_READY && radio_acquires == 1 && radio_active && radio_off == (here != origin));
        control_command(p, VIDEO_CONTROL_STOP, 1);
        assert(!video_control_received(p, 12, &route)); pool_drain(); finish();
        assert(radio_releases == 1);
    }
    puts("both bridge ingress choices and locally delivered owners exclude lease-only peer PHY PASS");
}

static void radio_contract(void) {
    configure(); request_next = 0;
    uint8_t p[VIDEO_START_BYTES], report[VIDEO_STATUS_BYTES];
    control_command(p, VIDEO_CONTROL_CAPABILITIES, 0);
    assert(!control_snapshot(p, 8, report) && report[71] == 0xbf && !radio_acquires);
    control_command(p, VIDEO_CONTROL_START, 1); p[18] = VIDEO_RADIO_OFF;
    assert(!control_snapshot(p, 24, report));
    assert(!control_snapshot(p, 24, report) && !radio_acquires);
    p[18] = 0; assert(video_control_received(p, 24, &control_route)); p[18] = VIDEO_RADIO_OFF;
    pool_drain(); await_ready(); assert(state() == VIDEO_READY && radio_off && radio_acquires == 1);
    control_command(p, VIDEO_CONTROL_RESET, 1); assert(!control_snapshot(p, 12, report));
    pool_drain(); finish(); assert(!radio_active && radio_releases == 1);
    control_command(p, VIDEO_CONTROL_START, 2); deny_radio = 1;
    assert(!control_snapshot(p, 24, report)); pool_drain();
    assert(state() == VIDEO_IDLE && context.video_control.error == VIDEO_CONTROL_RADIO && !allocations_live);
    deny_radio = 0; fail_at = allocation_calls + 1;
    uint32_t created = creates;
    control_command(p, VIDEO_CONTROL_START, 3); assert(!control_snapshot(p, 24, report)); pool_drain();
    assert(!radio_active && radio_releases == 2 && !allocations_live && creates == created);
    fail_at = 0;
    control_command(p, VIDEO_CONTROL_START, 4); assert(!control_snapshot(p, 24, report));
    pool_drain(); await_ready(); assert(state() == VIDEO_READY && !radio_off);
    service_context = 1; video_control_notify_lease(1); service_context = 0;
    pool_drain(); finish(); assert(!radio_active && radio_releases == 3);
    control_command(p, VIDEO_CONTROL_START, 5); p[18] = 4;
    assert(control_snapshot(p, 24, report) == VIDEO_CONTROL_FORMAT && !radio_active);
}

static void geometry_contract(void) {
    const video_geometry sizes[] = {
        {320,192,{0,6,320,180}}, {320,176,{3,0,313,176}}, {144,240,{4,0,135,240}}
    };
    for (uint32_t i = 0; i < 3; ++i) {
        configure();
        uint8_t p[VIDEO_START_GEOMETRY_BYTES] = {0}, snapshot[VIDEO_STATUS_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        const video_geometry g = sizes[i];
        p[12] = g.width; p[13] = g.width >> 8; p[14] = g.height; p[15] = g.height >> 8;
        uint16_t rect[] = {g.picture.x,g.picture.y,g.picture.width,g.picture.height};
        for (uint32_t j = 0; j < 4; ++j) { p[24+2*j] = rect[j]; p[25+2*j] = rect[j] >> 8; }
        assert(!control_snapshot(p, sizeof(p), snapshot));
        assert(context.video_control.geometry.width == g.width && context.video_control.geometry.picture.height == g.picture.height);
        uint8_t replay[VIDEO_STATUS_BYTES];
        assert(!control_snapshot(p, sizeof(p), replay) && !memcmp(snapshot,replay,VIDEO_BASE_STATUS_BYTES));
        ++p[24];
        assert(video_control_received(p, sizeof(p), &control_route));
        --p[24];
        worker_stream_geometry(g.width,g.height);
        feed = 1; pool_drain(); await_ready();
        uint32_t deadline = tick() + 3000;
        while (!__atomic_load_n(&fed, __ATOMIC_ACQUIRE)) { assert((int32_t)(tick()-deadline)<0); delay(1); }
        assert(stream_ok && state() == VIDEO_READY);
        video_worker_report r = video_completed_report(context.video_owner);
        fprintf(stderr,"geometry %ux%u 32 decoded pictures; storage peak %u guards %u limit %u\n",
                g.width,g.height,r.storage_peak,r.guards_ok,video_current_owner()->storage.limit);
        assert(r.guards_ok && r.storage_peak <= video_current_owner()->storage.limit);
        finish();
    }
    worker_stream_geometry(320,192);
    for (uint32_t bad = 0; bad < 5; ++bad) {
        configure();
        uint8_t p[VIDEO_START_GEOMETRY_BYTES] = {0}, snapshot[VIDEO_STATUS_BYTES];
        control_command(p, VIDEO_CONTROL_START, 1);
        p[26] = 0; p[28] = 64; p[29] = 1; p[30] = 176;
        if (bad == 0) p[14] = 176; /* Nonlegacy dimensions need extended command. */
        if (bad == 1) p[25] = 1;
        if (bad == 2) p[30] = 0;
        if (bad == 3) p[24] = 2;
        assert(control_snapshot(p, bad == 0 ? 24 : bad == 4 ? 31 : 32, snapshot) == VIDEO_CONTROL_FORMAT);
        assert(!context.video_control.stream_high && !pool_pending());
        finish();
    }
}

static void picture_style_contract(void) {
    for (uint32_t style = 0; style < 4; ++style) {
        configure();
        assert(video_timeline_configure(&context.video_control.timeline,1,2000,0,15,1));
        context.video_control.owner_origin = 1;
        uint8_t p[VIDEO_START_PICTURE_BYTES] = {0}, snapshot[VIDEO_STATUS_BYTES];
        control_command(p,VIDEO_CONTROL_START,1);
        p[18] = VIDEO_QUEUED_PRESENTATION | VIDEO_DITHER_PRESENTATION;
        p[28] = 64; p[29] = 1; p[30] = 192; p[32] = style;
        assert(!control_snapshot(p,sizeof(p),snapshot));
        assert(context.video_control.picture_style == style);
        uint8_t replay[VIDEO_STATUS_BYTES];
        assert(!control_snapshot(p,sizeof(p),replay) && !memcmp(snapshot,replay,VIDEO_BASE_STATUS_BYTES));
        p[32] ^= 1;
        assert(video_control_received(p,sizeof(p),&control_route));
        pool_drain(); await_ready();
        assert(state() == VIDEO_READY && video_current_owner()->presenter->queue.format ==
            (VIDEO_SOURCE_Y8 | VIDEO_SOURCE_DITHER | (style << VIDEO_SOURCE_STYLE_SHIFT)));
        finish();
    }
    for (uint32_t variant = 0; variant < 3; ++variant) {
        configure();
        uint8_t p[VIDEO_START_PICTURE_BYTES] = {0}, snapshot[VIDEO_STATUS_BYTES];
        control_command(p,VIDEO_CONTROL_START,1);
        p[28] = 64; p[29] = 1; p[30] = 192;
        p[32] = variant == 0 ? 4 : 1;
        if (variant == 1) p[33] = 1;
        assert(control_snapshot(p,sizeof(p),snapshot) == VIDEO_CONTROL_FORMAT);
        assert(!context.video_control.stream_high && !pool_pending()); finish();
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "picture-styles")) picture_style_contract();
    else if (!strcmp(argv[1], "geometry")) geometry_contract();
    else if (!strcmp(argv[1], "normal")) normal();
    else if (!strcmp(argv[1], "allocations")) allocation_failures();
    else if (!strcmp(argv[1], "startup")) startup();
    else if (!strcmp(argv[1], "reserves")) reserves();
    else if (!strcmp(argv[1], "pins")) pins();
    else if (!strcmp(argv[1], "quarantine")) quarantine();
    else if (!strcmp(argv[1], "wakeups")) wakeups();
    else if (!strcmp(argv[1], "preparation")) preparation_cancel();
    else if (!strcmp(argv[1], "controls")) controls();
    else if (!strcmp(argv[1], "control-paging")) control_paging();
    else if (!strcmp(argv[1], "receive-evidence")) receive_evidence();
    else if (!strcmp(argv[1], "rate-contract")) rate_contract();
    else if (!strcmp(argv[1], "credits")) credits();
    else if (!strcmp(argv[1], "diagnostics")) diagnostics();
    else if (!strcmp(argv[1], "control-preparation")) control_preparation();
    else if (!strcmp(argv[1], "control-failures")) control_failures();
    else if (!strcmp(argv[1], "nal-input")) nal_input();
    else if (!strcmp(argv[1], "queue-extension")) queue_extension();
    else if (!strcmp(argv[1], "recovery")) recovery();
    else if (!strcmp(argv[1], "consumer")) consumer();
    else if (!strcmp(argv[1], "queued-consumer")) queued_consumer();
    else if (!strcmp(argv[1], "phone-pacing")) phone_pacing();
    else if (!strcmp(argv[1], "timed-consumer")) timed_consumer();
    else if (!strcmp(argv[1], "timed-input")) timed_input();
    else if (!strcmp(argv[1], "dither-consumer")) dither_consumer();
    else if (!strcmp(argv[1], "paired-consumer")) paired_consumer();
    else if (!strcmp(argv[1], "playback-controls")) playback_controls();
    else if (!strcmp(argv[1], "seek-controls")) seek_controls();
    else if (!strcmp(argv[1], "menu-controls")) menu_controls();
    else if (!strcmp(argv[1], "sync-posting")) sync_posting();
    else if (!strcmp(argv[1], "evidence")) evidence();
    else if (!strcmp(argv[1], "evidence-replays")) evidence_replays();
    else if (!strcmp(argv[1], "presentation-failures")) presentation_failures();
    else if (!strcmp(argv[1], "status-copy")) status_copy_tuple();
    else if (!strcmp(argv[1], "profile")) profile_mailboxes();
    else if (!strcmp(argv[1], "original-display")) original_display();
    else if (!strcmp(argv[1], "inactivity")) inactivity();
    else if (!strcmp(argv[1], "cleanup")) cleanup();
    else if (!strcmp(argv[1], "lease-notifications")) lease_notifications();
    else if (!strcmp(argv[1], "handback")) handback();
    else if (!strcmp(argv[1], "radio-disconnect")) radio_disconnect();
    else if (!strcmp(argv[1], "radio-contract")) radio_contract();
    else if (!strcmp(argv[1], "policy-contract")) policy_contract();
    else if (!strcmp(argv[1], "gray-contract")) gray_contract();
    else if (!strcmp(argv[1], "radio-provenance")) radio_provenance();
    else assert(0);
    assert(!allocations_live && !task_live && !image_depth);
    printf("worker %s PASS\n", argv[1]);
}
