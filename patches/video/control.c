/* SPDX-License-Identifier: GPL-3.0-only */
#include "control.h"
#include "worker.h"
#include "queue.h"
#include "../cfw_context.h"

#ifndef VIDEO_RADIO_SNAPSHOT
#define VIDEO_RADIO_SNAPSHOT video_radio_snapshot
#endif
#ifndef VIDEO_RADIO_REFUSAL_SNAPSHOT
#define VIDEO_RADIO_REFUSAL_SNAPSHOT video_radio_refusal_snapshot
#endif

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
        r->reply_capacity > VIDEO_REPLY_HEADER_BYTES && r->reply_capacity <= VIDEO_PAGE_REPLY_CAPACITY;
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
    if (p[3] == VIDEO_IDLE && s->stop_generation) p[3] = VIDEO_STOPPING;
    else if (p[3] == VIDEO_IDLE && s->start_guard) p[3] = VIDEO_STARTING;
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
    p[47] = 1; /* Legacy single-slice byte kept for older apps; playback capability byte 7 bit 3 advertises multi-slice pictures. */
    p[48] = VIDEO_QUEUE_INITIAL; p[49] = VIDEO_QUEUE_MAX;
    video_queue_report queue;
    video_worker_queue_report_locked(&queue);
    p[50] = queue.capacity; p[51] = queue.credits;
    video_write32(p + 52, queue.expected);
    video_write32(p + 56, queue.accepted);
    video_write32(p + 60, queue.consumed);
    video_write32(p + 64, queue.pictures);
    p[68] = s->error >= VIDEO_CONTROL_GAP ? 2 : queue.gap_deadline ? 1 : 0;
    p[69] = (queue.header_progress & VIDEO_POWER_HEADER_MASK) | (__atomic_load_n(&ctx->video_power.acquire_step,
        __ATOMIC_ACQUIRE) << VIDEO_POWER_STEP_SHIFT);
    p[70] = 7 | VIDEO_FEATURE_CREDITS | VIDEO_FEATURE_DECODE_TOTALS |
        (1u << VIDEO_STATUS_EXTENSION_SHIFT); /* Advertise one 64-byte STATUS extension. */
    p[71] = VIDEO_PRESENT_NATIVE | VIDEO_RADIO_OFF | VIDEO_VERIFY_FRAMES | VIDEO_QUEUED_PRESENTATION | VIDEO_PAIRED_PRESENTATION | VIDEO_START_PAUSED | VIDEO_DITHER_PRESENTATION;
    video_write32(p + 72, queue.gap_deadline);
    video_write32(p + 76, queue.gap_sequence);
    video_write32(p + 80, ctx->video_presentation.presented);
    video_write32(p + 84, ctx->video_presentation.failures);
    video_write32(p + 88, ctx->video_presentation.completed_tick);
    video_write32(p + 92, s->options & 255u);
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
    if (entry->length == VIDEO_CREDITS_TIMELINE_REQUEST_BYTES) {
        const video_timeline *t = &ctx->video_control.timeline;
        uint32_t room = entry->result ? 0 : VIDEO_TIMELINE_RECORDS - t->count;
        p[5] = (p[5] & VIDEO_CREDIT_RESULT_MASK) | room << VIDEO_CREDIT_TIMELINE_SHIFT;
    }
}

/* Publish a timed record's frame time and NAL together, or neither: a frame
 * time without its NAL would hold a timeline record the phone never filled. */
static int video_control_timed_nal(customCfwContext *ctx, const uint8_t *data, uint16_t size,
                                   const cfw_message_route *route) {
    video_control_state *s = &ctx->video_control;
    video_owner *owner = ctx->video_owner;
    uint32_t stream = video_read32(data + 2), sequence = video_read32(data + 6);
    const uint8_t *nal = data + VIDEO_NAL_TIMED_HEADER_BYTES;
    uint16_t bytes = size - VIDEO_NAL_TIMED_HEADER_BYTES;
    video_frame_time frame = {video_read32(data + 10), sequence, video_read32(data + 14)};
    if (!owner || !s->start_guard || route->origin != s->owner_origin || stream != s->stream ||
        !video_control_generation_valid(ctx, s->start_guard) ||
        video_lifecycle_state(&ctx->video) != VIDEO_READY) return 0;
    /* An exact replay published its frame time with the first copy. */
    if (video_queue_probe(&owner->queue, sequence, nal, bytes) == 1 &&
        !video_timeline_offer_preroll(&s->timeline, stream, s->control_generation, &frame,
                                      (s->options >> VIDEO_PREROLL_SHIFT) & 255u))
        return VIDEO_TIMED_TIMELINE_REFUSED;
    return video_worker_receive_nal_locked(stream, sequence, nal, bytes, route->origin);
}

