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
    entry->snapshot_bytes = entry->command[1] == VIDEO_CONTROL_STATUS ?
        VIDEO_STATUS_BYTES : VIDEO_BASE_STATUS_BYTES;
    p[0] = VIDEO_PROTOCOL_VERSION;
    p[1] = 7; /* Shared-shadow presentation with optional frame verification. */
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
    p[46] = 1; /* Chroma skipped. */
    p[47] = 1; /* Multiple slices per picture are not implemented. */
    p[48] = VIDEO_QUEUE_INITIAL; p[49] = VIDEO_QUEUE_MAX;
    video_queue_report queue;
    video_worker_queue_report_locked(&queue);
    p[50] = queue.capacity; p[51] = queue.credits;
    video_write32(p + 52, queue.expected);
    video_write32(p + 56, queue.accepted);
    video_write32(p + 60, queue.consumed);
    video_write32(p + 64, queue.pictures);
    p[68] = s->error >= VIDEO_CONTROL_GAP ? 2 : queue.gap_deadline ? 1 : 0;
    p[69] = queue.header_progress;
    p[70] = 7 | VIDEO_FEATURE_CREDITS | VIDEO_FEATURE_DECODE_TOTALS |
        (1u << VIDEO_STATUS_EXTENSION_SHIFT); /* Advertise one 64-byte STATUS extension. */
    p[71] = VIDEO_PRESENT_NATIVE | VIDEO_VERIFY_FRAMES;
    video_write32(p + 72, queue.gap_deadline);
    video_write32(p + 76, queue.gap_sequence);
    video_write32(p + 80, ctx->video_presentation.presented);
    video_write32(p + 84, ctx->video_presentation.failures);
    video_write32(p + 88, ctx->video_presentation.copy_tick);
    video_write32(p + 92, s->options);
    video_evidence_status_locked(ctx, p);
    if (entry->snapshot_bytes == VIDEO_STATUS_BYTES)
        video_decode_encode(&ctx->video_decode_last, p + VIDEO_BASE_STATUS_BYTES);
}

/* Credits are a frozen owner-bound snapshot, not an acknowledgement that a
 * peer accepted input. Both-target senders must account each lens separately. */
static void video_credit_snapshot(customCfwContext *ctx, video_control_replay *entry) {
    video_queue_report queue;
    video_worker_queue_report_locked(&queue);
    uint32_t state = video_lifecycle_state(&ctx->video);
    uint8_t *p = entry->snapshot;
    entry->snapshot_bytes = VIDEO_CREDITS_BYTES;
    video_write32(p, queue.expected);
    p[4] = (entry->result ? 0 : queue.credits) |
        (queue.capacity == VIDEO_QUEUE_MAX ? VIDEO_CREDIT_CAPACITY_SIX : 0) |
        (state << VIDEO_CREDIT_STATE_SHIFT) | (queue.gap_deadline ? VIDEO_CREDIT_GAP : 0);
    p[5] = entry->result ? entry->result : ctx->video_control.error;
}

