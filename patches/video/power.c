/* SPDX-License-Identifier: GPL-3.0-only */
#include "power.h"
#include "control.h"
#include "platform.h"

#pragma clang section text=".text.video"

#ifndef VIDEO_POWER_PLATFORM_READY
/* UI state is published before INIT reaches the driver. Require both, and
 * never treat a framebuffer copy as evidence that the panel was powered up. */
static int video_power_platform_ready(void) {
    return *(const volatile uint8_t *)0x20077298U == 1 &&
        *(const volatile uint32_t *)0x20076784U == 1 &&
        *(const volatile uint32_t *)0x20076778U == 1;
}
#define VIDEO_POWER_PLATFORM_READY video_power_platform_ready
#ifndef VIDEO_POWER_HERE
#define VIDEO_POWER_HERE cfw_message_lens
#endif
#ifndef VIDEO_POWER_PLATFORM_REFRESH
#define VIDEO_POWER_PLATFORM_REFRESH ((uint32_t (*)(uint32_t, uint32_t, const void *, uint32_t))0x00443e9bU)
#endif
#ifndef VIDEO_POWER_QUEUE_PUT
#define VIDEO_POWER_QUEUE_PUT ((int (*)(uint32_t, const void *, uint8_t, uint32_t))0x00443299U)
#endif

typedef struct {
    uint32_t callback;
    uint16_t kind, bytes;
    uint8_t *payload;
} video_power_packet;
#ifndef VIDEO_POWER_NATIVE
_Static_assert(sizeof(video_power_packet) == 12, "stock sync packet ABI");
#endif

/* Both queues are static 150-entry pointer queues on this exact donor.
 * Bypass CommonMalloc's retries and the public startup wrapper's asserting
 * enqueue failure; stock consumers retain the original paired UI lifecycle. */
static uint32_t video_power_queue(int startup) {
    uint32_t here = VIDEO_POWER_HERE();
    int coordinator = startup && here == CFW_MESSAGE_RIGHT;
    uint32_t handle = *(const volatile uint32_t *)(uintptr_t)(coordinator ? 0x20076d1cU : 0x20076d18U);
    uint32_t expected = coordinator ? 0x20074d20U : 0x20074cd0U;
    if (!here || handle != expected ||
        *(const volatile uint32_t *)(uintptr_t)(handle + 60) != 150 ||
        *(const volatile uint32_t *)(uintptr_t)(handle + 64) != 4) return 0;
    return handle;
}
static int video_power_platform_packet(const uint8_t *body, uint32_t bytes, int startup) {
    uint32_t queue = video_power_queue(startup);
    video_heap_view view;
    /* Includes both allocations, TLSF overhead and alignment; no reserve
     * waiver while a peer-only owner has no decoder ledger. */
    uint32_t charge = sizeof(video_power_packet) + bytes + 8 + 16;
    if (!queue || bytes > VIDEO_POWER_BYTES ||
        !video_platform_heap_view(0, VIDEO_HEAP_CACHED, &view) ||
        view.max_alloc > view.free_bytes || view.free_bytes < VIDEO_CACHED_RESERVE + charge ||
        view.max_alloc < bytes + 8) return 0;
    video_power_packet *p = cfw_malloc(sizeof(*p));
    if (!p) return 0;
    p->payload = cfw_malloc(bytes + 8);
    if (!p->payload) { FW_FREE(p); return 0; }
    p->callback = 0; p->bytes = bytes + 8;
    p->kind = startup && VIDEO_POWER_HERE() == CFW_MESSAGE_RIGHT ? 2 : startup ? 0 : 1;
    uint8_t *d = p->payload;
    d[0] = startup ? 2 : 1; d[1] = startup ? 1 : 9;
    d[2] = startup ? 1 : CFW_MESSAGE_SID; d[3] = 0;
    d[4] = d[5] = 0; d[6] = bytes; d[7] = 0;
    if (bytes) memcpy(d + 8, body, bytes);
    /* Recheck after allocation before transferring ownership. */
    int room = video_platform_heap_view(0, VIDEO_HEAP_CACHED, &view) &&
        view.free_bytes >= VIDEO_CACHED_RESERVE;
    int sent = room && VIDEO_POWER_QUEUE_PUT(queue, &p, 0, 0) == 0;
    if (!sent) { FW_FREE(p->payload); FW_FREE(p); return 0; }
    if (!(startup && VIDEO_POWER_HERE() == CFW_MESSAGE_RIGHT)) {
        uint32_t event = *(const volatile uint32_t *)0x20076e6cU;
        if (!event || VIDEO_OS_EVENT_SET(event, 2) & VIDEO_EVENT_ERROR) return 0;
    }
    /* Once queued only the stock consumer may free either object. */
    return 1;
}
static int video_power_platform_start(void) { return video_power_platform_packet(0, 0, 1); }
static int video_power_platform_send(const uint8_t *p) {
    return video_power_platform_packet(p, VIDEO_POWER_BYTES, 0);
}
#define VIDEO_POWER_PLATFORM_START video_power_platform_start
#define VIDEO_POWER_PLATFORM_SEND video_power_platform_send
#endif

