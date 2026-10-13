/* Independent fixed-clock picture consumer. SPDX-License-Identifier: GPL-3.0-only */
#include "presenter.h"

#pragma clang section text=".text.video"

static void video_presenter_wake_locked(video_owner *owner) {
    if (owner && owner->presenter && owner->presenter->wake)
        VIDEO_OS_EVENT_SET(owner->presenter->wake, VIDEO_WAKE_ALL);
}

static void video_presenter_park(video_presenter *p) __attribute__((noreturn));
static void video_presenter_park(video_presenter *p) {
    uint32_t wake = p->wake, complete = p->complete;
    __atomic_store_n(&p->parked, 1, __ATOMIC_RELEASE);
    VIDEO_OS_EVENT_SET(complete, VIDEO_COMPLETE_PARKED);
    for (;;) VIDEO_OS_EVENT_WAIT(wake, VIDEO_WAKE_ALL, 0, VIDEO_WAIT_FOREVER);
}

static void video_presenter_fault(video_owner *owner, uint32_t fault) {
    __atomic_store_n(&owner->context->video.fault, fault, __ATOMIC_RELEASE);
    __atomic_store_n(&owner->context->video.cancel, 1, __ATOMIC_RELEASE);
    VIDEO_OS_EVENT_SET(owner->wake, VIDEO_WAKE_CANCEL);
    video_controller_parked(owner->token);
    video_presenter_park(owner->presenter);
}

int video_playback_control_locked(const uint8_t *data, uint32_t bytes, uint8_t origin, uint8_t *out) {
    customCfwContext *ctx = peekCustomCfwContext();
    video_owner *owner = ctx ? ctx->video_owner : 0;
    if (bytes != 20) return VIDEO_CONTROL_FORMAT;
    if (!owner || !owner->presenter || !ctx->video.published || ctx->video.preparing ||
        !video_control_generation_valid(ctx, owner->control_generation) ||
        video_read32(data + 8) != ctx->video_control.stream || origin != ctx->video_control.owner_origin ||
        video_read32(data + (ctx->video_control.owner_here == CFW_MESSAGE_LEFT ? 12 : 16)) != owner->token)
        return VIDEO_CONTROL_STALE;
    video_presenter *p = owner->presenter;
    uint32_t expires = VIDEO_TICK + VIDEO_INACTIVITY_LIMIT_MS;
    __atomic_store_n(&ctx->video_control.active_deadline, expires ? expires : 1, __ATOMIC_RELEASE);
    if (data[1] != VIDEO_CONTROL_PLAYBACK && (!p->paired || p->sync.side == CFW_MESSAGE_RIGHT)) {
        p->pause_requested = data[1] == VIDEO_CONTROL_PAUSE;
        if (data[1] == VIDEO_CONTROL_RESUME) p->pause_after_first = 0;
    }
    video_presenter_wake_locked(owner);
    out[3] = ctx->video_control.owner_here;
    video_write32(out + 4, ctx->video_control.stream); video_write32(out + 8, owner->token);
    video_write32(out + 12, p->paused);
    video_write32(out + 16, p->paired && p->sync.side == CFW_MESSAGE_LEFT ? p->wire_paused :
                  (p->pause_requested || p->pause_after_first));
    video_write32(out + 20, p->shown); video_write32(out + 24, p->shown ? p->done_pts : ctx->video_control.timeline.start_pts);
    video_write32(out + 28, ctx->video_control.timeline.duration);
    video_write32(out + 32, p->menu_command ||
        (p->paired && p->sync.side == CFW_MESSAGE_RIGHT && p->control_ack != p->control_serial));
    return VIDEO_CONTROL_ACCEPTED;
}

