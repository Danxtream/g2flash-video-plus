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
    uint32_t type = format & VIDEO_SOURCE_TYPE_MASK;
    int tv = (format & VIDEO_SOURCE_TV) != 0;
    uint32_t stride = (type == VIDEO_SOURCE_Y8 ? width : width / 2) + 17;
    uint32_t bytes = stride * height;
    uint8_t *source = malloc(bytes), *storage = malloc(VIDEO_PANEL_BYTES + 64);
    assert(source && storage);
    memset(source, 0xcd, bytes); memset(storage, 0xef, VIDEO_PANEL_BYTES + 64);
    for (uint32_t y = 0; y < height; ++y) {
        if (type == VIDEO_SOURCE_Y8) {
            for (uint32_t x = 0; x < width; ++x) source[y * stride + x] = brightness(x, y);
        } else {
            for (uint32_t x = 0; x < width; x += 2)
                source[y * stride + x / 2] = (video_gray_level(brightness(x, y), tv) << 4) |
                                             video_gray_level(brightness(x + 1, y), tv);
        }
    }
    video_frame_descriptor frame = {3, 9, 17, width, height, stride, bytes, format, scale, source, {0}};
    uint8_t *shadow = storage + 32;
    assert(video_pack_frame(shadow, VIDEO_PANEL_BYTES, &frame));
    uint32_t ox = (640 - width * scale) / 2, oy = (480 - height * scale) / 2;
    for (uint32_t y = 0; y < 480; ++y)
        for (uint32_t x = 0; x < 640; ++x) {
            uint8_t actual = shadow[y * 320 + x / 2];
            actual = x % 2 ? actual & 15 : actual >> 4;
            uint8_t expected = x >= ox && x < ox + width * scale &&
                y >= oy && y < oy + height * scale ?
                video_gray_level(brightness((x - ox) / scale, (y - oy) / scale), tv) : 0;
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
                                    VIDEO_SOURCE_Y8, VIDEO_SCALE_DOUBLE, source, {0}};
    for (uint32_t variant = 0; variant < 16; ++variant) {
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
            case 11: f.format |= 128; break;
            case 12: f.format |= VIDEO_SOURCE_UNSIGNALLED; break;
            case 13: f.format = VIDEO_SOURCE_MENU; break;
            case 14: f.format = VIDEO_SOURCE_A4 | VIDEO_SOURCE_DITHER; break;
            case 15: f.format |= 32; break; /* Retired development curve bit. */
        }
        assert(!video_pack_frame(shadow, VIDEO_PANEL_BYTES, &f));
    }
    assert(!video_pack_frame(shadow, VIDEO_PANEL_BYTES - 1, &valid));
    assert(!video_pack_frame(0, VIDEO_PANEL_BYTES, &valid));
    assert(!video_pack_frame(shadow, VIDEO_PANEL_BYTES, 0));
    for (uint32_t i = 0; i < VIDEO_PANEL_BYTES; ++i) assert(shadow[i] == 0xaa);
    free(source); free(shadow);
}
static uint32_t expected_quarter(uint32_t full) {
    uint32_t t = (full * 239 + 510) / 1020 + 2;
    return t < 4 ? 4 : t > 60 ? 60 : t;
}
static void dithered(void) {
    uint8_t source[320 * 192], shadow[VIDEO_PANEL_BYTES];
    const uint32_t threshold[2][2] = {{0, 2}, {3, 1}};
    for (uint32_t sample = 0; sample < 256; ++sample) {
        assert(video_quarter[sample] == expected_quarter(sample));
        for (uint32_t tv = 0; tv <= 1; ++tv) {
            uint32_t full = tv ? video_tv_full[sample] : sample;
            uint32_t t = expected_quarter(full), sum = 0;
            for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) {
                uint32_t expected = (t >> 2) + ((t & 3) > threshold[y][x]);
                if (!expected) expected = 1;
                assert(video_dither_level(sample, tv, x, y) == expected);
                if (sample) assert(expected >= video_dither_level(sample - 1, tv, x, y));
                if (!full || full == 255) assert(expected == (!full ? 1 : 15));
                sum += expected;
            }
            assert(sum == t);
            uint32_t plain = (16 + (full * 239 + 127) / 255) >> 4;
            assert(sum + 2 >= plain * 4 && sum <= plain * 4 + 2);
        }
    }
    for (uint32_t scale = 1; scale <= 2; ++scale) for (uint32_t tv = 0; tv <= 1; ++tv) {
        for (uint32_t i = 0; i < sizeof(source); ++i) source[i] = i;
        video_frame_descriptor f = {1, 1, 1, 320, 192, 320, sizeof(source),
            VIDEO_SOURCE_Y8 | VIDEO_SOURCE_DITHER | (tv ? VIDEO_SOURCE_TV : 0), scale, source, {0}};
        assert(video_pack_frame(shadow, sizeof(shadow), &f));
        uint32_t ox = (640 - 320 * scale) / 2, oy = (480 - 192 * scale) / 2;
        for (uint32_t y = 0; y < 480; ++y) for (uint32_t x = 0; x < 640; ++x) {
            uint32_t actual = x & 1 ? shadow[y * 320 + x / 2] & 15 : shadow[y * 320 + x / 2] >> 4;
            uint32_t expected = 0;
            if (x >= ox && x < ox + 320 * scale && y >= oy && y < oy + 192 * scale) {
                uint32_t full = source[((y - oy) / scale) * 320 + (x - ox) / scale];
                if (tv) full = video_tv_full[full];
                uint32_t t = expected_quarter(full);
                expected = (t >> 2) + ((t & 3) > threshold[y & 1][x & 1]);
                if (!expected) expected = 1;
            }
            assert(actual == expected);
        }
    }
}
static void dither_cells(void) {
    uint8_t source[8], shadow[VIDEO_PANEL_BYTES];
    for (uint32_t tv = 0; tv <= 1; ++tv)
        for (uint32_t phase = 0; phase < 2; ++phase) for (uint32_t sample = 0; sample < 256; ++sample) {
            uint16_t expected = 0;
            for (uint32_t row = 0; row < 2; ++row) for (uint32_t x = 0; x < 2; ++x)
                expected = (expected << 4) | video_dither_level(sample, tv, x, phase + row);
            assert(video_dither_cells[tv][phase][sample] == expected);
            memset(source, sample, sizeof(source));
            uint32_t height = phase ? 1 : 2;
            video_frame_descriptor f = {1, 1, 1, 4, height, 4, sizeof(source),
                VIDEO_SOURCE_Y8 | VIDEO_SOURCE_DITHER | (tv ? VIDEO_SOURCE_TV : 0), VIDEO_SCALE_DOUBLE, source, {0}};
            assert(video_pack_frame(shadow, sizeof(shadow), &f));
            uint32_t y = 240 - height;
            for (uint32_t row = 0; row < height * 2; ++row) for (uint32_t col = 0; col < 4; ++col)
                assert(shadow[(y + row) * VIDEO_PANEL_STRIDE + 158 + col] ==
                       (row & 1 ? expected & 255 : expected >> 8));
            assert(!shadow[y * VIDEO_PANEL_STRIDE + 157] && !shadow[y * VIDEO_PANEL_STRIDE + 162]);
        }
}

