/* SPDX-License-Identifier: GPL-3.0-only */
#include "control.h"
#include "worker.h"
#include "queue.h"
#include "../cfw_context.h"

#pragma clang section text=".text.video"

static uint32_t video_read32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static void video_write32(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = value >> (i * 8);
}
static int video_route_valid(const cfw_message_route *r) {
    return r && (r->here == CFW_MESSAGE_LEFT || r->here == CFW_MESSAGE_RIGHT) &&
        (r->origin == CFW_MESSAGE_LEFT || r->origin == CFW_MESSAGE_RIGHT) &&
        !(r->targets & ~CFW_MESSAGE_BOTH) && (r->targets & r->here) &&
        r->reply_capacity > VIDEO_REPLY_HEADER_BYTES && r->reply_capacity <= 30;
}

static void video_snapshot(customCfwContext *ctx, video_control_replay *entry) {
    video_control_state *s = &ctx->video_control;
    uint8_t *p = entry->snapshot;
    bzero(p, VIDEO_STATUS_BYTES);
    p[0] = VIDEO_PROTOCOL_VERSION;
    p[1] = 3; /* Ordered NAL ingress; no live consumer or presentation. */
    p[2] = entry->result;
    p[3] = video_lifecycle_state(&ctx->video);
    if (p[3] == VIDEO_IDLE && s->start_guard) p[3] = VIDEO_STARTING;
    video_write32(p + 4, s->stream);
    video_write32(p + 8, ctx->video.generation);
    video_write32(p + 12, s->request_high[0]);
    video_write32(p + 16, s->request_high[1]);
    video_write32(p + 20, s->stream_high);
    video_write32(p + 24, s->error);
    video_write32(p + 28, s->interval);
    video_write32(p + 32, VIDEO_RECORD_LIMIT);
    video_write32(p + 36, VIDEO_NAL_LIMIT);
    p[40] = VIDEO_FRAME_WIDTH & 255; p[41] = VIDEO_FRAME_WIDTH >> 8;
    p[42] = VIDEO_FRAME_HEIGHT & 255; p[43] = VIDEO_FRAME_HEIGHT >> 8;
    p[44] = VIDEO_FRAME_REFERENCES; p[45] = VIDEO_FRAME_DPB;
    p[46] = 1; /* Chroma skipped; completed pictures remain zero. */
    p[47] = 1; /* Multiple slices per picture are not implemented. */
    p[48] = VIDEO_QUEUE_INITIAL; p[49] = VIDEO_QUEUE_MAX;
    video_queue_report queue;
    video_worker_queue_report_locked(&queue);
    p[50] = queue.capacity; p[51] = queue.credits;
    video_write32(p + 52, queue.expected);
    video_write32(p + 56, queue.accepted);
    video_write32(p + 60, queue.consumed);
    video_write32(p + 64, queue.pictures);
}

