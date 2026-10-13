/* SPDX-License-Identifier: GPL-3.0-only */
#include "decoded_queue.h"
#include "brightness.h"

#pragma clang section text=".text.video"

int video_decoded_init(video_decoded_queue *q, uint32_t generation, uint32_t token,
                       uint32_t format, uint32_t depth, uint8_t *pixels, uint32_t bytes) {
    uint32_t slot = format == VIDEO_SOURCE_A4 ? VIDEO_DECODED_A4_BYTES :
                    format == VIDEO_SOURCE_Y8 ? VIDEO_DECODED_Y8_BYTES : 0;
    if (!q || !generation || !token || !pixels || !slot || !depth ||
        depth > VIDEO_DECODED_DEPTH || bytes < depth * slot) return 0;
    *q = (video_decoded_queue){.generation = generation, .token = token,
                                .format = format, .depth = depth, .slot_bytes = slot};
    for (uint32_t i = 0; i < depth; ++i) q->slots[i].pixels = pixels + i * slot;
    return 1;
}

int video_decoded_skip_prefix(video_decoded_queue *q, uint32_t generation, uint32_t prefix) {
    if (!q || q->generation != generation || prefix > 255 || q->count || q->serial ||
        q->accepted || q->released || q->cancelled) return 0;
    q->accepted = q->released = prefix;
    return 1;
}

uint32_t video_decoded_reserve(video_decoded_queue *q, uint32_t generation,
                              const video_frame_time *media) {
    if (!q || !media || q->generation != generation || q->cancelled ||
        q->count == q->depth || q->serial == UINT32_MAX || q->accepted == UINT32_MAX ||
        media->ordinal != q->accepted + 1) return 0;
    uint32_t index = (q->head + q->count) % q->depth;
    video_decoded_slot *slot = &q->slots[index];
    if (slot->state != VIDEO_DECODED_FREE) return 0;
    slot->media = *media;
    slot->serial = ++q->serial;
    slot->copied = 0;
    slot->state = VIDEO_DECODED_FILLING;
    ++q->count;
    q->accepted = media->ordinal;
    return slot->serial;
}

static video_decoded_slot *video_decoded_filling(video_decoded_queue *q, uint32_t serial) {
    for (uint32_t i = 0; i < q->depth; ++i)
        if (q->slots[i].serial == serial && q->slots[i].state == VIDEO_DECODED_FILLING)
            return &q->slots[i];
    return 0;
}

int video_decoded_copy(video_decoded_queue *q, uint32_t generation, uint32_t serial,
                       const uint8_t *y, uint32_t stride, uint32_t bytes) {
    return video_decoded_copy_range(q, generation, serial, y, stride, bytes, 0);
}

int video_decoded_copy_range(video_decoded_queue *q, uint32_t generation, uint32_t serial,
                             const uint8_t *y, uint32_t stride, uint32_t bytes, uint32_t range) {
    if (!q || !y || q->generation != generation ||
        (range & ~(VIDEO_SOURCE_TV | VIDEO_SOURCE_UNSIGNALLED)) ||
        ((range & VIDEO_SOURCE_UNSIGNALLED) && !(range & VIDEO_SOURCE_TV)) ||
        __atomic_load_n(&q->cancelled, __ATOMIC_ACQUIRE) || stride < 320 ||
        stride > UINT32_MAX / 192 || bytes < stride * 192) return 0;
    video_decoded_slot *slot = video_decoded_filling(q, serial);
    if (!slot) return 0;
    uintptr_t a = (uintptr_t)y, b = (uintptr_t)slot->pixels;
    if (a > UINTPTR_MAX - bytes || b > UINTPTR_MAX - q->slot_bytes ||
        (a < b + q->slot_bytes && b < a + bytes)) return 0;
    for (uint32_t row = 0; row < 192; ++row) {
        const uint8_t *src = y + row * stride;
        uint8_t *dst = slot->pixels + row * (q->format == VIDEO_SOURCE_A4 ? 160 : 320);
        if (q->format == VIDEO_SOURCE_A4) {
            for (uint32_t col = 0; col < 160; ++col)
                dst[col] = (video_gray_level(src[2 * col], range & VIDEO_SOURCE_TV) << 4) |
                            video_gray_level(src[2 * col + 1], range & VIDEO_SOURCE_TV);
        } else {
            for (uint32_t col = 0; col < 320; ++col) dst[col] = src[col];
        }
    }
    slot->range = range; slot->copied = 1;
    return 1;
}

int video_decoded_publish(video_decoded_queue *q, uint32_t generation, uint32_t serial) {
    if (!q || q->generation != generation || q->cancelled) return 0;
    video_decoded_slot *slot = video_decoded_filling(q, serial);
    if (!slot || !slot->copied) return 0;
    slot->state = VIDEO_DECODED_READY;
    return 1;
}

int video_decoded_claim(video_decoded_queue *q, uint32_t generation, uint32_t ordinal,
                        uint32_t scale, video_frame_descriptor *frame, video_frame_time *media) {
    if (!q || !frame || !media || q->generation != generation || q->cancelled || !q->count ||
        (scale != VIDEO_SCALE_NATIVE && scale != VIDEO_SCALE_DOUBLE)) return 0;
    video_decoded_slot *slot = &q->slots[q->head];
    if (slot->state != VIDEO_DECODED_READY || slot->media.ordinal != ordinal) return 0;
    *frame = (video_frame_descriptor){q->token, generation, ordinal, 320, 192,
        q->format == VIDEO_SOURCE_A4 ? 160 : 320, q->slot_bytes, q->format | slot->range, scale, slot->pixels};
    *media = slot->media;
    slot->state = VIDEO_DECODED_PRESENTING;
    return 1;
}

int video_decoded_release(video_decoded_queue *q, uint32_t generation, uint32_t ordinal) {
    if (!q || q->generation != generation || !q->count || q->released == UINT32_MAX) return 0;
    video_decoded_slot *slot = &q->slots[q->head];
    if (slot->state != VIDEO_DECODED_PRESENTING || slot->media.ordinal != ordinal ||
        ordinal != q->released + 1) return 0;
    slot->state = VIDEO_DECODED_FREE;
    q->head = (q->head + 1) % q->depth;
    --q->count;
    q->released = ordinal;
    return 1;
}

int video_decoded_cancel(video_decoded_queue *q, uint32_t generation) {
    if (!q || q->generation != generation) return 0;
    __atomic_store_n(&q->cancelled, 1, __ATOMIC_RELEASE);
    for (uint32_t i = 0; i < q->depth; ++i)
        if (q->slots[i].state == VIDEO_DECODED_PRESENTING) return 0;
    return 1;
}

#pragma clang section text=""