static void mode_changes(void) {
    uint8_t *source = malloc(640 * 384), *storage = malloc(VIDEO_PANEL_BYTES + 64);
    assert(source && storage);
    memset(storage, 0xa5, VIDEO_PANEL_BYTES + 64);
    uint8_t *shadow = storage + 32;
    const uint32_t geometry[6][3] = {{320,192,2},{320,192,1},{4,1,1},{4,2,2},{640,384,1},{320,191,2}};
    for (uint32_t tv = 0; tv <= 1; ++tv) for (uint32_t type = 1; type <= 3; ++type)
            for (uint32_t mode = 0; mode < 6; ++mode) {
                uint32_t width = geometry[mode][0], height = geometry[mode][1], scale = geometry[mode][2];
                uint32_t sample = (mode * 37 + type * 17) & 255;
                uint32_t f = tv ? (sample <= 16 ? 0 : sample >= 235 ? 255 :
                    ((sample - 16) * 255 + 109) / 219) : sample;
                uint32_t level = (16 + (f * 239 + 127) / 255) >> 4;
                uint32_t format = (type == 2 ? VIDEO_SOURCE_A4 : VIDEO_SOURCE_Y8) |
                    (tv ? VIDEO_SOURCE_TV : 0) |
                    (type == 3 ? VIDEO_SOURCE_DITHER : 0);
                uint32_t stride = type == 2 ? width / 2 : width;
                memset(source, type == 2 ? level * 17 : sample, 640 * 384);
                video_frame_descriptor frame = {1, 1, 1, width, height, stride, 640 * 384, format, scale, source, {0}};
                /* A menu occupies bottom margins; the next complete media frame
                 * must clear it without depending on a freshly zeroed shadow. */
                memset(shadow + 432 * VIDEO_PANEL_STRIDE, 0xab, 48 * VIDEO_PANEL_STRIDE);
                assert(video_pack_frame(shadow, VIDEO_PANEL_BYTES, &frame));
                uint32_t ox = (640 - width * scale) / 2, oy = (480 - height * scale) / 2;
                const uint32_t thresholds[2][2] = {{0,2},{3,1}};
                uint32_t t = expected_quarter(f);
                for (uint32_t y = 0; y < 480; ++y) for (uint32_t x = 0; x < 640; ++x) {
                    uint32_t expected = 0;
                    if (x >= ox && x < ox + width * scale && y >= oy && y < oy + height * scale)
                        expected = type == 3 ? (t >> 2) + ((t & 3) > thresholds[y & 1][x & 1]) : level;
                    uint32_t actual = shadow[y * VIDEO_PANEL_STRIDE + x / 2];
                    actual = x & 1 ? actual & 15 : actual >> 4;
                    assert(actual == expected);
                }
                for (uint32_t i = 0; i < 32; ++i)
                    assert(storage[i] == 0xa5 && storage[VIDEO_PANEL_BYTES + 32 + i] == 0xa5);
            }
    free(source); free(storage);
}

