/* SPDX-License-Identifier: GPL-3.0-only */
#include "queue.h"
#include <stddef.h>

#pragma clang section text=".text.video"

void video_queue_init(video_nal_queue *q, uint32_t token) {
    *q = (video_nal_queue){0};
    q->token = token;
}
int video_queue_attach(video_nal_queue *q, uint8_t *bytes) {
    if (!q || !bytes || q->capacity >= VIDEO_QUEUE_MAX) return 0;
    for (uint32_t i = 0; i < q->capacity; ++i)
        if (q->slots[i].bytes == bytes) return 0;
    q->slots[q->capacity] = (video_nal_slot){.bytes = bytes};
    ++q->capacity;
    return 1;
}
int video_queue_extend(video_nal_queue *q, uint8_t *bytes) {
    if (!q || !bytes || q->capacity != VIDEO_QUEUE_INITIAL) return 0;
    for (uint32_t i = 0; i < q->capacity; ++i)
        if (q->slots[i].bytes == bytes || q->slots[i].bytes == bytes + VIDEO_SLOT_BYTES) return 0;
    q->slots[VIDEO_QUEUE_INITIAL] = (video_nal_slot){.bytes = bytes};
    q->slots[VIDEO_QUEUE_INITIAL + 1] = (video_nal_slot){.bytes = bytes + VIDEO_SLOT_BYTES};
    q->capacity = VIDEO_QUEUE_MAX;
    return 1;
}
uint32_t video_queue_credits(const video_nal_queue *q) {
    return q ? q->capacity - q->count : 0;
}
int video_queue_push(video_nal_queue *q, uint32_t sequence,
                       const uint8_t *bytes, uint16_t length) {
    if (!q || !q->token || !bytes || !length || length > VIDEO_RAW_NAL_BYTES) return 0;
    if (sequence == UINT32_MAX) return VIDEO_QUEUE_WRAP;
    /* Released slot bytes form a bounded exact replay cache until reuse. No
     * checksum can silently accept a conflicting consumed payload. */
    for (uint32_t i = 0; i < q->capacity; ++i) {
        video_nal_slot *s = &q->slots[i];
        if (!s->length || s->sequence != sequence) continue;
        int equal = length == s->length;
        for (uint32_t j = 0; equal && j < length; ++j) equal = bytes[j] == s->bytes[j];
        return equal ? 2 : VIDEO_QUEUE_CONFLICT;
    }
    if (q->accepted == UINT32_MAX) return VIDEO_QUEUE_WRAP;
    if (sequence < q->expected) return VIDEO_QUEUE_STALE;
    if (sequence - q->expected >= q->capacity) return 0;
    for (uint32_t i = 0; i < q->capacity; ++i) {
        video_nal_slot *s = &q->slots[i];
        if (s->occupied) continue;
        for (uint32_t j = 0; j < length; ++j) s->bytes[j] = bytes[j];
        s->sequence = sequence; s->length = length;
        s->active = 0;
        s->occupied = 1; ++q->count; ++q->accepted;
        return 1;
    }
    return 0;
}
int video_queue_claim(video_nal_queue *q, uint32_t token, video_nal_view *view) {
    if (!q || !view || !token || token != q->token) return 0;
    uint32_t chosen = VIDEO_QUEUE_MAX;
    for (uint32_t i = 0; i < q->capacity; ++i) {
        video_nal_slot *s = &q->slots[i];
        if (s->active) return 0;
        if (s->occupied && s->sequence == q->expected) chosen = i;
    }
    if (chosen == VIDEO_QUEUE_MAX) return 0;
    video_nal_slot *s = &q->slots[chosen];
    s->active = 1;
    *view = (video_nal_view){s->bytes, token, s->sequence, s->length, chosen};
    return 1;
}
int video_queue_release(video_nal_queue *q, const video_nal_view *view) {
    if (!q || !view || view->token != q->token || view->slot >= q->capacity) return 0;
    video_nal_slot *s = &q->slots[view->slot];
    if (!s->occupied || !s->active || view->bytes != s->bytes ||
        view->sequence != s->sequence || view->length != s->length) return 0;
    s->active = s->occupied = 0;
    --q->count; ++q->consumed; ++q->expected;
    return 1;
}

#pragma clang section text=""
