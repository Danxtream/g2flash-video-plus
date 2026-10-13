/* Brightness packing in final panel coordinates. SPDX-License-Identifier: GPL-3.0-only */
#include "present.h"
#include "brightness.h"
#include "dither_cells.h"
#include <stddef.h>

#pragma clang section text=".text.video.pack"

uint32_t video_source_from_side(uint32_t side) {
    return side == 2 ? 1 : side == 1 ? 2 : 0;
}

int video_pack_frame(uint8_t *shadow, uint32_t bytes,
                     const video_frame_descriptor *frame) {
    if (!frame) return 0;
    const video_frame_descriptor descriptor = *frame;
    frame = &descriptor;
    uint32_t type = frame->format & VIDEO_SOURCE_TYPE_MASK;
    int tv = (frame->format & VIDEO_SOURCE_TV) != 0;
    int dither = (frame->format & VIDEO_SOURCE_DITHER) != 0;
    int raw = (frame->format & VIDEO_SOURCE_RAW_GRAY) != 0;
    if (!shadow || bytes < VIDEO_PANEL_BYTES || !frame->pixels ||
        !frame->width || !frame->height || frame->width % 4 ||
        (frame->scale != VIDEO_SCALE_NATIVE && frame->scale != VIDEO_SCALE_DOUBLE) ||
        (type != VIDEO_SOURCE_Y8 && type != VIDEO_SOURCE_A4) ||
        (frame->format & ~(VIDEO_SOURCE_TYPE_MASK | VIDEO_SOURCE_TV | VIDEO_SOURCE_UNSIGNALLED | VIDEO_SOURCE_DITHER | VIDEO_SOURCE_RAW_GRAY)) ||
        (dither && type != VIDEO_SOURCE_Y8) ||
        ((frame->format & VIDEO_SOURCE_UNSIGNALLED) && !tv) ||
        frame->width > VIDEO_PANEL_WIDTH / frame->scale ||
        frame->height > VIDEO_PANEL_HEIGHT / frame->scale) return 0;
    uint32_t row_bytes = type == VIDEO_SOURCE_Y8 ? frame->width : frame->width / 2;
    if (frame->stride < row_bytes ||
        frame->height - 1 > (UINT32_MAX - row_bytes) / frame->stride) return 0;
    uint32_t needed = (frame->height - 1) * frame->stride + row_bytes;
    uintptr_t source = (uintptr_t)frame->pixels, destination = (uintptr_t)shadow;
    if (frame->bytes < needed || source > UINTPTR_MAX - needed ||
        destination > UINTPTR_MAX - VIDEO_PANEL_BYTES ||
        (source < destination + VIDEO_PANEL_BYTES && destination < source + needed)) return 0;
    uint32_t x = (VIDEO_PANEL_WIDTH - frame->width * frame->scale) / 2;
    uint32_t y = (VIDEO_PANEL_HEIGHT - frame->height * frame->scale) / 2;
    /* The entire byte-aligned picture is overwritten below. Only margins can
     * retain the old scale, overlay or image, including odd-height centering. */
    uint32_t first = y * VIDEO_PANEL_STRIDE;
    uint32_t last = (y + frame->height * frame->scale) * VIDEO_PANEL_STRIDE;
    uint32_t left = x / 2, right = left + frame->width * frame->scale / 2;
    for (uint32_t i = 0; i < first; ++i) shadow[i] = 0;
    for (uint32_t i = last; i < VIDEO_PANEL_BYTES; ++i) shadow[i] = 0;
    for (uint32_t row = first; row < last; row += VIDEO_PANEL_STRIDE) {
        for (uint32_t col = 0; col < left; ++col) shadow[row + col] = 0;
        for (uint32_t col = right; col < VIDEO_PANEL_STRIDE; ++col) shadow[row + col] = 0;
    }
    if (dither) {
        if (frame->scale == VIDEO_SCALE_DOUBLE) {
            /* Width is a multiple of four, so the centered x is always even.
             * Each source load writes its complete two-row Bayer cell once. */
            const uint16_t *cells = video_dither_cells[tv && !raw][y & 1];
            for (uint32_t row = 0; row < frame->height; ++row) {
                const uint8_t *in = frame->pixels + row * frame->stride;
                uint8_t *out = shadow + (y + row * 2) * VIDEO_PANEL_STRIDE + x / 2;
                for (uint32_t col = 0; col < frame->width; ++col) {
                    uint16_t cell = cells[in[col]];
                    out[col] = cell >> 8;
                    out[VIDEO_PANEL_STRIDE + col] = cell;
                }
            }
            return 1;
        }
        for (uint32_t row = 0; row < frame->height * frame->scale; ++row) {
            const uint8_t *in = frame->pixels + (row / frame->scale) * frame->stride;
            uint8_t *out = shadow + (y + row) * VIDEO_PANEL_STRIDE + x / 2;
            for (uint32_t col = 0; col < frame->width * frame->scale; col += 2)
                out[col / 2] = (video_dither_curve(in[col / frame->scale], tv, raw, x + col, y + row) << 4) |
                    video_dither_curve(in[(col + 1) / frame->scale], tv, raw, x + col + 1, y + row);
        }
        return 1;
    }
    for (uint32_t row = 0; row < frame->height; ++row) {
        const uint8_t *in = frame->pixels + row * frame->stride;
        uint8_t *out = shadow + (y + row * frame->scale) * VIDEO_PANEL_STRIDE + x / 2;
        if (frame->scale == VIDEO_SCALE_NATIVE) {
            if (type == VIDEO_SOURCE_Y8) {
                for (uint32_t col = 0; col < frame->width; col += 2)
                    out[col / 2] = (video_gray_curve(in[col], tv, raw) << 4) |
                                   video_gray_curve(in[col + 1], tv, raw);
            } else {
                for (uint32_t col = 0; col < row_bytes; ++col) out[col] = in[col];
            }
        } else {
            if (type == VIDEO_SOURCE_Y8) {
                for (uint32_t col = 0; col < frame->width; ++col)
                    out[col] = (uint8_t)(video_gray_curve(in[col], tv, raw) * 0x11);
            } else {
                for (uint32_t col = 0; col < row_bytes; ++col) {
                    out[col * 2] = (uint8_t)((in[col] >> 4) * 0x11);
                    out[col * 2 + 1] = (uint8_t)((in[col] & 15) * 0x11);
                }
            }
            for (uint32_t col = 0; col < frame->width; ++col)
                out[VIDEO_PANEL_STRIDE + col] = out[col];
        }
    }
    return 1;
}

#pragma clang section text=""