int video_menu_control_locked(const uint8_t *data, uint32_t bytes, uint8_t origin, uint8_t *out) {
    if (bytes != 24) return VIDEO_CONTROL_FORMAT;
    uint32_t value = video_read32(data + 20);
    if ((data[1] == VIDEO_CONTROL_MENU && value > VIDEO_MENU_QUERY) ||
        (data[1] == VIDEO_CONTROL_SCALE && value != VIDEO_SCALE_NATIVE && value != VIDEO_SCALE_DOUBLE))
        return VIDEO_CONTROL_FORMAT;
    uint8_t query[20]; memcpy(query, data, 20); query[1] = VIDEO_CONTROL_PLAYBACK;
    int result = video_playback_control_locked(query, 20, origin, out);
    if (result) return result;
    video_owner *owner = video_current_owner(); video_presenter *p = owner->presenter;
    if (!p->paired || p->sync.side == CFW_MESSAGE_RIGHT) {
        if (data[1] == VIDEO_CONTROL_SCALE) p->scale_requested = value == VIDEO_SCALE_NATIVE;
        else if (value != VIDEO_MENU_QUERY) {
            if (p->menu_command || !p->shown ||
                (value >= VIDEO_MENU_NEXT && (!p->paused || !p->menu.open))) return VIDEO_CONTROL_BUSY;
            p->menu_command = value + 1;
        }
    }
    video_presenter_wake_locked(owner);
    video_write32(out + 32, video_read32(out + 32) || p->menu_command);
    video_write32(out + 36, p->menu_actual | p->scale_requested << 4 |
        ((owner->context->video_control.options & VIDEO_PRESENT_NATIVE) != 0) << 5);
    video_write32(out + 40, p->menu.seek_serial); video_write32(out + 44, p->menu.seek_target);
    return VIDEO_CONTROL_ACCEPTED;
}

/* A separate callback section lets the linker resolve its ROPI address;
 * same-section MOVW/MOVT folding overflows as the surrounding player grows. */
