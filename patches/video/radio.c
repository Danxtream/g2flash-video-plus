/* Narrow Cordio adaptation. SPDX-License-Identifier: GPL-3.0-only */
#include "radio.h"
#include "../cfw_context.h"

#pragma clang section text=".text.video"

typedef struct {
    uint16_t tag;
    uint8_t event, status;
    uint32_t epoch, serial, permission;
    uint8_t connection, operation, tx, rx;
} video_radio_command;
_Static_assert(sizeof(video_radio_command) == 20, "copied PHY command ABI");

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

/* WSF uses fixed pools and short interrupt critical sections, not the stock
 * heap mutex. The stock message envelope is eight bytes. Leave at least
 * three free buffers in the allocator's first eligible pool. Keep
 * these address readers out of line for the ARM ROPI relocation contract. */
__attribute__((noinline)) static int video_radio_pool_room(uint32_t bytes) {
    const uint8_t *pools = (const void *)(uintptr_t)*(const volatile uint32_t *)0x20077238U;
    uint32_t count = *(const volatile uint8_t *)0x2007740aU;
    if (!pools || !count || count > 16) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t *pool = pools + i * 12;
        if (*(const uint16_t *)pool < bytes + 8) continue;
        const uint32_t *free = (const void *)(uintptr_t)*(const uint32_t *)(pool + 8);
        for (uint32_t n = 0; n < 4; ++n) {
            uintptr_t address = (uintptr_t)free;
            if (address < 0x20000000U || address >= 0x202a0000U || address & 3) return 0;
            free = (const void *)(uintptr_t)*free;
        }
        return 1;
    }
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
    if (video_radio_attach(&ctx->video_radio.links[id - 1], ccb))
        ctx->video_radio.links[id - 1].bound = 1;
    VIDEO_RADIO_UNLOCK();
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

int video_radio_request(uint8_t id, uint32_t epoch, uint8_t operation,
                        uint8_t tx, uint8_t rx, uint32_t permission) {
    customCfwContext *ctx = VIDEO_RADIO_CONTEXT();
    if (!ctx || !id || id > VIDEO_RADIO_LINKS || !epoch ||
        (operation != VIDEO_RADIO_READ && operation != VIDEO_RADIO_SET)) return 0;
    VIDEO_RADIO_LOCK();
    video_radio_state *s = &ctx->video_radio;
    video_radio_link *link = &s->links[id - 1];
    if (s->pending || s->serial == UINT32_MAX || !link->connected || !link->bound ||
        link->epoch != epoch || (operation == VIDEO_RADIO_SET &&
        (!permission || permission != link->permission || !tx || tx > 3 || !rx || rx > 3)) ||
        !VIDEO_RADIO_POOL_ROOM(sizeof(video_radio_command))) {
        VIDEO_RADIO_UNLOCK(); return 0;
    }
    video_radio_command *command = VIDEO_RADIO_ALLOC(sizeof(*command));
    if (!command) { VIDEO_RADIO_UNLOCK(); return 0; }
    *command = (video_radio_command){VIDEO_RADIO_COMMAND_TAG, VIDEO_RADIO_COMMAND_EVENT, 0,
        epoch, ++s->serial, permission, id, operation, tx, rx};
    s->connection = id; s->epoch = epoch; s->permission = permission;
    s->operation = operation; s->tx = tx; s->rx = rx;
    s->result = 1; s->pending = 1;
    VIDEO_RADIO_SEND(VIDEO_RADIO_HANDLER_ID(), command);
    VIDEO_RADIO_UNLOCK();
    return 1;
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
    if (!command->connection || command->connection > VIDEO_RADIO_LINKS) return;
    video_radio_link *link = &s->links[command->connection - 1];
    const uint8_t *ccb = video_radio_live(command->connection);
    /* This consumer runs on the same WSF task as DM events. Revalidate after
     * queueing, before entering the stock HCI queue, to reject reused IDs. */
    if (s->pending != 1 || command->serial != s->serial) return;
    if (!ccb || !link->connected || !link->bound || command->epoch != link->epoch ||
        link->handle != video_radio_u16(ccb + 12) ||
        (command->operation == VIDEO_RADIO_SET && (!command->permission ||
         command->permission != link->permission)) ||
        !VIDEO_RADIO_COMMAND_ROOM() ||
        !VIDEO_RADIO_POOL_ROOM(command->operation == VIDEO_RADIO_SET ? 10 : 5)) {
        s->result = 2; s->completed = s->serial; s->pending = 0; return;
    }
    s->pending = 2;
    if (command->operation == VIDEO_RADIO_SET) {
        /* A PHY value becomes a one-bit preference mask for Set PHY. */
        VIDEO_RADIO_SET_PHY(command->connection, 0, 1u << (command->tx - 1),
                            1u << (command->rx - 1), 0);
    } else {
        /* Read PHY's public wrapper was stripped from this donor. Its standard
         * command-completion parser and DM event route remain present. Use the
         * same bounded allocator/queue as the retained Set PHY wrapper. */
        uint8_t *packet = VIDEO_RADIO_CMD_ALLOC(0x2030, 2);
        if (!packet) { s->result = 2; s->completed = s->serial; s->pending = 0; return; }
        packet[3] = link->handle; packet[4] = link->handle >> 8;
        VIDEO_RADIO_CMD_SEND(packet);
    }
    /* The WSF dispatcher frees its copied message after this handler returns.
     * Stock HCI owns its command buffer after submission. */
}

static void video_radio_write32(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = value >> (8 * i);
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
