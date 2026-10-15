/* Narrow Cordio adaptation. SPDX-License-Identifier: GPL-3.0-only */
#include "radio.h"
#include "../cfw_context.h"

#pragma clang section text=".text.video"

typedef struct {
    uint16_t tag;
    uint8_t event, status;
    uint32_t serial;
} video_radio_command;
_Static_assert(sizeof(video_radio_command) == 8, "copied PHY command ABI");
_Static_assert(sizeof(video_radio_trace) == VIDEO_RADIO_TRACE_BYTES, "radio trace ABI");

#ifndef VIDEO_RADIO_CONTEXT
#define VIDEO_RADIO_CONTEXT peekCustomCfwContext
#define VIDEO_RADIO_HERE cfw_message_lens
#define VIDEO_RADIO_TICK FW_MS_TICK
#define VIDEO_RADIO_LOCK ((void (*)(void))0x00541841U)
#define VIDEO_RADIO_UNLOCK ((void (*)(void))0x00541849U)
#define VIDEO_RADIO_CCB ((const uint8_t *(*)(uint8_t))0x004ca71fU)
#define VIDEO_RADIO_STOCK_DM ((void (*)(void *))0x004c7a33U)
#define VIDEO_RADIO_STOCK_APP ((void (*)(uint8_t, void *))0x004c90e5U)
#define VIDEO_RADIO_ALLOC ((void *(*)(uint16_t))0x004d50b7U)
#define VIDEO_RADIO_SEND ((void (*)(uint8_t, void *))0x004d50d3U)
#define VIDEO_RADIO_CMD_ALLOC ((uint8_t *(*)(uint16_t, uint8_t))0x00540da1U)
#define VIDEO_RADIO_CMD_SEND ((void (*)(uint8_t *))0x00540dc7U)
#define VIDEO_RADIO_SET_PHY ((void (*)(uint8_t,uint8_t,uint8_t,uint8_t,uint16_t))0x004db279U)
#endif

#if !defined(VIDEO_RADIO_POOL_ROOM) || defined(VIDEO_RADIO_PLATFORM_TEST)

/* WSF skips empty eligible pools, but consumes the first nonempty class.
 * Preserve three buffers in that exact class, including the eight-byte
 * message envelope. Called under the existing WSF critical section. Keep
 * these address readers out of line for the ARM ROPI relocation contract. */
__attribute__((noinline)) static int video_radio_pool_room(uint32_t bytes, video_radio_trace *trace) {
    const uint8_t *pools = (const void *)(uintptr_t)*(const volatile uint32_t *)0x20077238U;
    uint32_t count = *(const volatile uint8_t *)0x2007740aU;
    uintptr_t base = (uintptr_t)pools;
    trace->reason = VIDEO_RADIO_REFUSAL_POOL_METADATA;
    if (!count || count > 16 || base < 0x20000000U || base & 3 ||
        base > 0x202a0000U - count * 12 || bytes > UINT16_MAX - 8) return 0;
    trace->required = bytes + 8;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *pool = pools + i * 12;
        uint16_t size = *(const uint16_t *)pool;
        if (size < trace->required) continue;
        const uint32_t *free = (const void *)(uintptr_t)*(const uint32_t *)(pool + 8);
        if (trace->first.index == UINT8_MAX)
            trace->first = (video_radio_pool_sample){i, 0, size};
        if (!free) { ++trace->skipped; continue; }
        trace->selected = (video_radio_pool_sample){i, 0, size};
        uintptr_t seen[VIDEO_RADIO_POOL_RESERVE + 1];
        for (uint32_t n = 0; n <= VIDEO_RADIO_POOL_RESERVE; ++n) {
            uintptr_t address = (uintptr_t)free;
            if (!address) { trace->reason = VIDEO_RADIO_REFUSAL_POOL_RESERVE; return 0; }
            int valid = address >= 0x20000000U && address <= 0x202a0000U - 4 && !(address & 3);
            for (uint32_t previous = 0; previous < n; ++previous)
                if (seen[previous] == address) valid = 0;
            if (!valid) {
                trace->selected.free = UINT8_MAX;
                if (trace->first.index == i) trace->first.free = UINT8_MAX;
                return 0;
            }
            seen[n] = address;
            trace->selected.free = n + 1;
            if (trace->first.index == i) trace->first.free = n + 1;
            if (n < VIDEO_RADIO_POOL_RESERVE) free = (const void *)(uintptr_t)*free;
        }
        trace->reason = VIDEO_RADIO_REFUSAL_NONE;
        return 1;
    }
    trace->reason = trace->first.index == UINT8_MAX ?
        VIDEO_RADIO_REFUSAL_POOL_ELIGIBLE : VIDEO_RADIO_REFUSAL_POOL_EMPTY;
    return 0;
}
__attribute__((noinline)) static uint8_t video_radio_handler_id(void) {
    return *(const volatile uint8_t *)0x20072ff2U;
}
__attribute__((noinline)) static int video_radio_command_room(void) {
    /* HciCmdSend keeps a bare handle if queued behind another command. Submit
     * only to an empty queue with a credit and a free transport FIFO slot. */
    return *(const volatile uint8_t *)0x20075b62U &&
        !*(const volatile uint32_t *)0x20075b58U &&
        *(const volatile uint32_t *)0x20075c68U <
        *(const volatile uint32_t *)0x20075c6cU;
}
#ifndef VIDEO_RADIO_POOL_ROOM
#define VIDEO_RADIO_POOL_ROOM video_radio_pool_room
#define VIDEO_RADIO_HANDLER_ID video_radio_handler_id
#define VIDEO_RADIO_COMMAND_ROOM video_radio_command_room
#endif
#endif