static int video_control_apply(customCfwContext *ctx, const uint8_t *p,
                               uint32_t n, uint8_t origin) {
    video_control_state *s = &ctx->video_control;
    uint8_t op = p[1];
    if (op == VIDEO_CONTROL_CAPABILITIES || op == VIDEO_CONTROL_STATUS)
        return n == VIDEO_CONTROL_HEADER_BYTES ? VIDEO_CONTROL_ACCEPTED : VIDEO_CONTROL_FORMAT;
    if (op == VIDEO_CONTROL_START) {
        if (n != VIDEO_START_BYTES || p[12] != (VIDEO_FRAME_WIDTH & 255) ||
            p[13] != VIDEO_FRAME_WIDTH >> 8 || p[14] != VIDEO_FRAME_HEIGHT || p[15] ||
            p[16] != VIDEO_FRAME_REFERENCES || p[17] != VIDEO_FRAME_DPB || p[18] || p[19])
            return VIDEO_CONTROL_FORMAT;
        uint32_t stream = video_read32(p + 8), interval = video_read32(p + 20);
        if (!stream || stream <= s->stream_high || interval < VIDEO_INTERVAL_MIN ||
            interval > VIDEO_INTERVAL_MAX) return VIDEO_CONTROL_FORMAT;
        if (ctx->texture_cache || !video_lease_valid(ctx)) return VIDEO_CONTROL_LEASE;
        if (s->start_guard || s->controller_job || video_lifecycle_state(&ctx->video) != VIDEO_IDLE)
            return VIDEO_CONTROL_BUSY;
        if (s->control_generation == UINT32_MAX) return VIDEO_CONTROL_STALE;
        s->stream = s->stream_high = stream;
        s->interval = interval;
        s->owner_origin = origin;
        s->start_guard = ++s->control_generation;
        s->error = 0;
        if (!video_controller_request_locked(VIDEO_CONTROLLER_START)) {
            s->start_guard = 0; s->error = VIDEO_CONTROL_DISPATCH;
            __atomic_fetch_and(&s->controller_reasons, ~VIDEO_CONTROLLER_START, __ATOMIC_ACQ_REL);
            return VIDEO_CONTROL_DISPATCH;
        }
        return VIDEO_CONTROL_ACCEPTED;
    }
    if (op == VIDEO_CONTROL_STOP || op == VIDEO_CONTROL_RESET) {
        if (n != 12) return VIDEO_CONTROL_FORMAT;
        if (!s->stream || video_read32(p + 8) != s->stream || origin != s->owner_origin)
            return VIDEO_CONTROL_STALE;
        s->start_guard = 0; /* Cancels even before a private owner is claimed. */
        video_worker_request_stop_locked();
        if (!video_controller_request_locked(VIDEO_CONTROLLER_STOP)) {
            s->error = VIDEO_CONTROL_DISPATCH;
            if (ctx->video.token)
                __atomic_store_n(&ctx->video_quarantine_token, ctx->video.token, __ATOMIC_RELEASE);
            return VIDEO_CONTROL_DISPATCH;
        }
        return VIDEO_CONTROL_ACCEPTED;
    }
    return VIDEO_CONTROL_FORMAT;
}