__attribute__((section(".text.presenter"))) static void video_presenter_entry(void *argument) {
    video_presenter *p = argument;
    video_owner *owner = p->decoder;
    customCfwContext *ctx = owner->context;
    int buffering = 1;
    while (!__atomic_load_n(&p->armed, __ATOMIC_ACQUIRE)) {
        if (video_lifecycle_cancelled(&ctx->video, p->token)) video_presenter_park(p);
        VIDEO_OS_EVENT_WAIT(p->wake, VIDEO_WAKE_ALL, 0, VIDEO_WAIT_FOREVER);
    }
    while (!video_lifecycle_cancelled(&ctx->video, p->token)) {
        if (p->paired) video_presenter_sync_poll(p);
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS))
            video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
        uint32_t control_op = 0, control_serial = 0, control_boundary = 0;
        uint32_t control_now = VIDEO_TICK;
        int coordinator = !p->paired || p->sync.side == CFW_MESSAGE_RIGHT;
        int boundary = !p->paired || !p->sync.inflight ||
            (p->sync.completed == p->sync.inflight && p->sync.peer_completed == p->sync.inflight);
        if (coordinator && p->menu_command) {
            uint32_t action = p->menu_command - 1;
            int applied = video_menu_action(&p->menu, action, p->done_pts, ctx->video_control.timeline.duration);
            p->menu_command = 0;
            if (!applied) { video_control_give(ctx); video_presenter_fault(owner, VIDEO_FAULT_SEQUENCE); }
            if (applied == 2) p->pause_requested = 0;
            else if (action == VIDEO_MENU_OPEN) p->pause_requested = 1;
        }
        uint32_t transition = 0;
        if (coordinator && boundary && !p->pause_after_first && p->control_ack == p->control_serial) {
            if (p->paused != p->pause_requested) {
                p->paused = p->pause_requested;
                p->menu.open = p->paused; p->menu.selection = 0;
                transition = p->paused ? VIDEO_SYNC_PAUSE : VIDEO_SYNC_RESUME;
            } else if (p->paused && video_menu_bits(&p->menu) != p->menu_actual) transition = VIDEO_SYNC_MENU;
        }
        if (transition) {
            if (p->control_serial == UINT32_MAX) {
                video_control_give(ctx); video_presenter_fault(owner, VIDEO_FAULT_SEQUENCE);
            }
            ++p->control_serial; p->control_kind = transition;
            p->control_menu = video_menu_bits(&p->menu);
            if (p->menu_actual != p->control_menu) p->menu_dirty = 1;
            p->menu_actual = p->control_menu;
            buffering = 1;
            if (p->paired) p->control_deadline = control_now + VIDEO_STOP_LIMIT_MS;
            else p->control_ack = p->control_serial;
            p->control_sent = control_now - 100;
        }
        if (p->control_deadline && (int32_t)(control_now - p->control_deadline) >= 0) {
            video_control_give(ctx); video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
        }
        if (p->paired && coordinator && p->control_ack != p->control_serial &&
            (int32_t)(control_now - p->control_sent) >= 100) {
            control_op = p->control_kind;
            control_serial = p->control_serial; control_boundary = p->media_shown;
            p->control_sent = control_now;
        }
        if (p->shown && p->menu_dirty) {
            video_control_give(ctx);
            if (control_op && !video_presenter_sync_send(p, control_op, control_serial, control_boundary, 0, p->control_menu))
                video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
            video_frame_descriptor overlay = {.token = p->token, .generation = p->generation, .format = VIDEO_SOURCE_MENU};
            if (!video_present_frame(&overlay)) video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
            if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
            p->menu_dirty = 0; video_control_give(ctx);
            continue;
        }
        if (p->paused || (coordinator && p->control_ack != p->control_serial)) {
            buffering = 1;
            video_control_give(ctx);
            if (control_op && !video_presenter_sync_send(p, control_op, control_serial, control_boundary, 0, p->control_menu))
                video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
            VIDEO_OS_EVENT_WAIT(p->wake, VIDEO_WAKE_ALL, 0, 50);
            continue;
        }
        video_decoded_queue *q = &p->queue;
        uint32_t now = VIDEO_TICK, ordinal = q->released + 1, deadline = now;
        int available = q->count && q->slots[q->head].state == VIDEO_DECODED_READY;
        uint32_t ready_depth = 0;
        while (ready_depth < q->count &&
               q->slots[(q->head + ready_depth) % q->depth].state == VIDEO_DECODED_READY) ++ready_depth;
        /* Prebuffer two pictures, except an explicitly timed terminal picture.
         * After starvation re-anchor, never run a catch-up presentation burst. */
        int terminal = available && ctx->video_control.timeline.duration - q->slots[q->head].media.pts <= 63;
        uint32_t ready_pts = available ? q->slots[q->head].media.pts : 0;
        int advertise = 0;
        if (p->paired && available) {
            advertise = p->sync.local_ordinal != ordinal || p->sync.local_depth != ready_depth ||
                        (int32_t)(now - p->ready_sent) >= 100;
            if (!video_sync_ready(&p->sync, ordinal, ready_pts, ready_depth, terminal)) {
                video_control_give(ctx); video_presenter_fault(owner, VIDEO_FAULT_SEQUENCE);
            }
        }
        if (!available) {
            if (!buffering) ++p->holds;
            buffering = 1;
        }
        if (available && buffering && (ready_depth == q->depth || terminal) &&
            (!p->paired || p->sync.side == 1 ||
             (p->sync.peer_ordinal == ordinal && (p->sync.peer_depth == q->depth || p->sync.peer_terminal)))) {
            video_pacing_anchor(&p->pacing, p->generation, ordinal, now);
            buffering = 0;
        }
        if (available && !buffering &&
            !video_pacing_deadline(&p->pacing, p->generation, ordinal, &deadline)) {
            video_control_give(ctx);
            video_presenter_fault(owner, VIDEO_FAULT_SEQUENCE);
        }
        video_frame_descriptor frame;
        video_frame_time media;
        int trigger = 0;
        int due = available && !buffering && (int32_t)(now - deadline) >= 0;
        if (p->paired) {
            if (p->sync.side == 2) {
                trigger = due && video_sync_trigger(&p->sync);
                due = trigger;
                if (trigger) {
                    p->trigger_tick = now; p->sync_sent = now;
                    p->sync_deadline = now + VIDEO_DISPLAY_LIMIT_MS;
                    p->flight_native = p->scale_requested;
                }
            } else due = available && p->sync.triggered == ordinal && p->sync.completed < ordinal;
        }
        int claim = due &&
            video_decoded_claim(q, p->generation, ordinal,
                (p->paired ? p->flight_native : p->scale_requested) ? VIDEO_SCALE_NATIVE : VIDEO_SCALE_DOUBLE,
                &frame, &media);
        if (claim) {
            ctx->video_control.options = (ctx->video_control.options & ~VIDEO_PRESENT_NATIVE) |
                (frame.scale == VIDEO_SCALE_NATIVE ? VIDEO_PRESENT_NATIVE : 0);
        }
        if (claim && !p->paired) p->trigger_tick = deadline;
        uint32_t wait = available && !buffering && !claim ? deadline - now : 1000;
        if (p->paired && !claim) wait = 50;
        video_control_give(ctx);
        if (advertise) {
            if (!video_presenter_sync_send(p, VIDEO_SYNC_READY, ordinal, ready_pts, ready_depth, terminal))
                video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
            p->ready_sent = now;
        }
        if (trigger) {
            p->trigger_tick = VIDEO_TICK;
            if (!video_presenter_sync_send(p, VIDEO_SYNC_TRIGGER, ordinal, ready_pts, 0, p->flight_native))
                video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
        }
        if (!claim) {
            uint32_t result = VIDEO_OS_EVENT_WAIT(p->wake, VIDEO_WAKE_ALL, 0, wait);
            if ((result & VIDEO_EVENT_ERROR) && result != VIDEO_EVENT_TIMEOUT)
                video_presenter_fault(owner, VIDEO_FAULT_EVENT);
            continue;
        }
        frame.ordinal = p->shown + 1; /* Display copies exclude decoded preroll. */
        if (!video_present_frame(&frame)) {
            if (video_lifecycle_cancelled(&ctx->video, p->token)) break;
            video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
        }
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS))
            video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
        if (owner->evidence) {
            video_evidence *w = owner->evidence;
            if (frame.ordinal != w->high + 1 || w->high - w->acked >= VIDEO_EVIDENCE_ROWS) {
                video_control_give(ctx);
                video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
            }
            video_frame_result *row = &p->evidence[q->head];
            row->ordinal = frame.ordinal; row->copy_tick = ctx->video_presentation.copy_tick;
            w->rows[(frame.ordinal - 1) % VIDEO_EVIDENCE_ROWS] = *row;
            w->high = frame.ordinal;
        }
        video_shown_frame shown = {media.ordinal, media.pts, ctx->video_presentation.copy_tick,
            p->paired && p->sync.side == CFW_MESSAGE_LEFT ? p->wire_capture : p->trigger_tick,
            p->queued_ticks[q->head], p->paired ? VIDEO_SHOWN_PAIRED | VIDEO_SHOWN_UNCERTAIN : 0u};
        if (frame.format & VIDEO_SOURCE_TV) shown.flags |= VIDEO_SHOWN_TV;
        if (frame.format & VIDEO_SOURCE_UNSIGNALLED) shown.flags |= VIDEO_SHOWN_UNSIGNALLED;
        if (!video_shown_publish(&p->shown_rows, &shown)) {
            video_control_give(ctx); video_presenter_fault(owner, VIDEO_FAULT_SEQUENCE);
        }
        int released = video_decoded_release(q, p->generation, ordinal);
        ++p->shown; p->media_shown = ordinal;
        if (p->pause_after_first) { p->pause_requested = 1; p->pause_after_first = 0; }
        if (p->paired && !video_sync_complete(&p->sync, ordinal)) {
            video_control_give(ctx); video_presenter_fault(owner, VIDEO_FAULT_SEQUENCE);
        }
        p->done_pts = media.pts;
        /* Copy/decode overrun holds the cadence instead of showing overdue
         * pictures back-to-back. RIGHT paired coordination extends this later. */
        if ((int32_t)(VIDEO_TICK - deadline) >= 63) {
            ++p->holds;
            video_pacing_anchor(&p->pacing, p->generation, ordinal + 1, VIDEO_TICK + 63);
        }
        VIDEO_OS_EVENT_SET(owner->wake, VIDEO_WAKE_INPUT);
        video_control_give(ctx);
        if (!released) video_presenter_fault(owner, VIDEO_FAULT_OWNERSHIP);
        if (p->paired && p->sync.side == 1 &&
            !video_presenter_sync_send(p, VIDEO_SYNC_DONE, ordinal, media.pts, 0, 0))
            video_presenter_fault(owner, VIDEO_FAULT_DISPLAY);
    }
    video_presenter_park(p);
}