static uint16_t video_radio_u16(const uint8_t *p) {
    return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}
static const uint8_t *video_radio_live(uint8_t id) {
    if (!id || id > VIDEO_RADIO_LINKS) return 0;
    const uint8_t *ccb = VIDEO_RADIO_CCB(id);
    /* Connection ID, active flag and peripheral role are authenticated by the
     * donor guard. Role is at +25; +19 is peer address type, not role.
     * An EUS write binds a host link, never the UART bridge. */
    return ccb && ccb[16] == id && ccb[22] && ccb[25] == 1 ? ccb : 0;
}
static int video_radio_attach(video_radio_link *link, const uint8_t *ccb) {
    uint16_t handle = video_radio_u16(ccb + 12);
    if (link->connected && link->handle == handle) return 1;
    if (link->epoch == UINT32_MAX) return 0;
    uint32_t epoch = link->epoch + 1;
    *link = (video_radio_link){.epoch = epoch, .handle = handle, .connected = 1};
    return 1;
}

void video_radio_bind(uint8_t id) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx) return; /* Ordinary stock traffic never creates a context. */
    const uint8_t *ccb = video_radio_live(id);
    if (!ccb) return;
    VIDEO_RADIO_LOCK();
    if (video_radio_attach(&ctx->video_radio.links[id - 1], ccb)) {
        ctx->video_radio.links[id - 1].bound = 1;
        video_radio_state *s = &ctx->video_radio;
        if (s->transport_connection != id || s->transport_link_epoch != s->links[id - 1].epoch) {
            uint32_t epoch = __atomic_load_n(&s->transport_epoch, __ATOMIC_RELAXED);
            if (epoch != UINT32_MAX) __atomic_store_n(&s->transport_epoch, epoch + 1, __ATOMIC_RELEASE);
            s->transport_connection = id; s->transport_link_epoch = s->links[id - 1].epoch;
        }
    }
    VIDEO_RADIO_UNLOCK();
}

uint32_t video_radio_transport_epoch(void) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    uint32_t epoch = ctx ? __atomic_load_n(&ctx->video_radio.transport_epoch, __ATOMIC_ACQUIRE) : 0;
    return epoch == UINT32_MAX ? 0 : epoch;
}

#ifndef VIDEO_RADIO_NOTIFY
#define VIDEO_RADIO_NOTIFY video_control_notify_radio
#endif