static int video_control_apply(customCfwContext *ctx, const uint8_t *p,
                               uint32_t n, uint8_t here, uint8_t origin, uint8_t targets) {
    video_control_state *s = &ctx->video_control;
    uint8_t op = p[1];
    if (op == VIDEO_CONTROL_RADIO_POLICY) {
        if (n != 12) return VIDEO_CONTROL_FORMAT;
        uint32_t value = video_read32(p + 8);
        if (value > 1 && value != UINT32_MAX) return VIDEO_CONTROL_FORMAT;
        if (s->start_guard && origin != s->owner_origin) return VIDEO_CONTROL_STALE;
        if (value != UINT32_MAX)
            s->options = (s->options & ~VIDEO_RADIO_POLICY_MASK) | VIDEO_RADIO_POLICY_CONFIGURED |
                (value ? 0 : VIDEO_RADIO_POLICY_OFF);
        return VIDEO_CONTROL_ACCEPTED;
    }
    if (op == VIDEO_CONTROL_TIMELINE) {
        if (n != VIDEO_TIMELINE_BYTES) return VIDEO_CONTROL_FORMAT;
        if (s->start_guard || s->stop_generation || s->controller_job ||
            video_lifecycle_state(&ctx->video) != VIDEO_IDLE) return VIDEO_CONTROL_BUSY;
        uint32_t stream = video_read32(p + 8);
        if (stream <= s->stream_high || !video_timeline_configure(&s->timeline, stream,
            video_read32(p + 12), video_read32(p + 16),
            (uint16_t)p[20] | (uint16_t)p[21] << 8,
            (uint16_t)p[22] | (uint16_t)p[23] << 8)) return VIDEO_CONTROL_FORMAT;
        s->owner_origin = origin;
        return VIDEO_CONTROL_ACCEPTED;
    }
    if (op == VIDEO_CONTROL_FRAME_TIME || op == VIDEO_CONTROL_FRAME_TIME_FAST) {
        if (n != VIDEO_TIMELINE_BYTES) return VIDEO_CONTROL_FORMAT;
        if (!s->start_guard || origin != s->owner_origin ||
            !video_control_generation_valid(ctx, s->start_guard)) return VIDEO_CONTROL_STALE;
        video_frame_time frame = {video_read32(p + 12), video_read32(p + 16), video_read32(p + 20)};
        if (op == VIDEO_CONTROL_FRAME_TIME_FAST && s->timeline.count == VIDEO_TIMELINE_RECORDS &&
            frame.ordinal > s->timeline.accepted) return VIDEO_CONTROL_BUSY;
        return video_timeline_offer_preroll(&s->timeline, video_read32(p + 8),
            s->control_generation, &frame, (s->options >> VIDEO_PREROLL_SHIFT) & 255u) ?
            (video_worker_wake_locked(ctx->video.token, VIDEO_WAKE_INPUT), VIDEO_CONTROL_ACCEPTED) : VIDEO_CONTROL_INPUT;
    }
    if (op == VIDEO_CONTROL_CREDITS) {
        if (n != VIDEO_CREDITS_REQUEST_BYTES && (n != VIDEO_CREDITS_TIMELINE_REQUEST_BYTES ||
            video_read32(p + 20) != VIDEO_CREDITS_TIMELINE)) return VIDEO_CONTROL_FORMAT;
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
        if ((n != VIDEO_START_BYTES && n != VIDEO_START_GEOMETRY_BYTES && n != VIDEO_START_PICTURE_BYTES) ||
            p[16] != VIDEO_FRAME_REFERENCES || p[17] != VIDEO_FRAME_DPB ||
            p[18] & ~(VIDEO_PRESENT_NATIVE | VIDEO_RADIO_OFF | VIDEO_VERIFY_FRAMES | VIDEO_QUEUED_PRESENTATION | VIDEO_PAIRED_PRESENTATION | VIDEO_START_PAUSED | VIDEO_DITHER_PRESENTATION) ||
            ((p[19] || (p[18] & (VIDEO_START_PAUSED | VIDEO_DITHER_PRESENTATION))) && !(p[18] & VIDEO_QUEUED_PRESENTATION)))
            return VIDEO_CONTROL_FORMAT;
        video_geometry geometry = {p[12] | (uint16_t)p[13] << 8, p[14] | (uint16_t)p[15] << 8,
            {0, 0, p[12] | (uint16_t)p[13] << 8, p[14] | (uint16_t)p[15] << 8}};
        uint32_t style = n == VIDEO_START_PICTURE_BYTES ? p[32] : 0;
        if (style > 3 || (n == VIDEO_START_PICTURE_BYTES && (p[33] || p[34] || p[35])) ||
            (style && !(p[18] & VIDEO_DITHER_PRESENTATION))) return VIDEO_CONTROL_FORMAT;
        if (n >= VIDEO_START_GEOMETRY_BYTES) {
            geometry.picture = (video_picture_rect){p[24] | (uint16_t)p[25] << 8,
                p[26] | (uint16_t)p[27] << 8, p[28] | (uint16_t)p[29] << 8,
                p[30] | (uint16_t)p[31] << 8};
        } else if (geometry.width != VIDEO_FRAME_WIDTH || geometry.height != VIDEO_FRAME_HEIGHT)
            return VIDEO_CONTROL_FORMAT;
        if (!video_geometry_valid(geometry)) return VIDEO_CONTROL_FORMAT;
        uint32_t stream = video_read32(p + 8), interval = video_read32(p + 20);
        if (!stream || stream <= s->stream_high || interval < VIDEO_INTERVAL_MIN ||
            interval > VIDEO_INTERVAL_MAX) return VIDEO_CONTROL_FORMAT;
        if (ctx->texture_cache || !video_lease_valid(ctx)) return VIDEO_CONTROL_LEASE;
        if (s->start_guard || s->stop_generation || s->controller_job ||
            video_lifecycle_state(&ctx->video) != VIDEO_IDLE)
            return VIDEO_CONTROL_BUSY;
        if (s->control_generation == UINT32_MAX) return VIDEO_CONTROL_STALE;
        if (s->timeline.configured && (s->timeline.stream != stream || s->owner_origin != origin))
            return VIDEO_CONTROL_FORMAT;
        if ((p[18] & VIDEO_QUEUED_PRESENTATION) &&
            !s->timeline.configured) return VIDEO_CONTROL_FORMAT;
        if ((p[18] & VIDEO_PAIRED_PRESENTATION) && !(p[18] & VIDEO_QUEUED_PRESENTATION))
            return VIDEO_CONTROL_FORMAT;
        s->stream = s->stream_high = stream;
        s->geometry = geometry;
        s->picture_style = style;
        s->interval = interval;
        uint32_t policy = s->options & VIDEO_RADIO_POLICY_MASK;
        s->options = p[18] | (uint32_t)p[19] << VIDEO_PREROLL_SHIFT | policy;
        if ((policy & VIDEO_RADIO_POLICY_CONFIGURED) && (policy & VIDEO_RADIO_POLICY_OFF))
            s->options |= VIDEO_RADIO_OFF;
        s->owner_origin = origin;
        s->owner_here = here;
        s->owner_targets = targets;
        uint32_t generation = s->control_generation + 1;
        if (s->timeline.configured) {
            video_timeline_start(&s->timeline, stream, generation);
        }
        __atomic_store_n(&s->control_generation, generation, __ATOMIC_RELEASE);
        __atomic_store_n(&s->start_guard, generation, __ATOMIC_RELEASE);
        uint32_t deadline = VIDEO_TICK + VIDEO_INACTIVITY_LIMIT_MS;
        __atomic_store_n(&s->active_deadline, deadline ? deadline : 1, __ATOMIC_RELEASE);
        s->error = 0;
        ctx->video_decode_last = (video_decode_totals){0};
        __atomic_store_n(&ctx->video_failure.valid, 0, __ATOMIC_RELEASE);
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
        video_timeline_stop(&s->timeline, s->control_generation);
        s->stop_generation = s->control_generation;
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

static int video_control_command(const uint8_t *data, uint16_t size,
                                  const cfw_message_route *route, uint8_t *reply) {
    if (data && size >= 2 && data[0] == VIDEO_MESSAGE_ID &&
        (data[1] == VIDEO_CONTROL_NAL || data[1] == VIDEO_CONTROL_NAL_TIMED)) {
        uint32_t header = data[1] == VIDEO_CONTROL_NAL ? VIDEO_NAL_HEADER_BYTES : VIDEO_NAL_TIMED_HEADER_BYTES;
        if (size <= header || size > VIDEO_RECORD_LIMIT ||
            !video_route_valid(route)) return video_refused(peekCustomCfwContext(), data, size, route, VIDEO_REFUSAL_FORMAT, 0, 0, 0);
        customCfwContext *ctx = peekCustomCfwContext();
        if (!ctx || !ctx->image_mutex || !video_control_wait(ctx, VIDEO_STOP_LIMIT_MS))
            return video_refused(ctx, data, size, route, VIDEO_REFUSAL_LOCK, 0, 0, 0);
        video_fold_signals(ctx);
        int copied = data[1] == VIDEO_CONTROL_NAL_TIMED ? video_control_timed_nal(ctx, data, size, route) :
            video_worker_receive_nal_locked(video_read32(data + 2),
            video_read32(data + 6), data + VIDEO_NAL_HEADER_BYTES,
            size - VIDEO_NAL_HEADER_BYTES, route->origin);
        video_control_give(ctx);
        return copied > 0 ? 0 : video_refused(ctx, data, size, route, VIDEO_REFUSAL_COMMAND, 0, 0, (uint32_t)copied);
    }
    if (!data || size < VIDEO_CONTROL_HEADER_BYTES || data[0] != VIDEO_MESSAGE_ID ||
        data[2] != VIDEO_PROTOCOL_VERSION || data[3] || !video_route_valid(route))
        return video_refused(peekCustomCfwContext(), data, size, route, VIDEO_REFUSAL_FORMAT, 0, 0, 0);
    uint32_t request = video_read32(data + 4);
    if (!request) return video_refused(peekCustomCfwContext(), data, size, route, VIDEO_REFUSAL_FORMAT, 0, 0, 1);
    if (!video_worker_ensure_mutex()) return video_refused(peekCustomCfwContext(), data, size, route, VIDEO_REFUSAL_LOCK, 0, 0, 2);
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !ctx->image_mutex || !video_control_wait(ctx, VIDEO_STOP_LIMIT_MS))
        return video_refused(ctx, data, size, route, VIDEO_REFUSAL_LOCK, 0, 0, 0);
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
        if (size < VIDEO_CONTROL_HEADER_BYTES + 1 || !entry) {
            video_control_give(ctx);
            return video_refused(ctx, data, size, route, entry ? VIDEO_REFUSAL_FORMAT : VIDEO_REFUSAL_MISSING, 0, 0, 0);
        }
        page = data[8];
    } else if (entry) {
        int equal = size == entry->length;
        for (uint32_t i = 0; equal && i < size; ++i) equal = data[i] == entry->command[i];
        if (!equal) {
            video_control_give(ctx); return video_refused(ctx, data, size, route, VIDEO_REFUSAL_REPLAY, 0, 0, 0);
        }
    } else {
        if (request <= s->request_high[origin] || size > VIDEO_START_PICTURE_BYTES) {
            video_control_give(ctx); return video_refused(ctx, data, size, route, VIDEO_REFUSAL_REPLAY, 0, 0, s->request_high[origin]);
        }
        entry = &s->replay[origin][s->replay_next[origin]];
        s->replay_next[origin] = (s->replay_next[origin] + 1) % VIDEO_CONTROL_REPLAYS;
        *entry = (video_control_replay){0};
        entry->request = s->request_high[origin] = request;
        entry->length = size;
        /* Freeze the stride even when a later packet proves a larger budget. */
        entry->capacity = route->reply_capacity < VIDEO_PAGE_REPLY_CAPACITY ?
            route->reply_capacity : VIDEO_PAGE_REPLY_CAPACITY;
        memcpy(entry->command, data, size);
        entry->result = video_control_apply(ctx, data, size, route->here, route->origin, route->targets);
        video_snapshot(ctx, entry);
        if (data[1] == VIDEO_CONTROL_RADIO_POLICY) {
            bzero(entry->snapshot, VIDEO_RADIO_POLICY_BYTES);
            entry->snapshot_bytes = VIDEO_RADIO_POLICY_BYTES;
            uint32_t selected = !(s->options & VIDEO_RADIO_POLICY_CONFIGURED) || !(s->options & VIDEO_RADIO_POLICY_OFF);
            uint32_t active = s->start_guard ? !(s->options & VIDEO_RADIO_OFF) : UINT32_MAX;
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = VIDEO_CONTROL_RADIO_POLICY;
            entry->snapshot[2] = entry->result; entry->snapshot[3] = route->here;
            video_write32(entry->snapshot + 4, selected); video_write32(entry->snapshot + 8, active);
            video_write32(entry->snapshot + 12, active != UINT32_MAX && active != selected);
            video_write32(entry->snapshot + 16, s->start_guard && s->owner_here != s->owner_origin);
        }
        if (data[1] == VIDEO_CONTROL_MENU || data[1] == VIDEO_CONTROL_SCALE) {
            bzero(entry->snapshot, VIDEO_MENU_STATE_BYTES);
            entry->snapshot_bytes = VIDEO_MENU_STATE_BYTES;
            entry->result = video_menu_control_locked(data, size, route->origin, entry->snapshot);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = data[1]; entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_PAUSE || data[1] == VIDEO_CONTROL_RESUME ||
            data[1] == VIDEO_CONTROL_PLAYBACK) {
            bzero(entry->snapshot, VIDEO_PLAYBACK_STATE_BYTES);
            entry->snapshot_bytes = VIDEO_PLAYBACK_STATE_BYTES;
            entry->result = video_playback_control_locked(data, size, route->origin, entry->snapshot);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = data[1]; entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_PACE) {
            bzero(entry->snapshot, VIDEO_PLAYBACK_STATE_BYTES);
            entry->snapshot_bytes = VIDEO_PLAYBACK_STATE_BYTES;
            entry->result = video_pace_control_locked(data, size, route->origin, entry->snapshot);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = data[1]; entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_SHOWN) {
            bzero(entry->snapshot, VIDEO_SHOWN_BYTES);
            entry->snapshot_bytes = VIDEO_SHOWN_BYTES;
            entry->result = size != 20 ? VIDEO_CONTROL_FORMAT :
                video_shown_snapshot_locked(ctx, data, route->origin, entry->snapshot);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = VIDEO_CONTROL_SHOWN;
            entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_TRIGGER_TIMING) {
            bzero(entry->snapshot, VIDEO_TRIGGER_TIMING_BYTES);
            entry->snapshot_bytes = VIDEO_TRIGGER_TIMING_BYTES;
            entry->result = size != 20 ? VIDEO_CONTROL_FORMAT :
                video_trigger_snapshot_locked(ctx, data, route->origin, entry->snapshot);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = data[1]; entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_FRAME_TIME_FAST) {
            entry->snapshot_bytes = VIDEO_CREDITS_BYTES;
            video_write32(entry->snapshot, s->timeline.accepted);
            entry->snapshot[4] = VIDEO_TIMELINE_RECORDS - s->timeline.count;
            entry->snapshot[5] = entry->result;
        }
        if (data[1] == VIDEO_PLAYBACK_CAPABILITIES && size == VIDEO_CONTROL_HEADER_BYTES) {
            bzero(entry->snapshot, VIDEO_PLAYBACK_CAPABILITIES_BYTES);
            entry->snapshot_bytes = VIDEO_PLAYBACK_CAPABILITIES_BYTES;
            entry->result = VIDEO_CONTROL_ACCEPTED;
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION;
            entry->snapshot[1] = VIDEO_PLAYBACK_CAPABILITIES;
            entry->snapshot[3] = VIDEO_PLAYBACK_QUEUE | VIDEO_PLAYBACK_PAIRED | VIDEO_PLAYBACK_METADATA | VIDEO_PLAYBACK_SHOWN | VIDEO_PLAYBACK_CONTROLS | VIDEO_PLAYBACK_SEEK | VIDEO_PLAYBACK_MENU | VIDEO_PLAYBACK_POLICY;
            entry->snapshot[4] = VIDEO_DECODED_DEPTH | VIDEO_PLAYBACK_GEOMETRY | VIDEO_PLAYBACK_PICTURE_STYLE;
            entry->snapshot[5] = VIDEO_TIMELINE_RATE_MASK;
            entry->snapshot[6] = 255; /* Maximum explicit decoded seek prefix. */
            entry->snapshot[7] = 255; /* Adds timed NAL records and timeline credits; 8 marks multi-slice pictures. */
            video_write32(entry->snapshot + 8, VIDEO_PRESENTER_A4_LIMIT);
            video_write32(entry->snapshot + 12, VIDEO_PRESENTER_STACK_BYTES);
        }
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
        int profile = size == VIDEO_CONTROL_HEADER_BYTES + 4 && data[8] == VIDEO_PROFILE_DIAGNOSTICS &&
            data[9] < 2 && !data[10] && !data[11];
        if (data[1] == VIDEO_CONTROL_DIAGNOSTICS && profile) {
            bzero(entry->snapshot, VIDEO_DIAGNOSTICS_BYTES);
            entry->snapshot_bytes = VIDEO_DIAGNOSTICS_BYTES;
            entry->result = video_profile_snapshot_locked(ctx, data[9], route->origin, entry->snapshot);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION; entry->snapshot[1] = VIDEO_CONTROL_DIAGNOSTICS;
            entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_DIAGNOSTICS && size == 12 && !data[10] && !data[11] &&
            (data[8] == VIDEO_REFUSAL_DIAGNOSTICS || data[8] == VIDEO_CLOCK_DIAGNOSTICS)) {
            bzero(entry->snapshot, VIDEO_DIAGNOSTICS_BYTES);
            entry->snapshot_bytes = VIDEO_DIAGNOSTICS_BYTES;
            if (data[8] == VIDEO_REFUSAL_DIAGNOSTICS)
                entry->result = data[9] < 2 ? video_refusal_snapshot(ctx, data[9] + 1, route->here, entry->snapshot) : VIDEO_CONTROL_FORMAT;
            else {
                entry->result = data[9] < 3 ? VIDEO_CONTROL_ACCEPTED : VIDEO_CONTROL_FORMAT;
                if (data[9] < 3) memcpy(entry->snapshot, ctx->video_telemetry.clock.rows[data[9]], VIDEO_DIAGNOSTICS_BYTES);
                entry->snapshot[3] = VIDEO_CLOCK_DIAGNOSTICS; entry->snapshot[4] = data[9];
                entry->snapshot[5] = 1; entry->snapshot[6] = route->here;
            }
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION; entry->snapshot[1] = VIDEO_CONTROL_DIAGNOSTICS;
            entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_DIAGNOSTICS && (size == 12 || size == 16) && data[8] == VIDEO_HOP_DIAGNOSTICS && !data[10] && !data[11]) {
            entry->snapshot_bytes = size == 12 || data[9] ? VIDEO_DIAGNOSTICS_BYTES : VIDEO_HOP_BYTES;
            bzero(entry->snapshot, entry->snapshot_bytes);
            entry->result = data[9] > 1 || (size == 12 && data[9]) ? VIDEO_CONTROL_FORMAT : size == 12 ? VIDEO_CONTROL_ACCEPTED :
                video_hop_snapshot(route->origin, route->here, video_read32(data + 12), entry->snapshot, data[9]);
            entry->snapshot[0] = VIDEO_PROTOCOL_VERSION; entry->snapshot[1] = VIDEO_CONTROL_DIAGNOSTICS;
            entry->snapshot[2] = entry->result;
            if (size == 12) {
                entry->snapshot[3] = VIDEO_HOP_DIAGNOSTICS; entry->snapshot[5] = 2;
                entry->snapshot[8] = VIDEO_HOP_BYTES;
                entry->snapshot[9] = VIDEO_HOP_SIZE_BYTES;
            }
        }
        int radio = size == VIDEO_CONTROL_HEADER_BYTES + 4 &&
            (data[8] == VIDEO_RADIO_DIAGNOSTICS || data[8] == VIDEO_RADIO_REFUSAL_DIAGNOSTICS) &&
            !data[9] && !data[10] && !data[11];
        if (data[1] == VIDEO_CONTROL_DIAGNOSTICS && size == 12 &&
            data[8] == VIDEO_FAILURE_DIAGNOSTICS && data[9] < 2 && !data[10] && !data[11]) {
            uint8_t *out = entry->snapshot;
            bzero(out, VIDEO_FAILURE_BYTES);
            entry->snapshot_bytes = VIDEO_FAILURE_BYTES;
            entry->result = s->control_generation ? VIDEO_CONTROL_ACCEPTED : VIDEO_CONTROL_STALE;
            uint32_t valid = __atomic_load_n(&ctx->video_failure.valid, __ATOMIC_ACQUIRE);
            if (valid) memcpy(out, data[9] ? ctx->video_failure.history : ctx->video_failure.detail, VIDEO_FAILURE_BYTES);
            out[0] = 1; out[1] = VIDEO_CONTROL_DIAGNOSTICS; out[2] = entry->result;
            out[3] = VIDEO_FAILURE_DIAGNOSTICS; out[4] = data[9]; out[5] = 1;
            out[6] = route->here; out[7] = valid;
        }
        if (data[1] == VIDEO_CONTROL_DIAGNOSTICS && size == 24 &&
            data[8] == VIDEO_RATE_DIAGNOSTICS && data[9] < 3 && !data[10] && !data[11]) {
            entry->snapshot_bytes = data[9] ? VIDEO_DIAGNOSTICS_BYTES : 144;
            bzero(entry->snapshot, entry->snapshot_bytes);
            entry->result = route->origin == s->owner_origin &&
                video_read32(data + 12) == ctx->video_rate.stream &&
                video_rate_snapshot(&ctx->video_rate, data[9],
                    video_read32(data + (route->here == 1 ? 16 : 20)), route->here, entry->snapshot) ?
                VIDEO_CONTROL_ACCEPTED : VIDEO_CONTROL_STALE;
            entry->snapshot[0] = 1; entry->snapshot[1] = VIDEO_CONTROL_DIAGNOSTICS;
            entry->snapshot[2] = entry->result;
        }
        if (data[1] == VIDEO_CONTROL_DIAGNOSTICS &&
            (size == VIDEO_CONTROL_HEADER_BYTES || radio)) {
            entry->pending = 1;
            /* Heap walking and completed-worker sampling belong outside the
             * receive publication lock. Revalidate the reserved replay after
             * sampling: another request may have evicted it meanwhile. */
            video_control_give(ctx);
            uint8_t *snapshot = reply;
            int sampled = radio ? data[8] == VIDEO_RADIO_REFUSAL_DIAGNOSTICS ?
                VIDEO_RADIO_REFUSAL_SNAPSHOT(snapshot) : VIDEO_RADIO_SNAPSHOT(snapshot) :
                video_diagnostics_snapshot(snapshot);
            if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS))
                return video_refused(ctx, data, size, route, VIDEO_REFUSAL_LOCK, 0, 0, 1);
            if (entry->request != request || !entry->pending) {
                video_control_give(ctx); return video_refused(ctx, data, size, route, VIDEO_REFUSAL_MISSING, 0, 0, 1);
            }
            entry->pending = 0;
            entry->result = sampled ? VIDEO_CONTROL_ACCEPTED : VIDEO_CONTROL_BUSY;
            if (!sampled) bzero(snapshot, VIDEO_DIAGNOSTICS_BYTES);
            snapshot[0] = VIDEO_PROTOCOL_VERSION;
            snapshot[1] = VIDEO_CONTROL_DIAGNOSTICS;
            snapshot[2] = entry->result;
            if (!radio && video_read32(snapshot + 8) != ctx->video.generation) snapshot[3] |= 32;
            memcpy(entry->snapshot, snapshot, VIDEO_DIAGNOSTICS_BYTES);
            entry->snapshot_bytes = VIDEO_DIAGNOSTICS_BYTES;
        }
    }
    if (entry->pending) {
        video_control_give(ctx); return video_refused(ctx, data, size, route, VIDEO_REFUSAL_PENDING, 0, 0, 0);
    }
    uint32_t count = entry->capacity - VIDEO_REPLY_HEADER_BYTES;
    uint32_t pages = (entry->snapshot_bytes + count - 1) / count;
    if (page >= pages || route->reply_capacity < entry->capacity) {
        uint8_t capacity = entry->capacity;
        video_control_give(ctx);
        return video_refused(ctx, data, size, route, page >= pages ? VIDEO_REFUSAL_PAGE : VIDEO_REFUSAL_CAPACITY, capacity, pages, 0);
    }
    reply[0] = VIDEO_MESSAGE_ID; reply[1] = route->here;
    video_write32(reply + 2, request);
    reply[6] = page; reply[7] = pages;
    uint32_t offset = page * count;
    if (count > entry->snapshot_bytes - offset) count = entry->snapshot_bytes - offset;
    memcpy(reply + VIDEO_REPLY_HEADER_BYTES, entry->snapshot + offset, count);
    int accepted = data[1] == VIDEO_CONTROL_PAGE || entry->result == VIDEO_CONTROL_ACCEPTED;
    uint8_t frozen_capacity = entry->capacity, result = entry->result;
    video_control_give(ctx);
    /* Both transport send services copy before return; no owner pointer is
     * retained and the owned reply remains valid through the enqueue. */
    int sent = cfw_message_video_reply(route, reply, VIDEO_REPLY_HEADER_BYTES + count);
    if (sent || !accepted)
        return video_refused(ctx, data, size, route, sent ? VIDEO_REFUSAL_SEND : VIDEO_REFUSAL_COMMAND, frozen_capacity, pages, sent ? (uint32_t)sent : result);
    return !sent && accepted ? 0 : -1;
}

