/* No transport scheduling changes. SPDX-License-Identifier: GPL-3.0-only */
#include "hop.h"
#pragma clang section text=".text.video"

__attribute__((noinline)) static video_hop_state *video_hop_take(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx) return 0;
    video_hop_state *s = &ctx->video_telemetry.hop;
    uint32_t empty = 0;
    if (__atomic_compare_exchange_n(&s->busy, &empty, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return s;
    __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED);
    return 0;
}
__attribute__((noinline)) uint32_t video_hop_tick(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    uint32_t generation = ctx ? __atomic_load_n(&ctx->video_control.start_guard, __ATOMIC_ACQUIRE) : 0;
    if (!generation) return 0;
    if (__atomic_load_n(&ctx->video_telemetry.hop.generation, __ATOMIC_ACQUIRE) != generation) {
        video_hop_state *s = video_hop_take();
        if (!s) return 0;
        if (__atomic_load_n(&s->generation, __ATOMIC_RELAXED) != generation) {
            __atomic_store_n(&s->generation, 0, __ATOMIC_RELEASE);
            __atomic_store_n(&s->dropped, 0, __ATOMIC_RELAXED);
            bzero((uint8_t *)s + 12, sizeof(*s) - 12);
            s->stream = ctx->video_control.stream;
            __atomic_store_n(&s->generation, generation, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    }
    return VIDEO_TICK;
}
static uint32_t video_hop_add(video_hop_state *s, uint32_t a, uint32_t b) {
    if (b <= UINT32_MAX - a) return a + b;
    s->flags |= 1; return UINT32_MAX;
}
__attribute__((noinline)) static void video_hop_sample(video_hop_state *s, uint32_t stage, uint32_t ticks) {
    video_hop_row *r = &s->rows[stage];
    r->count = video_hop_add(s, r->count, 1); r->sum = video_hop_add(s, r->sum, ticks);
    if (ticks > r->maximum) r->maximum = ticks;
    uint32_t bin = ticks <= 1 ? 0 : ticks <= 8 ? 1 : ticks <= 32 ? 2 : 3;
    if (r->bins[bin] != UINT16_MAX) ++r->bins[bin]; else s->flags |= 1;
}
__attribute__((noinline)) static video_hop_slot *video_hop_find(video_hop_state *s, uint32_t key, int opening) {
    video_hop_slot *empty = 0, *old = 0;
    for (uint32_t i = 0; i < VIDEO_HOP_SLOTS; ++i) {
        video_hop_slot *p = &s->slots[i];
        if (p->flags && p->key == key) return p;
        if (!p->flags) empty = p;
        if (p->flags >= 64 && (!old || (int32_t)(p->peer - old->peer) < 0)) old = p;
    }
    return opening ? (empty ? empty : old) : 0;
}
__attribute__((noinline)) uint32_t video_hop_overlap(uint32_t key) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !__atomic_load_n(&ctx->video_control.start_guard, __ATOMIC_ACQUIRE)) return 0;
    video_hop_state *s = video_hop_take();
    if (!s) return 0;
    uint32_t overlap = 0;
    for (uint32_t i = 0; i < VIDEO_HOP_SLOTS; ++i)
        if ((s->slots[i].flags & 7) == 3 && s->slots[i].key != key) overlap = 1;
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    return overlap;
}
static void video_hop_bridge_sample(video_hop_state *s, const video_hop_slot *slot, uint32_t ticks) {
    video_hop_sample(s, 4, ticks);
    uint32_t index = slot->bytes <= 512 ? 0 : slot->bytes <= 1024 ? 1 : slot->bytes <= 2048 ? 2 : 3;
    video_hop_bucket *r = &s->buckets[index];
    r->count = video_hop_add(s, r->count, 1); r->sum = video_hop_add(s, r->sum, ticks);
    r->bytes = video_hop_add(s, r->bytes, slot->bytes);
    if (ticks > r->maximum) r->maximum = ticks;
}
__attribute__((noinline)) void video_hop_open(const cfw_message_stream *stream, uint8_t origin, uint8_t here, const uint8_t *data) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !stream->hop_rx || !ctx->video_control.start_guard ||
        origin != ctx->video_control.owner_origin || video_read32(data + 2) != ctx->video_control.stream) return;
    video_hop_state *s = video_hop_take();
    if (!s) return;
    if (!s->began) s->began = stream->hop_rx;
    s->token = ctx->video.token;
    uint32_t key = stream->stream_id | (uint32_t)stream->message_id << 8 | (uint32_t)origin << 24;
    video_hop_slot *slot = video_hop_find(s, key, 1);
    if (!slot || (slot->flags && slot->flags < 64)) {
        s->flags |= 2; __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED);
        __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE); return;
    }
    uint32_t early = slot->flags >= 64 && slot->key == key && slot->checksum == stream->checksum ? slot->flags : 0;
    uint32_t peer = slot->peer, relay = slot->rx;
    *slot = (video_hop_slot){key, stream->hop_rx, stream->hop_forward, 0, peer, 1, stream->checksum, stream->size};
    /* Classification follows the first six decoded bytes. A delayed header or
     * compressed record makes peak depth a lower bound; durations stay local. */
    if (VIDEO_TICK - stream->hop_rx > 1) s->flags |= 32;
    if (origin == here && (stream->options & (here ^ CFW_MESSAGE_BOTH))) {
        s->incoming = video_hop_add(s, s->incoming, 1);
        if (stream->hop_overlap) s->overlapped = video_hop_add(s, s->overlapped, 1);
        slot->flags |= 2;
        if (early) {
            if (early & 1) s->flags |= 4;
            if (early & 2) s->flags |= 16;
            s->flags |= 32; slot->flags |= 4;
            video_hop_bridge_sample(s, slot, peer - slot->forward);
            if (!s->maximum) s->maximum = 1;
            if (early & 128) {
                slot->flags |= 16;
                video_hop_sample(s, 5, relay - peer);
            }
        } else { ++s->pending; if (s->pending > s->maximum) s->maximum = s->pending; }
        video_hop_sample(s, 0, stream->hop_forward - stream->hop_rx);
    }
    s->last = VIDEO_TICK;
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
}
__attribute__((noinline)) void video_hop_event(uint32_t key, uint8_t event, uint32_t detail) {
    customCfwContext *ctx = peekCustomCfwContext();
    if (!ctx || !__atomic_load_n(&ctx->video_telemetry.hop.generation, __ATOMIC_ACQUIRE)) return;
    video_hop_state *s = video_hop_take();
    if (!s) return;
    video_hop_slot *p = video_hop_find(s, key, 0);
    uint32_t now = VIDEO_TICK;
    /* The donor may deliver the peer ACK before local parsing has classified
     * the body. Cache it without counting non-NAL records; verify CRC on open. */
    if (event == VIDEO_HOP_PEER && !p) {
        p = video_hop_find(s, key, 1);
        if (p && p->flags) s->flags |= 64;
        if (p) *p = (video_hop_slot){key, 0, 0, 0, now, 64 | (detail & 3), detail >> 16, 0};
        else { s->flags |= 2; __atomic_fetch_add(&s->dropped, 1, __ATOMIC_RELAXED); }
    }
    if (p && p->flags >= 64) {
        if (event == VIDEO_HOP_RELAY) { p->rx = now; p->flags |= 128 | (detail & 1); }
        __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE); return;
    }
    if (p && event == VIDEO_HOP_PEER && p->checksum != detail >> 16) {
        s->flags |= 128; __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE); return;
    }
    if (p) {
        s->last = now;
        if (event == VIDEO_HOP_WIRE) {
            p->wire = now; video_hop_sample(s, 2, now - p->rx);
            if (p->flags & 2) video_hop_sample(s, 1, detail);
        } else if (event == VIDEO_HOP_LOCAL) {
            if (detail & 1) s->flags |= 4;
            video_hop_sample(s, 3, now - p->wire);
            p->flags |= 8;
            if (!(p->flags & 2)) { video_hop_sample(s, 6, now - p->rx); p->flags = 0; }
        } else if (event == VIDEO_HOP_PEER && (p->flags & 2) && !(p->flags & 4)) {
            if (detail & 1) s->flags |= 4;
            if (detail & 2) s->flags |= 16;
            p->peer = now; p->flags |= 4;
            if (s->pending) --s->pending;
            video_hop_bridge_sample(s, p, now - p->forward);
        } else if (event == VIDEO_HOP_RELAY && (p->flags & 4) && !(p->flags & 16)) {
            if (detail & 1) s->flags |= 4;
            video_hop_sample(s, 5, now - p->peer);
            p->flags |= 16;
        }
        if ((p->flags & 26) == 26) {
            video_hop_sample(s, 6, now - p->rx); p->flags = 0;
        }
    }
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
}
__attribute__((noinline)) int video_hop_snapshot(uint8_t origin, uint8_t here, uint32_t stream, uint8_t *out, uint32_t mode) {
    video_hop_state *s = video_hop_take();
    if (!s) return VIDEO_CONTROL_BUSY;
    uint32_t now = peekCustomCfwContext()->video_control.start_guard ? VIDEO_TICK : s->last, live = 0;
    for (uint32_t i = 0; i < VIDEO_HOP_SLOTS; ++i) {
        video_hop_slot *p = &s->slots[i];
        if ((p->flags & 7) == 3) live = video_hop_add(s, live, now - p->forward);
    }
    out[3] = VIDEO_HOP_DIAGNOSTICS; out[4] = mode; out[5] = 2; out[6] = here; out[7] = origin;
    video_write32(out + 8, s->stream); video_write32(out + 12, s->token);
    out[16] = s->pending; out[17] = s->maximum;
    video_write32(out + 20, now - s->began); video_write32(out + 24, live);
    video_write32(out + 28, s->flags | (__atomic_load_n(&s->dropped, __ATOMIC_RELAXED) ? 8 : 0));
    /* These fixed-width rows are the wire layout on the little-endian target.
     * Refuse another ABI instead of silently exporting native padding. */
    _Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ && sizeof(video_hop_row) == 20 &&
                   __builtin_offsetof(video_hop_row, bins) == 12, "Hop wire layout");
    if (mode) {
        _Static_assert(sizeof(video_hop_bucket) == 16, "Bucket wire layout");
        memcpy(out + 32, s->buckets, sizeof(s->buckets));
        video_write32(out + 96, s->overlapped); video_write32(out + 100, s->incoming);
    } else memcpy(out + 32, s->rows, sizeof(s->rows));
    int result = s->stream == stream && origin == peekCustomCfwContext()->video_control.owner_origin ?
        VIDEO_CONTROL_ACCEPTED : VIDEO_CONTROL_STALE;
    __atomic_store_n(&s->busy, 0, __ATOMIC_RELEASE);
    return result;
}
#pragma clang section text=""
