/* Bounded coded frames and their lit picture. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

typedef struct { uint16_t x, y, width, height; } video_picture_rect;
typedef struct { uint16_t width, height; video_picture_rect picture; } video_geometry;

/* Mid sizes fill the panel's full width (stretch 640 / width). Each coded height
 * is the 16:9 picture height rounded up to whole macroblocks. */
static inline int video_geometry_full_width(video_geometry g) {
    return (g.width == 384 && g.height == 224) || (g.width == 448 && g.height == 256) ||
           (g.width == 480 && g.height == 272) || (g.width == 512 && g.height == 288);
}

static inline int video_geometry_valid(video_geometry g) {
    return ((g.width == 320 && (g.height == 192 || g.height == 176)) ||
            (g.width == 144 && g.height == 240) || video_geometry_full_width(g)) &&
        g.picture.width && g.picture.height &&
        g.picture.x + g.picture.width <= g.width && g.picture.y + g.picture.height <= g.height;
}

static inline video_geometry video_geometry_default(void) {
    return (video_geometry){320, 192, {0, 0, 320, 192}};
}
