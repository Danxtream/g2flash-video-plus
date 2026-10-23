/* SPDX-License-Identifier: GPL-3.0-only */
#include "decoded_queue.h"
#include "brightness.h"

#pragma clang section text=".text.video"

int video_decoded_init(video_decoded_queue *q, uint32_t generation, uint32_t token,
                       uint32_t format, uint32_t depth, uint8_t *pixels, uint32_t bytes) {
    uint32_t type = format & VIDEO_SOURCE_TYPE_MASK;
    uint32_t slot = type == VIDEO_SOURCE_A4 ? VIDEO_DECODED_A4_BYTES :
                    type == VIDEO_SOURCE_Y8 ? VIDEO_DECODED_Y8_BYTES : 0;
    if (!q || !generation || !token || !pixels || !slot || !depth ||
        depth > VIDEO_DECODED_DEPTH || bytes < depth * slot ||
        (format & ~(VIDEO_SOURCE_TYPE_MASK | VIDEO_SOURCE_DITHER | VIDEO_SOURCE_STYLE_MASK | VIDEO_SOURCE_PATTERN4)) ||
        ((format & VIDEO_SOURCE_STYLE_MASK) && !(format & VIDEO_SOURCE_DITHER)) ||
        ((format & VIDEO_SOURCE_PATTERN4) && (type != VIDEO_SOURCE_A4 || !(format & VIDEO_SOURCE_DITHER)))) return 0;
    /* Dithered 4-bit slots (mid sizes) take the pattern when copied; see copy_range. */
    *q = (video_decoded_queue){.generation = generation, .token = token, .format = format,
        .depth = depth, .slot_bytes = slot, .bytes = bytes, .geometry = video_geometry_default()};
    for (uint32_t i = 0; i < depth; ++i) q->slots[i].pixels = pixels + i * slot;
    return 1;
}

int video_decoded_geometry(video_decoded_queue *q, video_geometry geometry) {
    if (!q || q->count || q->accepted || q->cancelled || !video_geometry_valid(geometry)) return 0;
    if (video_geometry_full_width(geometry)) {
        if ((q->format & VIDEO_SOURCE_TYPE_MASK) != VIDEO_SOURCE_A4) return 0;
        uint32_t slot = geometry.width * geometry.height / 2;
        if (q->depth * slot > q->bytes) return 0;
        for (uint32_t i = 0; i < q->depth; ++i) q->slots[i].pixels = q->slots[0].pixels + i * slot;
        q->slot_bytes = slot;
    } else if ((q->format & VIDEO_SOURCE_TYPE_MASK) == VIDEO_SOURCE_A4 && (q->format & VIDEO_SOURCE_DITHER))
        return 0;
    q->geometry = geometry;
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
        __atomic_load_n(&q->cancelled, __ATOMIC_ACQUIRE) || stride < q->geometry.width ||
        stride > UINT32_MAX / q->geometry.height || bytes < stride * q->geometry.height) return 0;
    video_decoded_slot *slot = video_decoded_filling(q, serial);
    if (!slot) return 0;
    uintptr_t a = (uintptr_t)y, b = (uintptr_t)slot->pixels;
    if (a > UINTPTR_MAX - bytes || b > UINTPTR_MAX - q->slot_bytes ||
        (a < b + q->slot_bytes && b < a + bytes)) return 0;
    uint32_t width = q->geometry.width, height = q->geometry.height;
    int dithered = (q->format & VIDEO_SOURCE_TYPE_MASK) == VIDEO_SOURCE_A4 && (q->format & VIDEO_SOURCE_DITHER);
    int fine = (q->format & VIDEO_SOURCE_PATTERN4) != 0;
    uint32_t shift = fine ? 4 : 2, mask = fine ? 15 : 3, tv = (range & VIDEO_SOURCE_TV) != 0;
    if (dithered && q->steps_range != tv + 1) {
        /* Same results as video_dither_tuned / video_dither_4x4 per pixel, with the
         * per-value work done once: about a third of the 4x4 copy time on the PC. */
        uint32_t style = (q->format & VIDEO_SOURCE_STYLE_MASK) >> VIDEO_SOURCE_STYLE_SHIFT;
        for (uint32_t v = 0; v < 256; ++v)
            q->steps[v] = fine ? video_dither_sixteenth(v, tv, style) : video_dither_quarter(v, tv, style);
        q->steps_range = tv + 1;
    }
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t *src = y + row * stride;
        uint8_t *dst = slot->pixels + row * ((q->format & VIDEO_SOURCE_TYPE_MASK) == VIDEO_SOURCE_A4 ? width / 2 : width);
        if (dithered) {
            /* The pattern sits on source pixels; the full-width stretch then widens
             * some of them, which both cell sizes tolerate. Mid widths are multiples
             * of 16, so each step fills two whole bytes. */
            const uint8_t *cell = fine ? video_dither_cell4[row & 3] : video_dither_cell2[row & 1];
            for (uint32_t col = 0; col < width / 2; col += 2) {
                uint32_t a = q->steps[src[2 * col]], b = q->steps[src[2 * col + 1]];
                uint32_t c = q->steps[src[2 * col + 2]], d = q->steps[src[2 * col + 3]];
                dst[col] = ((a >> shift) + ((a & mask) > cell[0])) << 4 | ((b >> shift) + ((b & mask) > cell[1]));
                dst[col + 1] = ((c >> shift) + ((c & mask) > cell[2])) << 4 | ((d >> shift) + ((d & mask) > cell[3]));
            }
        } else if ((q->format & VIDEO_SOURCE_TYPE_MASK) == VIDEO_SOURCE_A4) {
            for (uint32_t col = 0; col < width / 2; ++col)
                dst[col] = (video_gray_level(src[2 * col], range & VIDEO_SOURCE_TV) << 4) |
                            video_gray_level(src[2 * col + 1], range & VIDEO_SOURCE_TV);
        } else {
            for (uint32_t col = 0; col < width; ++col) dst[col] = src[col];
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
        (scale != VIDEO_SCALE_NATIVE && scale != VIDEO_SCALE_DOUBLE && scale != VIDEO_SCALE_FULL_WIDTH) ||
        (scale == VIDEO_SCALE_FULL_WIDTH) != video_geometry_full_width(q->geometry)) return 0;
    video_decoded_slot *slot = &q->slots[q->head];
    if (slot->state != VIDEO_DECODED_READY || slot->media.ordinal != ordinal) return 0;
    *frame = (video_frame_descriptor){q->token, generation, ordinal, q->geometry.width, q->geometry.height,
        (q->format & VIDEO_SOURCE_TYPE_MASK) == VIDEO_SOURCE_A4 ? q->geometry.width / 2 : q->geometry.width,
        q->slot_bytes, ((q->format & VIDEO_SOURCE_TYPE_MASK) == VIDEO_SOURCE_A4 ?
            q->format & ~(VIDEO_SOURCE_DITHER | VIDEO_SOURCE_STYLE_MASK | VIDEO_SOURCE_PATTERN4) : q->format) | slot->range,
        scale, slot->pixels, q->geometry.picture};
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
