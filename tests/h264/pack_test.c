/* Production packing checks. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../patches/video/pack.c"

static uint8_t brightness(uint32_t x, uint32_t y) {
    return (uint8_t)(x + y * 7);
}
static void check_pixels(uint32_t format, uint32_t scale, uint32_t width, uint32_t height) {
    uint32_t stride = (format == VIDEO_SOURCE_Y8 ? width : width / 2) + 17;
    uint32_t bytes = stride * height;
    uint8_t *source = malloc(bytes), *storage = malloc(VIDEO_PANEL_BYTES + 64);
    assert(source && storage);
    memset(source, 0xcd, bytes); memset(storage, 0xef, VIDEO_PANEL_BYTES + 64);
    for (uint32_t y = 0; y < height; ++y) {
        if (format == VIDEO_SOURCE_Y8) {
            for (uint32_t x = 0; x < width; ++x) source[y * stride + x] = brightness(x, y);
        } else {
            for (uint32_t x = 0; x < width; x += 2)
                source[y * stride + x / 2] = (brightness(x, y) & 0xf0) |
                                             (brightness(x + 1, y) >> 4);
        }
    }
    video_frame_descriptor frame = {3, 9, 17, width, height, stride, bytes, format, scale, source};
    uint8_t *shadow = storage + 32;
    assert(video_pack_frame(shadow, VIDEO_PANEL_BYTES, &frame));
    uint32_t ox = (640 - width * scale) / 2, oy = (480 - height * scale) / 2;
    for (uint32_t y = 0; y < 480; ++y)
        for (uint32_t x = 0; x < 640; ++x) {
            uint8_t actual = shadow[y * 320 + x / 2];
            actual = x % 2 ? actual & 15 : actual >> 4;
            uint8_t expected = x >= ox && x < ox + width * scale &&
                y >= oy && y < oy + height * scale ?
                brightness((x - ox) / scale, (y - oy) / scale) >> 4 : 0;
            assert(actual == expected);
        }
    for (uint32_t i = 0; i < 32; ++i)
        assert(storage[i] == 0xef && storage[VIDEO_PANEL_BYTES + 32 + i] == 0xef);
    free(source); free(storage);
}
static void rejected(void) {
    uint8_t *shadow = malloc(VIDEO_PANEL_BYTES), *source = malloc(320 * 192);
    assert(shadow && source); memset(shadow, 0xaa, VIDEO_PANEL_BYTES);
    video_frame_descriptor valid = {1, 1, 1, 320, 192, 320, 320 * 192,
                                    VIDEO_SOURCE_Y8, VIDEO_SCALE_DOUBLE, source};
    for (uint32_t variant = 0; variant < 11; ++variant) {
        video_frame_descriptor f = valid;
        switch (variant) {
            case 0: f.width = 0; break;
            case 1: f.width = 322; break;
            case 2: f.height = 241; break;
            case 3: f.stride = 319; break;
            case 4: f.stride = UINT32_MAX; break;
            case 5: --f.bytes; break;
            case 6: f.scale = 0; break;
            case 7: f.format = 0; break;
            case 8: f.pixels = shadow + 16; break;
            case 9: f.pixels = (const uint8_t *)(UINTPTR_MAX - 1); break;
            case 10: f.pixels = 0; break;
        }
        assert(!video_pack_frame(shadow, VIDEO_PANEL_BYTES, &f));
    }
    assert(!video_pack_frame(shadow, VIDEO_PANEL_BYTES - 1, &valid));
    assert(!video_pack_frame(0, VIDEO_PANEL_BYTES, &valid));
    assert(!video_pack_frame(shadow, VIDEO_PANEL_BYTES, 0));
    for (uint32_t i = 0; i < VIDEO_PANEL_BYTES; ++i) assert(shadow[i] == 0xaa);
    free(source); free(shadow);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "pixels")) {
        for (uint32_t format = VIDEO_SOURCE_Y8; format <= VIDEO_SOURCE_A4; ++format)
            for (uint32_t scale = VIDEO_SCALE_NATIVE; scale <= VIDEO_SCALE_DOUBLE; ++scale)
                check_pixels(format, scale, 320, 192);
        check_pixels(VIDEO_SOURCE_A4, VIDEO_SCALE_NATIVE, 640, 384);
    } else if (!strcmp(argv[1], "reject")) rejected();
    else if (!strcmp(argv[1], "sides")) {
        assert(video_source_from_side(2) == 1 && video_source_from_side(1) == 2);
        assert(!video_source_from_side(0) && !video_source_from_side(3));
    } else assert(0);
    puts("bounded top-down A4 packing PASS");
    return 0;
}