static uint32_t video_power_read(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void video_power_write(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = value >> (8 * i);
}
static int video_power_live(uint32_t deadline) {
    return deadline && (int32_t)(deadline - VIDEO_TICK) > 0;
}
static uint32_t video_power_limit(uint32_t limit) {
    uint32_t now = VIDEO_TICK, end = now + VIDEO_POWER_LEASE_MS;
    if (!end) end = 1;
    return (int32_t)(limit - end) < 0 ? limit : end;
}
static int video_power_owner_live(customCfwContext *ctx) {
    video_power_state *p = &ctx->video_power;
    return p->generation && video_control_generation_valid(ctx, p->generation) &&
        video_lease_valid(ctx);
}
static void video_power_encode(uint8_t *p, uint32_t op, uint32_t result,
                                uint32_t generation, uint32_t stream, uint32_t serial,
                                uint32_t duration, uint32_t nonce) {
    p[0] = VIDEO_POWER_BRIDGE; p[1] = VIDEO_POWER_HERE();
    p[2] = VIDEO_POWER_VERSION; p[3] = op; p[4] = result; p[5] = 0;
    video_power_write(p + 6, generation); video_power_write(p + 10, stream);
    video_power_write(p + 14, serial); video_power_write(p + 18, duration);
    video_power_write(p + 22, nonce);
}
static int video_power_send_owner(video_power_state *p, uint32_t op) {
    uint8_t record[VIDEO_POWER_BYTES];
    if (!p->serial || !p->generation) return 0;
    uint32_t now = VIDEO_TICK, duration = video_power_live(p->deadline) ? p->deadline - now : 0;
    video_power_encode(record, op, 0, p->generation, p->stream, p->serial, duration, p->nonce);
    return VIDEO_POWER_PLATFORM_SEND(record);
}
static void video_power_bootstrap(uint32_t app, const uint8_t *data, uint32_t size, uint16_t event) {
    if (app != VIDEO_MESSAGE_ID || data || !size || event) return;
    video_heap_view view;
    if (!video_platform_heap_view(0, VIDEO_HEAP_CACHED, &view) ||
        view.max_alloc > view.free_bytes ||
        view.free_bytes < VIDEO_CACHED_RESERVE + sizeof(customCfwContext) + 128) return;
    video_worker_ensure_mutex();
}
uint32_t video_power_received(const uint8_t *data, uint32_t size) {
    uint32_t here = VIDEO_POWER_HERE();
    if (!data || size != VIDEO_POWER_BYTES || data[0] != VIDEO_POWER_BRIDGE ||
        !here || (data[1] != CFW_MESSAGE_LEFT && data[1] != CFW_MESSAGE_RIGHT) ||
        data[2] != VIDEO_POWER_VERSION || data[3] < VIDEO_POWER_ACQUIRE ||
        data[3] > VIDEO_POWER_ACK || data[4] > VIDEO_POWER_REFUSED || data[5] ||
        !video_power_read(data + 6) || !video_power_read(data + 10) ||
        !video_power_read(data + 14) || !video_power_read(data + 22) ||
        video_power_read(data + 18) > VIDEO_POWER_LEASE_MS) return 0xau;
    if (data[1] == here) return 0;
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !ctx->image_mutex) {
        video_pool_item item = {video_power_bootstrap, VIDEO_MESSAGE_ID, 0, 0, 0,
                                video_power_read(data + 14)};
        VIDEO_POOL_DISPATCH(&item); return 6; /* Owner retries after task-context bootstrap. */
    }
    video_power_state *p = &ctx->video_power;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&p->mailbox_state, &expected, 1, 0,
                                      __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 6;
    for (uint32_t i = 0; i < size; ++i) p->mailbox[i] = data[i];
    __atomic_store_n(&p->mailbox_state, 2, __ATOMIC_RELEASE);
    return video_controller_request_locked(VIDEO_CONTROLLER_POWER) ? 0 : 6;
}
static void video_power_mail(customCfwContext *ctx) {
    video_power_state *p = &ctx->video_power;
    uint32_t expected = 2;
    if (!__atomic_compare_exchange_n(&p->mailbox_state, &expected, 3, 0,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    uint8_t record[VIDEO_POWER_BYTES];
    memcpy(record, p->mailbox, sizeof(record));
    __atomic_store_n(&p->mailbox_state, 0, __ATOMIC_RELEASE);
    uint32_t op = record[3], generation = video_power_read(record + 6);
    uint32_t stream = video_power_read(record + 10), serial = video_power_read(record + 14);
    uint32_t duration = video_power_read(record + 18), nonce = video_power_read(record + 22);
    if (op == VIDEO_POWER_ACK) {
        if (generation == p->generation && stream == p->stream && nonce == p->nonce &&
            serial == p->serial) {
            p->ack_serial = serial; p->ack_result = record[4];
            p->ack_deadline = VIDEO_TICK + duration;
        }
        return;
    }
    video_power_peer *peer = &p->peer;
    int same = generation == peer->generation && stream == peer->stream && nonce == peer->nonce;
    int fresh = generation > peer->generation && stream > peer->stream;
    uint32_t result = VIDEO_POWER_REFUSED;
    if (op == VIDEO_POWER_ACQUIRE && fresh && duration) {
        __atomic_store_n(&peer->deadline, 0, __ATOMIC_RELEASE);
        peer->generation = generation; peer->stream = stream; peer->nonce = nonce;
        peer->serial = serial; peer->closed = peer->requested = 0;
        __atomic_store_n(&peer->deadline, VIDEO_TICK + duration, __ATOMIC_RELEASE);
        peer->requested = VIDEO_POWER_PLATFORM_READY() || VIDEO_POWER_PLATFORM_START();
        if (!peer->requested) { peer->closed = 1; __atomic_store_n(&peer->deadline, 0, __ATOMIC_RELEASE); }
        same = 1;
    }
    if (same && serial >= peer->serial) {
        if (op == VIDEO_POWER_RELEASE) {
            peer->closed = 1; __atomic_store_n(&peer->deadline, 0, __ATOMIC_RELEASE);
            result = VIDEO_POWER_OK;
        } else if (!peer->closed && video_power_live(peer->deadline) && duration) {
            if (serial > peer->serial) __atomic_store_n(&peer->deadline, VIDEO_TICK + duration, __ATOMIC_RELEASE);
            result = VIDEO_POWER_PLATFORM_READY() ? VIDEO_POWER_OK :
                op == VIDEO_POWER_ACQUIRE && peer->requested ? VIDEO_POWER_WAIT : VIDEO_POWER_REFUSED;
        }
        peer->serial = serial;
    }
    uint32_t remaining = video_power_live(peer->deadline) ? peer->deadline - VIDEO_TICK : 0;
    video_power_encode(record, VIDEO_POWER_ACK, result, generation, stream, serial, remaining, nonce);
    VIDEO_POWER_PLATFORM_SEND(record);
}
uint32_t video_power_deadline(uint32_t generation) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !generation) return 0;
    video_power_state *p = &ctx->video_power;
    uint32_t local = __atomic_load_n(&p->deadline, __ATOMIC_ACQUIRE);
    uint32_t peer = __atomic_load_n(&p->peer_deadline, __ATOMIC_ACQUIRE);
    return p->generation == generation && p->acquired && video_power_live(local) &&
        video_power_live(peer) && VIDEO_POWER_PLATFORM_READY() ?
        ((int32_t)(local - peer) < 0 ? local : peer) : 0;
}
void video_power_release(uint32_t generation) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !generation || generation != ctx->video_power.generation) return;
    video_power_state *p = &ctx->video_power;
    __atomic_store_n(&p->deadline, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&p->peer_deadline, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&p->acquired, 0, __ATOMIC_RELEASE);
    if (p->serial != UINT32_MAX) { ++p->serial; video_power_send_owner(p, VIDEO_POWER_RELEASE); }
}
int video_power_acquire(uint32_t generation) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !generation || !video_control_generation_valid(ctx, generation)) return 0;
    video_power_state *p = &ctx->video_power;
    __atomic_store_n(&p->generation, generation, __ATOMIC_RELEASE);
    p->stream = ctx->video_control.stream;
    p->nonce = VIDEO_TICK ^ (generation * 0x9e3779b9U); if (!p->nonce) p->nonce = 1;
    p->serial = 1; p->ack_serial = 0;
    __atomic_store_n(&p->acquired, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&p->deadline, video_power_limit(ctx->video_control.active_deadline), __ATOMIC_RELEASE);
    if (!VIDEO_POWER_PLATFORM_READY() && !VIDEO_POWER_PLATFORM_START()) goto refused;
    uint32_t until = VIDEO_TICK + VIDEO_POWER_START_MS, retry = VIDEO_TICK;
    while (video_power_owner_live(ctx) && (int32_t)(until - VIDEO_TICK) > 0) {
        video_power_mail(ctx);
        if (p->ack_serial == p->serial && p->ack_result == VIDEO_POWER_REFUSED) goto refused;
        if (p->ack_serial == p->serial && p->ack_result == VIDEO_POWER_OK &&
            VIDEO_POWER_PLATFORM_READY() && video_power_live(p->ack_deadline)) {
            __atomic_store_n(&p->peer_deadline, p->ack_deadline, __ATOMIC_RELEASE);
            p->renewed = VIDEO_TICK;
            __atomic_store_n(&p->acquired, 1, __ATOMIC_RELEASE); return 1;
        }
        if ((int32_t)(VIDEO_TICK - retry) >= 0) {
            video_power_send_owner(p, VIDEO_POWER_ACQUIRE);
            retry = VIDEO_TICK + VIDEO_POWER_RETRY_MS;
        }
        VIDEO_OS_DELAY(10);
    }