void video_radio_dm_event(void *event) {
    const uint8_t *data = event;
    int notify = 0;
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (ctx && data) {
        uint16_t id = video_radio_u16(data);
        if (id && id <= VIDEO_RADIO_LINKS) {
            video_radio_state *s = &ctx->video_radio;
            video_radio_link *link = &s->links[id - 1];
            VIDEO_RADIO_LOCK();
            if (data[2] == VIDEO_RADIO_DM_OPEN && !data[3]) {
                const uint8_t *ccb = video_radio_live(id);
                if (ccb) video_radio_attach(link, ccb);
            } else if (data[2] == VIDEO_RADIO_DM_CLOSE) {
                if (s->transport_connection == id) {
                    uint32_t epoch = __atomic_load_n(&s->transport_epoch, __ATOMIC_RELAXED);
                    if (epoch != UINT32_MAX) __atomic_store_n(&s->transport_epoch, epoch + 1, __ATOMIC_RELEASE);
                    s->transport_connection = 0; s->transport_link_epoch = 0;
                }
                link->connected = link->bound = link->valid = 0;
                link->permission = 0;
                link->before.valid = link->after.valid = 0;
                if (s->connection == id) s->pending = 0;
            } else if ((data[2] == VIDEO_RADIO_DM_READ || data[2] == VIDEO_RADIO_DM_UPDATE) &&
                       link->connected && link->handle == video_radio_u16(data + 6)) {
                link->status = data[4]; link->tick = VIDEO_RADIO_TICK;
                link->valid = !data[4] && data[8] >= VIDEO_RADIO_PHY_1M &&
                    data[8] <= VIDEO_RADIO_PHY_CODED && data[9] >= VIDEO_RADIO_PHY_1M &&
                    data[9] <= VIDEO_RADIO_PHY_CODED;
                link->tx = data[8]; link->rx = data[9];
                if (link->sample < UINT32_MAX) ++link->sample;
                if (s->pending == 2 && s->connection == id && s->epoch == link->epoch &&
                    ((s->operation == VIDEO_RADIO_READ && data[2] == VIDEO_RADIO_DM_READ) ||
                     (s->operation == VIDEO_RADIO_SET && data[2] == VIDEO_RADIO_DM_UPDATE))) {
                    s->result = link->valid ? 0 : 2;
                    s->completed = s->serial; s->pending = 0;
                }
            }
            notify = link->owners || link->permission || link->uncertain;
            for (uint32_t i = 0; i < VIDEO_RADIO_OWNERS; ++i)
                if (s->owners[i].connection == id) notify = 1;
            VIDEO_RADIO_UNLOCK();
        }
    }
    VIDEO_RADIO_STOCK_DM(event);
    if (notify) VIDEO_RADIO_NOTIFY();
}

static video_radio_trace video_radio_attempt(uint32_t attempt, uint8_t id,
                                             uint32_t epoch, uint8_t operation, uint8_t site) {
    return (video_radio_trace){.attempt = attempt, .tick = VIDEO_RADIO_TICK,
        .epoch = epoch, .connection = id, .operation = operation, .site = site,
        .first = {UINT8_MAX, UINT8_MAX, 0}, .selected = {UINT8_MAX, UINT8_MAX, 0}};
}
static void video_radio_record(video_radio_state *s, const video_radio_trace *trace) {
    s->last_attempt = *trace;
    if (trace->reason) s->last_refusal = *trace;
}

int video_radio_request(uint8_t id, uint32_t epoch, uint8_t operation,
                        uint8_t tx, uint8_t rx, uint32_t permission) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx) return 0;
    VIDEO_RADIO_LOCK();
    video_radio_state *s = &ctx->video_radio;
    if (s->attempt_counter < UINT32_MAX) ++s->attempt_counter;
    video_radio_trace trace = video_radio_attempt(s->attempt_counter, id, epoch,
                                                  operation, VIDEO_RADIO_REQUEST_SITE);
    if (!id || id > VIDEO_RADIO_LINKS || !epoch ||
        (operation != VIDEO_RADIO_READ && operation != VIDEO_RADIO_SET)) {
        trace.reason = VIDEO_RADIO_REFUSAL_ARGUMENT;
        goto refused;
    }
    video_radio_link *link = &s->links[id - 1];
    if (s->pending) trace.reason = VIDEO_RADIO_REFUSAL_BUSY;
    else if (s->serial == UINT32_MAX) trace.reason = VIDEO_RADIO_REFUSAL_SERIAL;
    else if (!link->connected || !link->bound) trace.reason = VIDEO_RADIO_REFUSAL_LINK;
    else if (link->epoch != epoch) trace.reason = VIDEO_RADIO_REFUSAL_EPOCH;
    else if (operation == VIDEO_RADIO_SET && (!permission || permission != link->permission ||
             !tx || tx > 3 || !rx || rx > 3)) trace.reason = VIDEO_RADIO_REFUSAL_PERMISSION;
    if (trace.reason || !VIDEO_RADIO_POOL_ROOM(sizeof(video_radio_command), &trace)) goto refused;
    video_radio_command *command = VIDEO_RADIO_ALLOC(sizeof(*command));
    if (!command) { trace.reason = VIDEO_RADIO_REFUSAL_ALLOC; goto refused; }
    *command = (video_radio_command){VIDEO_RADIO_COMMAND_TAG, VIDEO_RADIO_COMMAND_EVENT, 0,
        ++s->serial};
    s->connection = id; s->epoch = epoch; s->permission = permission;
    s->operation = operation; s->tx = tx; s->rx = rx;
    s->result = 1; s->pending = 1;
    s->copied_attempt = trace.attempt;
    video_radio_record(s, &trace);
    VIDEO_RADIO_SEND(VIDEO_RADIO_HANDLER_ID(), command);
    VIDEO_RADIO_UNLOCK();
    return 1;