int video_control_received(const uint8_t *data, uint16_t size,
                            const cfw_message_route *route) {
    if (data && size >= 2 && data[0] == VIDEO_MESSAGE_ID &&
        data[1] == VIDEO_CONTROL_NAL) {
        if (size <= VIDEO_NAL_HEADER_BYTES || size > VIDEO_RECORD_LIMIT ||
            !video_route_valid(route)) return -1;
        customCfwContext *ctx = peekCustomCfwContext();
        if (!ctx || !ctx->image_mutex || !video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return -1;
        video_fold_signals(ctx);
        int copied = video_worker_receive_nal_locked(video_read32(data + 2),
            video_read32(data + 6), data + VIDEO_NAL_HEADER_BYTES,
            size - VIDEO_NAL_HEADER_BYTES, route->origin);
        video_control_give(ctx);
        return copied > 0 ? 0 : -1;
    }
    if (!data || size < VIDEO_CONTROL_HEADER_BYTES || data[0] != VIDEO_MESSAGE_ID ||
        data[2] != VIDEO_PROTOCOL_VERSION || data[3] || !video_route_valid(route)) return -1;
    uint32_t request = video_read32(data + 4);
    if (!request) return -1;
    if (!video_worker_ensure_mutex()) return -1;
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !ctx->image_mutex || !video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return -1;
    video_fold_signals(ctx);
    video_control_state *s = &ctx->video_control;
    if (__atomic_exchange_n(&s->controller_failed, 0, __ATOMIC_ACQ_REL)) {
        s->start_guard = 0;
        s->error = VIDEO_CONTROL_DISPATCH;
        if (ctx->video.state != VIDEO_IDLE) video_lifecycle_quarantine(&ctx->video);
    }
    uint32_t origin = route->origin - 1;
    video_control_replay *entry = 0;
    for (uint32_t i = 0; i < VIDEO_CONTROL_REPLAYS; ++i)
        if (s->replay[origin][i].request == request) entry = &s->replay[origin][i];
    uint32_t page = 0;
    if (data[1] == VIDEO_CONTROL_PAGE) {
        if (size != VIDEO_CONTROL_HEADER_BYTES + 1 || !entry) {
            video_control_give(ctx); return -1;
        }
        page = data[8];
    } else if (entry) {
        int equal = size == entry->length;
        for (uint32_t i = 0; equal && i < size; ++i) equal = data[i] == entry->command[i];
        if (!equal) {
            video_control_give(ctx); return -1;
        }
    } else {
        if (request <= s->request_high[origin] || size > VIDEO_START_BYTES) {
            video_control_give(ctx); return -1;
        }
        entry = &s->replay[origin][s->replay_next[origin]];
        s->replay_next[origin] = (s->replay_next[origin] + 1) % VIDEO_CONTROL_REPLAYS;
        *entry = (video_control_replay){0};
        entry->request = s->request_high[origin] = request;
        entry->length = size;
        entry->capacity = route->reply_capacity;
        memcpy(entry->command, data, size);
        entry->result = video_control_apply(ctx, data, size, route->origin);
        video_snapshot(ctx, entry);
    }
    uint32_t count = entry->capacity - VIDEO_REPLY_HEADER_BYTES;
    uint32_t pages = (VIDEO_STATUS_BYTES + count - 1) / count;
    if (page >= pages || route->reply_capacity < entry->capacity) {
        video_control_give(ctx); return -1;
    }
    uint8_t reply[30] = {VIDEO_MESSAGE_ID, route->here};
    video_write32(reply + 2, request);
    reply[6] = page; reply[7] = pages;
    uint32_t offset = page * count;
    if (count > VIDEO_STATUS_BYTES - offset) count = VIDEO_STATUS_BYTES - offset;
    memcpy(reply + VIDEO_REPLY_HEADER_BYTES, entry->snapshot + offset, count);
    int accepted = data[1] == VIDEO_CONTROL_PAGE || entry->result == VIDEO_CONTROL_ACCEPTED;
    video_control_give(ctx);
    /* Both transport send services copy before return; no owner pointer is
     * retained and this stack reply remains valid through the enqueue. */
    int sent = cfw_message_video_reply(route, reply, VIDEO_REPLY_HEADER_BYTES + count);
    return !sent && accepted ? 0 : -1;
}

int video_control_blocks_custom(const uint8_t *p, uint32_t n) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !p || !n || (!ctx->video_control.start_guard &&
        ctx->video.state == VIDEO_IDLE)) return 0;
    uint8_t mode = p[0] & 0x7f;
    return mode == 3 || mode == 6 || mode == 8 || mode == 9 || mode == 15 ||
           mode == 18 || mode == 19 || mode == 20;
}

int video_transport_room(uint32_t bytes) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || (!__atomic_load_n(&ctx->video_control.start_guard, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&ctx->video.state, __ATOMIC_ACQUIRE) == VIDEO_IDLE)) return 1;
    if (bytes > UINT32_MAX - 7u) return 0;
    uint32_t charge = bytes ? ((bytes + 3u) & ~3u) + 4u : 0;
    uint32_t keep = VIDEO_DISPLAY_RESERVE;
    if (!__atomic_load_n(&ctx->framebuffer_shadow, __ATOMIC_ACQUIRE)) keep += VIDEO_SHADOW_BYTES;
    video_heap_view view;
    return video_platform_heap_view(0, VIDEO_HEAP_DISPLAY, &view) &&
        view.max_alloc <= view.free_bytes && view.free_bytes != UINT32_MAX &&
        view.free_bytes >= keep && charge <= view.free_bytes - keep &&
        (!bytes || bytes <= view.max_alloc);
}

#pragma clang section text=""