static int video_presenter_prepare(video_owner *owner) {
    uint32_t raw_bytes = sizeof(video_presenter) + VIDEO_ALLOC_ALIGNMENT - 1;
    if (!video_bootstrap_room(raw_bytes, owner->storage.display_allowance)) return 0;
    void *raw = VIDEO_OWNER_ALLOC(raw_bytes);
    if (!raw) return 0;
    video_presenter *p = (void *)(((uintptr_t)raw + VIDEO_ALLOC_ALIGNMENT - 1) &
                                 ~(uintptr_t)(VIDEO_ALLOC_ALIGNMENT - 1));
    bzero((uint8_t *)p, sizeof(*p));
    p->raw = raw; p->decoder = owner; p->generation = owner->control_generation; p->token = owner->token;
    p->preroll = (owner->context->video_control.options >> VIDEO_PREROLL_SHIFT) & 255u;
    p->pause_after_first = (owner->context->video_control.options & VIDEO_START_PAUSED) != 0;
    p->scale_requested = (owner->context->video_control.options & VIDEO_PRESENT_NATIVE) != 0;
    p->paired = owner->context->video_control.owner_targets == CFW_MESSAGE_BOTH ||
                (owner->context->video_control.options & VIDEO_PAIRED_PRESENTATION);
    if (!video_sync_init(&p->sync, owner->context->video_control.stream,
                         owner->context->video_control.owner_here)) {
        VIDEO_OWNER_FREE(raw); return 0;
    }
    owner->presenter = p;
    p->sync.next = p->preroll + 1;
    video_heap_ops heap = {0, video_platform_allocate, video_platform_release, video_platform_heap_view};
    if (!video_storage_init(&p->storage, &heap, video_owner_charge(raw_bytes),
                            owner->storage.display_allowance)) return 0;
    p->storage.limit = VIDEO_PRESENTER_A4_LIMIT;
    p->storage.cached_allowance = 0; /* Input slots already admitted by decoder. */
    p->pixels = video_storage_alloc(&p->storage, VIDEO_DECODED_DEPTH * VIDEO_DECODED_A4_BYTES);
    p->stack_allocation = video_storage_alloc(&p->storage,
        VIDEO_PRESENTER_STACK_BYTES + 2 * VIDEO_STACK_GUARD_BYTES);
    if (!p->pixels || !p->stack_allocation ||
        !video_decoded_init(&p->queue, p->generation, p->token, VIDEO_SOURCE_A4,
                            VIDEO_DECODED_DEPTH, p->pixels, VIDEO_DECODED_DEPTH * VIDEO_DECODED_A4_BYTES) ||
        !video_decoded_skip_prefix(&p->queue, p->generation, p->preroll)) return 0;
    uint8_t *base = p->stack_allocation;
    for (uint32_t i = 0; i < VIDEO_PRESENTER_STACK_BYTES + 2 * VIDEO_STACK_GUARD_BYTES; ++i)
        base[i] = VIDEO_GUARD_FILL;
    p->stack = base + VIDEO_STACK_GUARD_BYTES;
    for (uint32_t i = 0; i < VIDEO_PRESENTER_STACK_BYTES; ++i) p->stack[i] = VIDEO_STACK_FILL;
    video_event_attr wake = {"video picture wake", 0, p->wake_control, sizeof(p->wake_control)};
    video_event_attr complete = {"video picture complete", 0, p->complete_control, sizeof(p->complete_control)};
    p->wake = VIDEO_OS_EVENT_NEW(&wake); p->complete = VIDEO_OS_EVENT_NEW(&complete);
    if (!p->wake || !p->complete) return 0;
    video_thread_attr attr = {"video presenter", 0, p->tcb, sizeof(p->tcb), p->stack,
                             VIDEO_PRESENTER_STACK_BYTES, 8, 0, 0};
    p->thread = VIDEO_OS_THREAD_NEW(video_presenter_entry, p, &attr);
    if (p->thread && p->tcb[VIDEO_THREAD_STATIC_OFFSET] != VIDEO_THREAD_STATIC_FLAG) {
        __atomic_store_n(&owner->context->video.fault, VIDEO_FAULT_ABI, __ATOMIC_RELEASE);
        __atomic_store_n(&owner->context->video_quarantine_token, owner->token, __ATOMIC_RELEASE);
        return 0;
    }
    return p->thread != 0;
}