refused:
    video_radio_record(s, &trace);
    VIDEO_RADIO_UNLOCK();
    return 0;
}

void video_radio_app_handler(uint8_t events, void *message) {
    video_radio_command *command = message;
    if (!command || command->event != VIDEO_RADIO_COMMAND_EVENT ||
        command->tag != VIDEO_RADIO_COMMAND_TAG) {
        VIDEO_RADIO_STOCK_APP(events, message); return;
    }
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx) return;
    video_radio_state *s = &ctx->video_radio;
    /* One pending record owns the immutable command fields. Match its full
     * serial before reading them: stale mail cannot borrow a newer request,
     * while eight-byte mail can use a smaller pool class than Set PHY. */
    VIDEO_RADIO_LOCK();
    if (s->pending != 1 || command->serial != s->serial ||
        !s->connection || s->connection > VIDEO_RADIO_LINKS) {
        VIDEO_RADIO_UNLOCK(); return;
    }
    video_radio_link *link = &s->links[s->connection - 1];
    const uint8_t *ccb = video_radio_live(s->connection);
    /* Match the copied request's attempt identity, even if another caller
     * recorded a busy refusal while this mail was queued. */
    video_radio_trace trace = video_radio_attempt(s->copied_attempt, s->connection,
        s->epoch, s->operation, VIDEO_RADIO_CONSUMER_SITE);
    if (!ccb || !link->connected || !link->bound || s->epoch != link->epoch ||
        link->handle != video_radio_u16(ccb + 12) ||
         (s->operation == VIDEO_RADIO_SET && (!s->permission ||
          s->permission != link->permission))) trace.reason = VIDEO_RADIO_REFUSAL_COMMAND_LINK;
    else if (!VIDEO_RADIO_COMMAND_ROOM()) trace.reason = VIDEO_RADIO_REFUSAL_COMMAND_CREDIT;
    if (!trace.reason) VIDEO_RADIO_POOL_ROOM(s->operation == VIDEO_RADIO_SET ? 10 : 5, &trace);
    video_radio_record(s, &trace);
    if (trace.reason) {
        s->result = 2; s->completed = s->serial; s->pending = 0;
        VIDEO_RADIO_UNLOCK(); return;
    }
    s->pending = 2;
    /* Keep selection and allocation indivisible with respect to WSF pool
     * users. The retained SET wrapper only allocates/copies/posts bounded
     * stock commands; its nested WSF sections have no heap mutex or wait. */
    if (s->operation == VIDEO_RADIO_SET) {
        /* A PHY value becomes a one-bit preference mask for Set PHY. */
        VIDEO_RADIO_SET_PHY(s->connection, 0, 1u << (s->tx - 1),
                             1u << (s->rx - 1), 0);
        VIDEO_RADIO_UNLOCK();
    } else {
        /* Read PHY's public wrapper was stripped from this donor. Its standard
         * command-completion parser and DM event route remain present. Use the
         * same bounded allocator/queue as the retained Set PHY wrapper. */
        uint8_t *packet = VIDEO_RADIO_CMD_ALLOC(0x2030, 2);
        if (!packet) {
            trace.reason = VIDEO_RADIO_REFUSAL_COMMAND_ALLOC;
            video_radio_record(s, &trace);
            s->result = 2; s->completed = s->serial; s->pending = 0;
            VIDEO_RADIO_UNLOCK(); return;
        }
        packet[3] = link->handle; packet[4] = link->handle >> 8;
        VIDEO_RADIO_UNLOCK();
        VIDEO_RADIO_CMD_SEND(packet);
    }
    /* The WSF dispatcher frees its copied message after this handler returns.
     * Stock HCI owns its command buffer after submission. */
}

