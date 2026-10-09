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
int video_queue_push(video_nal_queue *q, uint32_t sequence,
                       const uint8_t *bytes, uint16_t length) {
    if (!q || !q->token || !bytes || !length || length > VIDEO_RAW_NAL_BYTES ||
        q->accepted == UINT32_MAX) return 0;
    for (uint32_t i = 0; i < q->capacity; ++i) {
        video_nal_slot *s = &q->slots[i];
        if (s->occupied) continue;
        for (uint32_t j = 0; j < length; ++j) s->bytes[j] = bytes[j];
        s->sequence = sequence; s->length = length; s->ordinal = q->accepted;
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
        if (s->occupied && (chosen == VIDEO_QUEUE_MAX ||
            s->ordinal < q->slots[chosen].ordinal)) chosen = i;
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
    --q->count; ++q->consumed;
    return 1;
}

#pragma clang section text=""
