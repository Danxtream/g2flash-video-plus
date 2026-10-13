/* SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../patches/video/decoded_queue.c"

int main(void) {
    uint8_t *y = malloc(VIDEO_DECODED_Y8_BYTES);
    uint8_t *pixels = malloc(2 * VIDEO_DECODED_Y8_BYTES);
    assert(y && pixels);
    for (uint32_t format = VIDEO_SOURCE_Y8; format <= VIDEO_SOURCE_A4; ++format) {
        for (uint32_t depth = 1; depth <= VIDEO_DECODED_DEPTH; ++depth) {
            video_decoded_queue q = {0};
            video_frame_descriptor frame;
            video_frame_time media, first = {1, 3, 0}, second = {2, 7, 62};
            assert(!video_decoded_init(&q, 1, 9, format, depth, pixels, 1));
            assert(video_decoded_init(&q, 1, 9, format, depth, pixels, 2 * VIDEO_DECODED_Y8_BYTES));
            uint32_t serial = video_decoded_reserve(&q, 1, &first);
            assert(serial && !video_decoded_publish(&q, 1, serial));
            assert(!video_decoded_copy(&q, 1, serial, y, 319, VIDEO_DECODED_Y8_BYTES));
            assert(!video_decoded_copy(&q, 1, serial, y, UINT32_MAX, VIDEO_DECODED_Y8_BYTES));
            memset(y, 0xab, VIDEO_DECODED_Y8_BYTES);
            assert(video_decoded_copy(&q, 1, serial, y, 320, VIDEO_DECODED_Y8_BYTES));
            memset(y, 0, VIDEO_DECODED_Y8_BYTES);
            assert(!video_decoded_publish(&q, 2, serial));
            assert(video_decoded_publish(&q, 1, serial));
            assert(!video_decoded_claim(&q, 1, 2, VIDEO_SCALE_NATIVE, &frame, &media));
            assert(video_decoded_claim(&q, 1, 1, VIDEO_SCALE_DOUBLE, &frame, &media));
            assert(frame.pixels[0] == (format == VIDEO_SOURCE_A4 ? 0xaa : 0xab));
            assert(media.ordinal == 1 && media.nal_sequence == 3);
            uint32_t next = video_decoded_reserve(&q, 1, &second);
            assert((next != 0) == (depth == 2));
            assert(!video_decoded_release(&q, 2, 1));
            assert(!video_decoded_cancel(&q, 1));
            assert(!video_decoded_reserve(&q, 1, &second));
            assert(video_decoded_release(&q, 1, 1));
            assert(video_decoded_cancel(&q, 1));
            assert(!video_decoded_copy(&q, 1, next, y, 320, VIDEO_DECODED_Y8_BYTES));
            assert(!video_decoded_claim(&q, 1, 2, VIDEO_SCALE_NATIVE, &frame, &media));
        }
    }
    video_decoded_queue q = {0};
    assert(!video_decoded_init(&q, 7, 9, VIDEO_SOURCE_A4 | VIDEO_SOURCE_DITHER, 2, pixels, 2 * VIDEO_DECODED_Y8_BYTES));
    assert(!video_decoded_init(&q, 7, 9, VIDEO_SOURCE_Y8 | VIDEO_SOURCE_DITHER, 2, pixels, 2 * VIDEO_DECODED_A4_BYTES));
    assert(video_decoded_init(&q, 7, 9, VIDEO_SOURCE_Y8 | VIDEO_SOURCE_DITHER, 2, pixels, 2 * VIDEO_DECODED_Y8_BYTES));
    video_frame_time dither_time = {1, 1, 0};
    uint32_t dither_serial = video_decoded_reserve(&q, 7, &dither_time);
    memset(y, 14, VIDEO_DECODED_Y8_BYTES);
    assert(video_decoded_copy_range(&q, 7, dither_serial, y, 320, VIDEO_DECODED_Y8_BYTES, 0));
    memset(y, 0, VIDEO_DECODED_Y8_BYTES);
    assert(video_decoded_publish(&q, 7, dither_serial));
    video_frame_descriptor dither_frame;
    assert(video_decoded_claim(&q, 7, 1, VIDEO_SCALE_DOUBLE, &dither_frame, &dither_time));
    assert(dither_frame.format == (VIDEO_SOURCE_Y8 | VIDEO_SOURCE_DITHER) && dither_frame.pixels[0] == 14);
    assert(video_decoded_release(&q, 7, 1));
    assert(video_decoded_init(&q, 7, 9, VIDEO_SOURCE_A4, 2, pixels, 2 * VIDEO_DECODED_Y8_BYTES));
    for (uint32_t ordinal = 1; ordinal < 50; ++ordinal) {
        video_frame_time media = {ordinal, ordinal * 3, ordinal * 62};
        video_frame_descriptor frame;
        memset(y, ordinal, VIDEO_DECODED_Y8_BYTES);
        uint32_t serial = video_decoded_reserve(&q, 7, &media);
        assert(serial && video_decoded_copy(&q, 7, serial, y, 320, VIDEO_DECODED_Y8_BYTES));
        assert(video_decoded_publish(&q, 7, serial));
        assert(video_decoded_claim(&q, 7, ordinal, VIDEO_SCALE_NATIVE, &frame, &media));
        assert(video_decoded_release(&q, 7, ordinal));
    }
    q.serial = UINT32_MAX;
    video_frame_time final = {50, 150, 3100};
    assert(!video_decoded_reserve(&q, 7, &final));
    assert(video_decoded_init(&q, 8, 9, VIDEO_SOURCE_A4, 2, pixels, 2 * VIDEO_DECODED_Y8_BYTES));
    assert(!video_decoded_skip_prefix(&q, 7, 3));
    assert(!video_decoded_skip_prefix(&q, 8, 256));
    assert(video_decoded_skip_prefix(&q, 8, 3));
    assert(q.accepted == 3 && q.released == 3 && !q.serial && !q.count);
    video_frame_time sought = {4, 8, 500};
    assert(video_decoded_reserve(&q, 8, &sought));
    assert(!video_decoded_skip_prefix(&q, 8, 3));
    for (uint32_t format = VIDEO_SOURCE_Y8; format <= VIDEO_SOURCE_A4; ++format) {
        assert(video_decoded_init(&q, 9, 9, format, 2, pixels, 2 * VIDEO_DECODED_Y8_BYTES));
        memset(y, 235, VIDEO_DECODED_Y8_BYTES);
        for (uint32_t ordinal = 1; ordinal <= 2; ++ordinal) {
            video_frame_time time = {ordinal, ordinal, ordinal * 62};
            uint32_t serial = video_decoded_reserve(&q, 9, &time);
            uint32_t range = ordinal == 1 ? 0 : VIDEO_SOURCE_TV | VIDEO_SOURCE_UNSIGNALLED;
            assert(!video_decoded_copy_range(&q, 9, serial, y, 320, VIDEO_DECODED_Y8_BYTES, 128));
            assert(video_decoded_copy_range(&q, 9, serial, y, 320, VIDEO_DECODED_Y8_BYTES, range));
            assert(video_decoded_publish(&q, 9, serial));
        }
        memset(y, 0, VIDEO_DECODED_Y8_BYTES);
        for (uint32_t ordinal = 1; ordinal <= 2; ++ordinal) {
            video_frame_descriptor frame; video_frame_time time;
            assert(video_decoded_claim(&q, 9, ordinal, VIDEO_SCALE_NATIVE, &frame, &time));
            assert((frame.format & VIDEO_SOURCE_TV) == (ordinal == 1 ? 0 : VIDEO_SOURCE_TV));
            assert((frame.format & VIDEO_SOURCE_UNSIGNALLED) == (ordinal == 1 ? 0 : VIDEO_SOURCE_UNSIGNALLED));
            assert(frame.pixels[0] == (format == VIDEO_SOURCE_Y8 ? 235 : ordinal == 1 ? 0xee : 0xff));
            assert(video_decoded_release(&q, 9, ordinal));
        }
    }
    free(y);
    free(pixels);
    puts("A4/Y8 ownership, poisoned producer, FIFO pins, cancel, depth and finite serials PASS");
    return 0;
}
