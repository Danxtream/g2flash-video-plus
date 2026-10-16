/* Bounded coded frames and their lit picture. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

typedef struct { uint16_t x, y, width, height; } video_picture_rect;
typedef struct { uint16_t width, height; video_picture_rect picture; } video_geometry;

static inline int video_geometry_valid(video_geometry g) {
    return ((g.width == 320 && (g.height == 192 || g.height == 176)) ||
            (g.width == 144 && g.height == 240)) && g.picture.width && g.picture.height &&
        g.picture.x + g.picture.width <= g.width && g.picture.y + g.picture.height <= g.height;
}

static inline video_geometry video_geometry_default(void) {
    return (video_geometry){320, 192, {0, 0, 320, 192}};
}