static int video_presenter_produce(video_owner *owner, const video_frame_descriptor *frame,
                                    uint32_t nal_sequence) {
    video_presenter *p = owner->presenter;
    customCfwContext *ctx = owner->context;
    video_frame_time media;
    uint32_t serial = 0, slot = 0;
    while (!video_lifecycle_cancelled(&ctx->video, owner->token)) {
        if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return 0;
        int room = p->queue.count < p->queue.depth;
        int matched = room && video_timeline_take(&ctx->video_control.timeline,
            p->generation, frame->ordinal, nal_sequence, &media);
        if (matched) {
            if (media.ordinal <= p->preroll) {
                if (owner->evidence) owner->evidence->building = (video_frame_result){0};
                video_control_give(ctx); return 1;
            }
            slot = (p->queue.head + p->queue.count) % p->queue.depth;
            serial = video_decoded_reserve(&p->queue, p->generation, &media);
            if (serial && owner->evidence) {
                p->evidence[slot] = owner->evidence->building;
                owner->evidence->building = (video_frame_result){0};
            }
        }
        video_control_give(ctx);
        if (serial) break;
        VIDEO_OS_EVENT_WAIT(owner->wake, VIDEO_WAKE_ALL, 0, 100);
    }
    if (!serial) return 0;
    int copied = video_decoded_copy_range(&p->queue, p->generation, serial,
        frame->pixels, frame->stride, frame->bytes,
        frame->format & (VIDEO_SOURCE_TV | VIDEO_SOURCE_UNSIGNALLED));
    if (!video_control_wait(ctx, VIDEO_STOP_LIMIT_MS)) return 0;
    int published = copied && video_decoded_publish(&p->queue, p->generation, serial);
    if (published) p->queued_ticks[slot] = VIDEO_TICK;
    video_presenter_wake_locked(owner);
    video_control_give(ctx);
    return published;
}

