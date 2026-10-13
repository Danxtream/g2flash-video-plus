/* Bridge callbacks post captured records only. SPDX-License-Identifier: GPL-3.0-only */
#include "sync.h"
#pragma clang section text=".text.video"

/* Keep the receiver in its own text section instead of inlining mailbox work
 * into the much larger stock bridge-wrapper section and overflowing ROPI. */
__attribute__((noinline)) uint32_t video_sync_received(const uint8_t *data, uint32_t bytes) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !data || bytes != VIDEO_SYNC_BYTES || data[0] != VIDEO_SYNC_BRIDGE || data[2] != 1)
        return 6;
    video_sync_mail *m = &ctx->video_control.sync_mail;
    uint32_t high = __atomic_load_n(&m->high, __ATOMIC_RELAXED);
    uint32_t low = __atomic_load_n(&m->low, __ATOMIC_ACQUIRE);
    if (high == UINT32_MAX || high - low == VIDEO_SYNC_MAIL_ROWS) {
        __atomic_store_n(&m->overflow, 1, __ATOMIC_RELEASE);
        video_control_notify_sync(); return 6;
    }
    /* This dispatcher is the ordered single producer before the worker pool.
     * Never reset indices at START while an old callback could still publish. */
    uint32_t slot = high % VIDEO_SYNC_MAIL_ROWS;
    memcpy(m->rows[slot], data, bytes); m->captured[slot] = VIDEO_TICK;
    __atomic_store_n(&m->high, high + 1, __ATOMIC_RELEASE);
    video_control_notify_sync(); return 0;
}

#ifndef VIDEO_SYNC_SEND
#define VIDEO_SYNC_SEND(p) video_power_platform_packet(p, VIDEO_SYNC_BYTES, 0)
#endif

static int video_presenter_sync_send(video_presenter *p, uint32_t op, uint32_t ordinal,
                                       uint32_t pts, uint32_t depth, int terminal) {
    uint8_t body[VIDEO_SYNC_BYTES] = {VIDEO_SYNC_BRIDGE, p->sync.side, 1, op};
    video_write32(body + 4, p->sync.stream); video_write32(body + 8, ordinal);
    video_write32(body + 12, pts); video_write32(body + 16, VIDEO_TICK);
    body[20] = depth; body[21] = terminal != 0;
    return VIDEO_SYNC_SEND(body);
}

static void video_presenter_sync_poll(video_presenter *p) {
    video_sync_mail *m = &p->decoder->context->video_control.sync_mail;
    if (__atomic_load_n(&m->overflow, __ATOMIC_ACQUIRE))
        video_presenter_fault(p->decoder, VIDEO_FAULT_OWNERSHIP);
    for (;;) {
        uint32_t low = __atomic_load_n(&m->low, __ATOMIC_RELAXED);
        if (low == __atomic_load_n(&m->high, __ATOMIC_ACQUIRE)) break;
        uint8_t body[VIDEO_SYNC_BYTES]; uint32_t index = low % VIDEO_SYNC_MAIL_ROWS;
        memcpy(body, m->rows[index], sizeof(body));
        uint32_t captured = m->captured[index];
        __atomic_store_n(&m->low, low + 1, __ATOMIC_RELEASE);
        if (body[1] == p->sync.side || video_read32(body + 4) != p->sync.stream) continue;
        uint32_t ordinal = video_read32(body + 8), pts = video_read32(body + 12);
        if (body[3] == VIDEO_SYNC_PAUSE || body[3] == VIDEO_SYNC_RESUME) {
            if (body[22] || body[23] || body[21] || body[20] > 1 || !ordinal)
                video_presenter_fault(p->decoder, VIDEO_FAULT_SEQUENCE);
            uint32_t paused = body[3] == VIDEO_SYNC_PAUSE;
            if (p->sync.side == CFW_MESSAGE_LEFT && body[1] == CFW_MESSAGE_RIGHT && !body[20]) {
                if (ordinal < p->wire_serial) continue;
                if (ordinal > p->wire_serial + 1 ||
                    (ordinal > p->wire_serial ? pts != p->media_shown :
                     (paused != p->wire_paused || pts != p->wire_boundary)))
                    video_presenter_fault(p->decoder, VIDEO_FAULT_SEQUENCE);
                p->wire_serial = ordinal; p->wire_paused = p->paused = paused;
                p->wire_boundary = pts;
                if (!video_presenter_sync_send(p, body[3], ordinal, pts, 1, 0))
                    video_presenter_fault(p->decoder, VIDEO_FAULT_DISPLAY);
            } else if (p->sync.side == CFW_MESSAGE_RIGHT && body[1] == CFW_MESSAGE_LEFT && body[20]) {
                if (ordinal <= p->control_ack) continue;
                if (ordinal != p->control_serial || pts != p->media_shown || paused != p->paused)
                    video_presenter_fault(p->decoder, VIDEO_FAULT_SEQUENCE);
                p->control_ack = ordinal; p->control_deadline = 0;
            } else video_presenter_fault(p->decoder, VIDEO_FAULT_SEQUENCE);
            continue;
        }
        if (body[22] || body[23] || body[21] > 1 ||
            !video_sync_receive(&p->sync, body[1], video_read32(body + 4), body[3],
                                ordinal, pts, body[20], body[21]))
            video_presenter_fault(p->decoder, VIDEO_FAULT_SEQUENCE);
        if (body[3] == VIDEO_SYNC_TRIGGER) {
            if (ordinal == p->sync.inflight && ordinal != p->captured_ordinal) {
                p->wire_capture = captured; p->captured_ordinal = ordinal;
            }
            if (ordinal == p->media_shown && pts == p->done_pts)
                video_presenter_sync_send(p, VIDEO_SYNC_DONE, ordinal, pts, 0, 0);
        }
    }
    uint32_t now = VIDEO_TICK;
    if (p->sync.completed && p->sync.completed == p->sync.peer_completed) p->sync_deadline = 0;
    if (p->sync_deadline && (int32_t)(now - p->sync_deadline) >= 0)
        video_presenter_fault(p->decoder, VIDEO_FAULT_DISPLAY);
    if (p->sync.side == 2 && p->sync.inflight && p->sync.peer_completed != p->sync.inflight &&
        (int32_t)(now - p->sync_sent) >= 100) {
        if (!video_presenter_sync_send(p, VIDEO_SYNC_TRIGGER, p->sync.inflight, p->sync.flight_pts, 0, 0))
            video_presenter_fault(p->decoder, VIDEO_FAULT_DISPLAY);
        p->sync_sent = now;
    }
}
#pragma clang section text=""