static void plain_cells(void) {
    uint8_t source[16], guarded[VIDEO_PANEL_BYTES + 2];
    for (uint32_t sample = 0; sample < 256; ++sample) {
        uint32_t first = (sample >> 4) * 17, second = (sample & 15) * 17;
        assert(video_plain_cells[sample] == (first << 8 | second));
        for (uint32_t height = 1; height <= 3; ++height) for (uint32_t tv = 0; tv < 2; ++tv) {
            memset(source, sample, sizeof(source));
            memset(guarded, 0xcd, sizeof(guarded));
            uint8_t *shadow = guarded + 1; /* Also permit unaligned destinations. */
            video_frame_descriptor frame = {1, 1, 1, 4, height, 3, sizeof(source),
                VIDEO_SOURCE_A4 | (tv ? VIDEO_SOURCE_TV : 0), VIDEO_SCALE_DOUBLE, source, {0}};
            assert(video_pack_frame(shadow, VIDEO_PANEL_BYTES, &frame));
            uint32_t y = 240 - height;
            for (uint32_t row = 0; row < 2 * height; ++row) {
                uint8_t *out = shadow + (y + row) * VIDEO_PANEL_STRIDE;
                assert(out[158] == first && out[159] == second && out[160] == first && out[161] == second);
                assert(!out[157] && !out[162]);
            }
            assert(guarded[0] == 0xcd && guarded[VIDEO_PANEL_BYTES + 1] == 0xcd);
        }
    }
}

static void picture_margins(void) {
    uint8_t source[320 * 192], shadow[VIDEO_PANEL_BYTES];
    const uint16_t sizes[3][2] = {{320,192}, {320,176}, {144,240}};
    for (uint32_t size = 0; size < 3; ++size) for (uint32_t scale = 1; scale <= 2; ++scale)
        for (uint32_t mode = 0; mode < 3; ++mode) for (uint32_t edge = 0; edge < 3; ++edge) {
            uint32_t width = sizes[size][0], height = sizes[size][1];
            uint32_t type = mode == 0 ? VIDEO_SOURCE_A4 : VIDEO_SOURCE_Y8;
            memset(source, mode == 0 ? 0x11 : 0, sizeof(source));
            memset(shadow, 0xff, sizeof(shadow));
            video_frame_descriptor f = {1,1,1,width,height,type == VIDEO_SOURCE_A4 ? width/2 : width,
                sizeof(source),type | (mode == 2 ? VIDEO_SOURCE_DITHER : 0),scale,source,
                {4,6,width-8,height-12}};
            if (edge == 1) f.picture = (video_picture_rect){3,0,width-7,height};
            if (edge == 2) f.picture = (video_picture_rect){4,3,width-9,height-7};
            assert(video_pack_frame(shadow, sizeof(shadow), &f));
            uint32_t x0 = (640-width*scale)/2 + f.picture.x*scale;
            uint32_t y0 = (480-height*scale)/2 + f.picture.y*scale;
            for (uint32_t y = 0; y < 480; ++y) for (uint32_t x = 0; x < 640; ++x) {
                uint32_t level = shadow[y*320+x/2]; level = x&1 ? level&15 : level>>4;
                assert(level == (x >= x0 && x < x0+f.picture.width*scale &&
                                 y >= y0 && y < y0+f.picture.height*scale ? 1u : 0u));
            }
            f.picture.x = width;
            memset(shadow, 0xa5, sizeof(shadow));
            assert(!video_pack_frame(shadow, sizeof(shadow), &f));
            for (uint32_t i = 0; i < sizeof(shadow); ++i) assert(shadow[i] == 0xa5);
        }
}

