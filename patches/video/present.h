/* Bounded shared-shadow presentation. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_PANEL_WIDTH = 640,
    VIDEO_PANEL_HEIGHT = 480,
    VIDEO_PANEL_STRIDE = VIDEO_PANEL_WIDTH / 2,
    VIDEO_PANEL_BYTES = VIDEO_PANEL_STRIDE * VIDEO_PANEL_HEIGHT,
    VIDEO_SOURCE_Y8 = 1,
    VIDEO_SOURCE_A4 = 2,
    VIDEO_SCALE_NATIVE = 1,
    VIDEO_SCALE_DOUBLE = 2
};

/* Packing consumes borrowed Y immediately. A later queued presenter can supply
 * owned A4 through the same descriptor without retaining a decoder plane. */
typedef struct {
    uint32_t token, generation, ordinal;
    uint32_t width, height, stride, bytes, format, scale;
    const uint8_t *pixels;
} video_frame_descriptor;

/* Clear margins and pack top-down pixels, high nibble first. Refuse malformed
 * geometry, insufficient storage and overlap before changing the shadow. */
int video_pack_frame(uint8_t *shadow, uint32_t bytes,
                     const video_frame_descriptor *frame);
/* Stock side values differ from the transport's left/right source bits. */
uint32_t video_source_from_side(uint32_t side);