int video_control_received(const uint8_t *data, uint16_t size,
                            const cfw_message_route *route) {
    if (data && size >= 2 && data[0] == VIDEO_MESSAGE_ID &&
        (data[1] == VIDEO_CONTROL_NAL || data[1] == VIDEO_CONTROL_NAL_TIMED))
        return video_control_command(data, size, route, 0);
    if (!data || size < VIDEO_CONTROL_HEADER_BYTES || !video_route_valid(route)) return -1;
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return -1;
    uint32_t origin = route->origin - 1, empty = 0;
    /* Fixed per-origin scratch survives unlocked diagnostic sampling and the
     * copied enqueue. A nested same-origin request refuses, never overwrites. */
    if (!__atomic_compare_exchange_n(&ctx->video_telemetry.reply_busy[origin], &empty, 1, 0,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
        return video_refused(ctx, data, size, route, VIDEO_REFUSAL_PENDING, 0, 0, 1);
    uint8_t *reply = ctx->video_telemetry.reply[origin];
    int result = video_control_command(data, size, route, reply);
    __atomic_store_n(&ctx->video_telemetry.reply_busy[origin], 0, __ATOMIC_RELEASE);
    return result;
}

int video_control_blocks_custom(const uint8_t *p, uint32_t n) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !p || !n || (!ctx->video_control.start_guard && !ctx->video_control.stop_generation &&
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