static int video_presenter_stop(video_owner *owner, uint32_t timeout) {
    video_presenter *p = owner->presenter;
    if (!p) return 1;
    if (p->thread) {
        VIDEO_OS_EVENT_SET(p->wake, VIDEO_WAKE_CANCEL);
        if (!__atomic_load_n(&p->parked, __ATOMIC_ACQUIRE) &&
            (VIDEO_OS_EVENT_WAIT(p->complete, VIDEO_COMPLETE_PARKED, 0, timeout) & VIDEO_EVENT_ERROR)) return 0;
        if (VIDEO_OS_THREAD_TERMINATE(p->thread)) return 0;
        p->thread = 0;
    }
    if (!video_platform_event_quiescent(p->wake) || !video_platform_event_quiescent(p->complete)) return 0;
    p->guards_ok = 1;
    if (p->stack) {
        const uint8_t *base = p->stack_allocation;
        for (uint32_t i = 0; i < VIDEO_STACK_GUARD_BYTES; ++i)
            if (base[i] != VIDEO_GUARD_FILL || p->stack[VIDEO_PRESENTER_STACK_BYTES + i] != VIDEO_GUARD_FILL)
                p->guards_ok = 0;
        uint32_t untouched = 0;
        while (untouched < VIDEO_PRESENTER_STACK_BYTES && p->stack[untouched] == VIDEO_STACK_FILL) ++untouched;
        p->stack_used = VIDEO_PRESENTER_STACK_BYTES - untouched;
    }
    return p->guards_ok;
}

static void video_presenter_destroy(video_owner *owner) {
    video_presenter *p = owner->presenter;
    if (!p) return;
    video_storage_reclaim(&p->storage);
    VIDEO_OWNER_FREE(p->raw);
    owner->presenter = 0;
}

#pragma clang section text=""