static int video_control_apply(customCfwContext *ctx, const uint8_t *p,
                               uint32_t n, uint8_t here, uint8_t origin) {
    video_control_state *s = &ctx->video_control;
    uint8_t op = p[1];
    if (op == VIDEO_CONTROL_CREDITS) {
        if (n != VIDEO_CREDITS_REQUEST_BYTES) return VIDEO_CONTROL_FORMAT;
        uint32_t token = video_read32(p + (here == CFW_MESSAGE_LEFT ? 12 : 16));
        if (!token || token != ctx->video.token || video_read32(p + 8) != s->stream ||
            origin != s->owner_origin || !s->start_guard ||
            !video_control_generation_valid(ctx, s->start_guard)) return VIDEO_CONTROL_STALE;
        if (video_lifecycle_state(&ctx->video) != VIDEO_READY) return VIDEO_CONTROL_BUSY;
        uint32_t deadline = VIDEO_TICK + VIDEO_INACTIVITY_LIMIT_MS;
        __atomic_store_n(&s->active_deadline, deadline ? deadline : 1, __ATOMIC_RELEASE);
        video_worker_wake_locked(token, VIDEO_WAKE_INPUT);
        return VIDEO_CONTROL_ACCEPTED;
    }
    if (op == VIDEO_CONTROL_CAPABILITIES || op == VIDEO_CONTROL_STATUS) {
        if (n != VIDEO_CONTROL_HEADER_BYTES) return VIDEO_CONTROL_FORMAT;
        if (s->start_guard && origin == s->owner_origin) {
            if (!video_control_generation_valid(ctx, s->start_guard))
                video_fault_locked(ctx, VIDEO_FAULT_INACTIVITY);
            else {
                uint32_t deadline = VIDEO_TICK + VIDEO_INACTIVITY_LIMIT_MS;
                __atomic_store_n(&s->active_deadline, deadline ? deadline : 1, __ATOMIC_RELEASE);
                video_worker_wake_locked(ctx->video.token, VIDEO_WAKE_INPUT);
            }
        }
        return VIDEO_CONTROL_ACCEPTED;
    }
    if (op == VIDEO_CONTROL_START) {
        if (n != VIDEO_START_BYTES || p[12] != (VIDEO_FRAME_WIDTH & 255) ||
            p[13] != VIDEO_FRAME_WIDTH >> 8 || p[14] != VIDEO_FRAME_HEIGHT || p[15] ||
            p[16] != VIDEO_FRAME_REFERENCES || p[17] != VIDEO_FRAME_DPB ||
            p[18] & ~(VIDEO_PRESENT_NATIVE | VIDEO_VERIFY_FRAMES) || p[19])
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
        s->options = p[18];
        s->owner_origin = origin;
        uint32_t generation = s->control_generation + 1;
        __atomic_store_n(&s->control_generation, generation, __ATOMIC_RELEASE);
        __atomic_store_n(&s->start_guard, generation, __ATOMIC_RELEASE);
        uint32_t deadline = VIDEO_TICK + VIDEO_INACTIVITY_LIMIT_MS;
        __atomic_store_n(&s->active_deadline, deadline ? deadline : 1, __ATOMIC_RELEASE);
        s->error = 0;
        ctx->video_decode_last = (video_decode_totals){0};
        if (!video_controller_request_locked(VIDEO_CONTROLLER_START)) {
            __atomic_store_n(&s->start_guard, 0, __ATOMIC_RELEASE); s->error = VIDEO_CONTROL_DISPATCH;
            __atomic_fetch_and(&s->controller_reasons, ~VIDEO_CONTROLLER_START, __ATOMIC_ACQ_REL);
            return VIDEO_CONTROL_DISPATCH;
        }
        return VIDEO_CONTROL_ACCEPTED;
    }
    if (op == VIDEO_CONTROL_STOP || op == VIDEO_CONTROL_RESET) {
        if (n != 12) return VIDEO_CONTROL_FORMAT;
        if (!s->stream || video_read32(p + 8) != s->stream || origin != s->owner_origin)
            return VIDEO_CONTROL_STALE;
        __atomic_store_n(&s->start_guard, 0, __ATOMIC_RELEASE); /* Cancels even before a private owner is claimed. */
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
        __atomic_store_n(&s->start_guard, 0, __ATOMIC_RELEASE);
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
        /* A shorter PAGE resets the transport's inferred reply budget. Freeze
         * the stride to fit that query too, or its pages will be refused. */
        entry->capacity = route->reply_capacity < VIDEO_PAGE_REPLY_CAPACITY ?
            route->reply_capacity : VIDEO_PAGE_REPLY_CAPACITY;
        memcpy(entry->command, data, size);
        entry->result = video_control_apply(ctx, data, size, route->here, route->origin);
        video_snapshot(ctx, entry);
        if (data[1] == VIDEO_CONTROL_CREDITS) video_credit_snapshot(ctx, entry);
        if (data[1] == VIDEO_CONTROL_FRAME_READ || data[1] == VIDEO_CONTROL_FRAME_ACK) {
            bzero(entry->snapshot, VIDEO_DIAGNOSTICS_BYTES);
            entry->snapshot_bytes = data[1] == VIDEO_CONTROL_FRAME_READ ? VIDEO_FRAME_RESULT_BYTES : VIDEO_FRAME_ACK_BYTES;
            entry->result = size != 20 ? VIDEO_CONTROL_FORMAT : data[1] == VIDEO_CONTROL_FRAME_READ ?
                video_evidence_read_locked(ctx, data, route->origin, entry->snapshot) :
                video_evidence_ack_locked(ctx, data, route->origin, entry->snapshot);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = data[1];
            entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_DIAGNOSTICS && size == VIDEO_CONTROL_HEADER_BYTES) {
            entry->pending = 1;
            /* Heap walking and completed-worker sampling belong outside the
             * receive publication lock. Revalidate the reserved replay after
             * sampling: another request may have evicted it meanwhile. */
            video_control_give(ctx);
            uint8_t snapshot[VIDEO_DIAGNOSTICS_BYTES];
            int sampled = video_diagnostics_snapshot(snapshot);
            if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return -1;
            if (entry->request != request || !entry->pending) {
                video_control_give(ctx); return -1;
            }
            entry->pending = 0;
            entry->result = sampled ? VIDEO_CONTROL_ACCEPTED : VIDEO_CONTROL_BUSY;
            if (!sampled) bzero(snapshot, sizeof(snapshot));
            snapshot[0] = VIDEO_PROTOCOL_VERSION;
            snapshot[1] = VIDEO_CONTROL_DIAGNOSTICS;
            snapshot[2] = entry->result;
            if (video_read32(snapshot + 8) != ctx->video.generation) snapshot[3] |= 32;
            memcpy(entry->snapshot, snapshot, sizeof(snapshot));
            entry->snapshot_bytes = VIDEO_DIAGNOSTICS_BYTES;
        }
    }
    if (entry->pending) { video_control_give(ctx); return -1; }
    uint32_t count = entry->capacity - VIDEO_REPLY_HEADER_BYTES;
    uint32_t pages = (entry->snapshot_bytes + count - 1) / count;
    if (page >= pages || route->reply_capacity < entry->capacity) {
        video_control_give(ctx); return -1;
    }
    uint8_t reply[30] = {VIDEO_MESSAGE_ID, route->here};
    video_write32(reply + 2, request);
    reply[6] = page; reply[7] = pages;
    uint32_t offset = page * count;
    if (count > entry->snapshot_bytes - offset) count = entry->snapshot_bytes - offset;
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