refused:
    video_power_release(generation); return 0;
}
void video_power_step(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return;
    video_power_state *p = &ctx->video_power;
    video_power_mail(ctx);
    if (!p->acquired) return;
    if (!video_power_owner_live(ctx) || !video_power_deadline(p->generation) ||
        (p->ack_serial == p->serial && p->ack_result == VIDEO_POWER_REFUSED)) {
        if (video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) {
            if (ctx->video_control.start_guard == p->generation) {
                if (!ctx->video_control.error) ctx->video_control.error = VIDEO_CONTROL_POWER;
                video_control_cancel_locked();
            }
            video_control_give(ctx);
        }
        /* The terminating owner retains its finite lease until its copy and
         * task have parked. Refuse renewal, never release early while pinned. */
        return;
    }
    if (p->ack_serial == p->serial && p->ack_result == VIDEO_POWER_OK)
        __atomic_store_n(&p->peer_deadline, p->ack_deadline, __ATOMIC_RELEASE);
    if ((uint32_t)(VIDEO_TICK - p->renewed) >= VIDEO_POWER_RENEW_MS) {
        if (p->serial == UINT32_MAX) { video_power_release(p->generation); return; }
        ++p->serial; p->renewed = VIDEO_TICK;
        __atomic_store_n(&p->deadline, video_power_limit(ctx->video_control.active_deadline), __ATOMIC_RELEASE);
        video_power_send_owner(p, VIDEO_POWER_RENEW);
    }
}
uint32_t video_dashboard_idle_count(uint32_t count) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx && VIDEO_POWER_PLATFORM_READY()) {
        video_power_state *p = &ctx->video_power;
        int owner = __atomic_load_n(&p->generation, __ATOMIC_ACQUIRE) &&
            video_power_live(__atomic_load_n(&p->deadline, __ATOMIC_ACQUIRE));
        if ((owner || video_power_live(__atomic_load_n(&p->peer.deadline, __ATOMIC_ACQUIRE))) && count < 2)
            count = 2;
    }
    return count - 1;
}
#ifndef VIDEO_POWER_NATIVE
/* Replace only LDR/SUBS at the dashboard's automatic-idle decrement. The next
 * stock STR/LDR/CMP supplies all live flags; preserve other volatile registers. */