static void video_radio_write32(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = value >> (8 * i);
}
static void video_radio_trace_encode(uint8_t *p, const video_radio_trace *trace) {
    video_radio_write32(p, trace->attempt);
    video_radio_write32(p + 4, trace->tick); video_radio_write32(p + 8, trace->epoch);
    p[12] = trace->connection; p[13] = trace->operation;
    p[14] = trace->site; p[15] = trace->reason;
    p[16] = trace->required; p[17] = trace->required >> 8; p[18] = trace->skipped;
    p[20] = trace->first.index; p[21] = trace->first.free;
    p[22] = trace->first.size; p[23] = trace->first.size >> 8;
    p[24] = trace->selected.index; p[25] = trace->selected.free;
    p[26] = trace->selected.size; p[27] = trace->selected.size >> 8;
}
int video_radio_refusal_snapshot(uint8_t out[VIDEO_RADIO_REPORT_BYTES]) {
    if (!out) return 0;
    for (uint32_t i = 0; i < VIDEO_RADIO_REPORT_BYTES; ++i) out[i] = 0;
    out[0] = VIDEO_PROTOCOL_VERSION; out[1] = VIDEO_CONTROL_DIAGNOSTICS;
    out[3] = VIDEO_RADIO_REFUSAL_DIAGNOSTICS;
    out[8] = VIDEO_RADIO_REPORT_VERSION; out[9] = VIDEO_RADIO_TRACE_BYTES;
    out[10] = 2; out[11] = VIDEO_RADIO_HERE();
    out[13] = VIDEO_RADIO_POOL_RESERVE; out[14] = 1; /* Empty-only donor fallback. */
    VIDEO_RADIO_LOCK();
    video_radio_write32(out + 4, VIDEO_RADIO_TICK);
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (ctx) {
        video_radio_state *s = &ctx->video_radio;
        out[12] = 1;
        video_radio_write32(out + 16, s->attempt_counter);
        video_radio_write32(out + 20, s->serial); video_radio_write32(out + 24, s->completed);
        video_radio_write32(out + 28, s->epoch);
        out[32] = s->pending; out[33] = s->result;
        out[34] = s->connection; out[35] = s->operation;
        video_radio_trace_encode(out + 40, &s->last_attempt);
        video_radio_trace_encode(out + 72, &s->last_refusal);
    }
    VIDEO_RADIO_UNLOCK();
    return 1;
}
int video_radio_snapshot(uint8_t out[VIDEO_RADIO_REPORT_BYTES]) {
    if (!out) return 0;
    for (uint32_t i = 0; i < VIDEO_RADIO_REPORT_BYTES; ++i) out[i] = 0;
    out[0] = VIDEO_PROTOCOL_VERSION; out[1] = VIDEO_CONTROL_DIAGNOSTICS; out[3] = VIDEO_RADIO_DIAGNOSTICS;
    video_radio_write32(out + 4, VIDEO_RADIO_TICK);
    out[8] = VIDEO_RADIO_REPORT_VERSION; out[9] = VIDEO_RADIO_RECORD_BYTES;
    out[10] = VIDEO_RADIO_LINKS; out[11] = VIDEO_RADIO_HERE();
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    uint8_t observe = 0; uint32_t epoch = 0;
    VIDEO_RADIO_LOCK();
    for (uint32_t i = 0; i < VIDEO_RADIO_LINKS; ++i) {
        uint8_t *p = out + 12 + i * VIDEO_RADIO_RECORD_BYTES;
        p[0] = i + 1;
        if (!ctx) continue;
        video_radio_state *s = &ctx->video_radio;
        video_radio_link *link = &s->links[i];
        p[1] = link->connected | (link->bound << 1) | (link->valid << 2) |
            (link->before.valid << 3) | (link->after.valid << 4) |
            (!!link->owners << 5) | ((s->pending && s->connection == i + 1) << 6) |
            (!!link->uncertain << 7);
        p[2] = link->phase; p[3] = link->status;
        video_radio_write32(p + 4, link->epoch);
        video_radio_write32(p + 8, link->tick);
        video_radio_write32(p + 12, link->before.tick);
        video_radio_write32(p + 16, link->after.tick);
        p[20] = link->before.tx; p[21] = link->before.rx;
        p[22] = link->tx; p[23] = link->rx;
        p[24] = link->after.tx; p[25] = link->after.rx;
        p[26] = link->owners;
        p[27] = s->connection == i + 1 && s->pending ? s->operation : 0;
        video_radio_write32(p + 28, link->permission);
    }
    if (ctx) {
        video_radio_state *s = &ctx->video_radio;
        video_radio_write32(out + 108, s->serial);
        video_radio_write32(out + 112, s->completed);
        out[116] = s->pending; out[117] = s->result; out[118] = s->connection;
        video_radio_write32(out + 120, s->epoch);
        for (uint32_t n = 0; n < VIDEO_RADIO_LINKS && !s->pending; ++n) {
            uint32_t i = (s->observe_next + n) % VIDEO_RADIO_LINKS;
            video_radio_link *link = &s->links[i];
            if (link->connected && link->bound && !link->owners && !link->uncertain) {
                observe = i + 1; epoch = link->epoch; s->observe_next = (i + 1) % VIDEO_RADIO_LINKS;
                break;
            }
        }
    }
    VIDEO_RADIO_UNLOCK();
    if (observe) video_radio_request(observe, epoch, VIDEO_RADIO_READ, 0, 0, 0);
    return 1;
}

#pragma clang section text=""
