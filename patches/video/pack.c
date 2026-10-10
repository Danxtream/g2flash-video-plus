/* Brightness packing without dithering. SPDX-License-Identifier: GPL-3.0-only */
#include "present.h"
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
    if (!shadow || bytes < VIDEO_PANEL_BYTES || !frame->pixels ||
        !frame->width || !frame->height || frame->width % 4 ||
        (frame->scale != VIDEO_SCALE_NATIVE && frame->scale != VIDEO_SCALE_DOUBLE) ||
        (frame->format != VIDEO_SOURCE_Y8 && frame->format != VIDEO_SOURCE_A4) ||
        frame->width > VIDEO_PANEL_WIDTH / frame->scale ||
        frame->height > VIDEO_PANEL_HEIGHT / frame->scale) return 0;
    uint32_t row_bytes = frame->format == VIDEO_SOURCE_Y8 ? frame->width : frame->width / 2;
    if (frame->stride < row_bytes ||
        frame->height - 1 > (UINT32_MAX - row_bytes) / frame->stride) return 0;
    uint32_t needed = (frame->height - 1) * frame->stride + row_bytes;
    uintptr_t source = (uintptr_t)frame->pixels, destination = (uintptr_t)shadow;
    if (frame->bytes < needed || source > UINTPTR_MAX - needed ||
        destination > UINTPTR_MAX - VIDEO_PANEL_BYTES ||
        (source < destination + VIDEO_PANEL_BYTES && destination < source + needed)) return 0;
    for (uint32_t i = 0; i < VIDEO_PANEL_BYTES; ++i) shadow[i] = 0;
    uint32_t x = (VIDEO_PANEL_WIDTH - frame->width * frame->scale) / 2;
    uint32_t y = (VIDEO_PANEL_HEIGHT - frame->height * frame->scale) / 2;
    for (uint32_t row = 0; row < frame->height; ++row) {
        const uint8_t *in = frame->pixels + row * frame->stride;
        uint8_t *out = shadow + (y + row * frame->scale) * VIDEO_PANEL_STRIDE + x / 2;
        if (frame->scale == VIDEO_SCALE_NATIVE) {
            if (frame->format == VIDEO_SOURCE_Y8) {
                for (uint32_t col = 0; col < frame->width; col += 2)
                    out[col / 2] = (in[col] & 0xf0) | (in[col + 1] >> 4);
            } else {
                for (uint32_t col = 0; col < row_bytes; ++col) out[col] = in[col];
            }
        } else {
            if (frame->format == VIDEO_SOURCE_Y8) {
                for (uint32_t col = 0; col < frame->width; ++col)
                    out[col] = (uint8_t)((in[col] >> 4) * 0x11);
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