__attribute__((naked)) void video_dashboard_idle_gate(void) {
    __asm volatile("push {r0, r2-r4, r12, lr}\n\tldr r0, [r0]\n\t"
                   "bl video_dashboard_idle_count\n\tmov r1, r0\n\t"
                   "pop {r0, r2-r4, r12, pc}");
}
#endif
uint32_t video_ui_refresh(uint32_t app, uint32_t event, const void *data, uint32_t size) {
    uint32_t result = VIDEO_POWER_PLATFORM_REFRESH(app, event, data, size);
    customCfwContext *ctx = peekCustomCfwContext();
    if (ctx && event == 4) {
        video_power_state *p = &ctx->video_power;
        uint32_t now = VIDEO_TICK, last = __atomic_load_n(&p->signal_tick, __ATOMIC_ACQUIRE);
        if ((__atomic_load_n(&p->acquired, __ATOMIC_ACQUIRE) ||
             video_power_live(__atomic_load_n(&p->peer.deadline, __ATOMIC_ACQUIRE))) &&
            (uint32_t)(now - last) >= VIDEO_POWER_RENEW_MS &&
            __atomic_compare_exchange_n(&p->signal_tick, &last, now, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            video_controller_request_locked(VIDEO_CONTROLLER_POWER);
    }
    return result;
}

#pragma clang section text=""