static void picture_styles(void) {
    const uint32_t threshold[2][2] = {{0,2},{3,1}};
    uint8_t pixels[8], shadow[VIDEO_PANEL_BYTES];
    for (uint32_t style = 0; style < 4; ++style)
    for (uint32_t tv = 0; tv < 2; ++tv) for (uint32_t sample = 0; sample < 256; ++sample) {
        uint32_t full = tv ? video_tv_full[sample] : sample;
        const uint32_t floors[] = {4,0,1,2};
        uint32_t t = full < 7 ? floors[style]+(4-floors[style])*full/7 : expected_quarter(full);
        uint32_t sum = 0;
        for (uint32_t y = 0; y < 2; ++y) for (uint32_t x = 0; x < 2; ++x) {
            uint32_t level = video_dither_tuned(sample,tv,x,y,style);
            assert(level == (t>>2)+((t&3)>threshold[y][x]));
            assert(video_dither_tuned(sample,tv,x,y,0) == video_dither_level(sample,tv,x,y));
            if (sample) assert(level >= video_dither_tuned(sample-1,tv,x,y,style));
            if (full >= 7) assert(level == video_dither_level(sample,tv,x,y));
            sum += level;
        }
        assert(sum == t && sum >= floors[style] && sum <= 60);
        if (!full) assert(sum == floors[style]);
        if (sample > 7 && sample != 16 && sample != 235 && sample != 255) continue;
        memset(pixels,sample,sizeof(pixels));
        for (uint32_t scale = 1; scale <= 2; ++scale) for (uint32_t height = 1; height <= 2; ++height)
            for (uint32_t odd = 0; odd < 2; ++odd) {
                video_frame_descriptor f = {1,1,1,4,height,4,sizeof(pixels),VIDEO_SOURCE_Y8 |
                    VIDEO_SOURCE_DITHER | (style << VIDEO_SOURCE_STYLE_SHIFT) | (tv ? VIDEO_SOURCE_TV : 0),scale,pixels,{0}};
                f.picture = (video_picture_rect){odd,0,4-odd,height};
                memset(shadow,0xef,sizeof(shadow));
                assert(video_pack_frame(shadow,sizeof(shadow),&f));
                uint32_t ox = (640-4*scale)/2, oy = (480-height*scale)/2;
                for (uint32_t y = 0; y < 480; ++y) for (uint32_t x = 0; x < 640; ++x) {
                    uint32_t level = shadow[y*320+x/2]; level = x&1 ? level&15 : level>>4;
                    uint32_t expected = x >= ox+odd*scale && x < ox+4*scale && y >= oy && y < oy+height*scale ?
                        video_dither_tuned(sample,tv,x,y,style) : 0;
                    assert(level == expected);
                }
            }
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "pixels")) {
        for (uint32_t format = VIDEO_SOURCE_Y8; format <= VIDEO_SOURCE_A4; ++format)
            for (uint32_t scale = VIDEO_SCALE_NATIVE; scale <= VIDEO_SCALE_DOUBLE; ++scale)
                check_pixels(format, scale, 320, 192);
        check_pixels(VIDEO_SOURCE_A4, VIDEO_SCALE_NATIVE, 640, 384);
    } else if (!strcmp(argv[1], "range")) {
        for (uint32_t y = 0; y < 256; ++y) {
            uint32_t v = y <= 16 ? 0 : y >= 235 ? 219 : y - 16;
            uint32_t expected = (v * 255 + 109) / 219;
            assert(video_tv_full[y] == expected);
            assert(video_gray_level(y, 0) == ((16 + (y * 239 + 127) / 255) >> 4));
            assert(video_gray_level(y, 1) == ((16 + (expected * 239 + 127) / 255) >> 4));
        }
        for (uint32_t type = VIDEO_SOURCE_Y8; type <= VIDEO_SOURCE_A4; ++type)
            for (uint32_t scale = 1; scale <= 2; ++scale) {
                check_pixels(type | VIDEO_SOURCE_TV, scale, 320, 192);
                check_pixels(type | VIDEO_SOURCE_TV | VIDEO_SOURCE_UNSIGNALLED, scale, 320, 192);
            }
    } else if (!strcmp(argv[1], "dump-gray")) {
        for (uint32_t y = 0; y < 256; ++y)
            printf("%u %u %u\n", y, video_gray_level(y, 0), video_gray_level(y, 1));
    } else if (!strcmp(argv[1], "picture-styles")) picture_styles();
    else if (!strcmp(argv[1], "picture-margins")) picture_margins();
    else if (!strcmp(argv[1], "margins")) mode_changes();
    else if (!strcmp(argv[1], "plain-cells")) plain_cells();
    else if (!strcmp(argv[1], "cells")) dither_cells();
    else if (!strcmp(argv[1], "dither")) dithered();
    else if (!strcmp(argv[1], "reject")) rejected();
    else if (!strcmp(argv[1], "sides")) {
        assert(video_source_from_side(2) == 1 && video_source_from_side(1) == 2);
        assert(!video_source_from_side(0) && !video_source_from_side(3));
    } else assert(0);
    puts("bounded top-down A4 packing PASS");
    return 0;
}
