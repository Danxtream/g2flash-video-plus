/* Immutable source-luma normalization. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

/* H.264 TV luma [16,235], clamped and rounded to the full [0,255] domain. */
static const uint8_t video_tv_full[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 12, 13, 14, 15, 16, 17,
    19, 20, 21, 22, 23, 24, 26, 27, 28, 29, 30, 31, 33, 34, 35, 36,
    37, 38, 40, 41, 42, 43, 44, 45, 47, 48, 49, 50, 51, 52, 54, 55,
    56, 57, 58, 59, 61, 62, 63, 64, 65, 66, 68, 69, 70, 71, 72, 73,
    75, 76, 77, 78, 79, 80, 82, 83, 84, 85, 86, 87, 88, 90, 91, 92,
    93, 94, 95, 97, 98, 99, 100, 101, 102, 104, 105, 106, 107, 108, 109, 111,
    112, 113, 114, 115, 116, 118, 119, 120, 121, 122, 123, 125, 126, 127, 128, 129,
    130, 132, 133, 134, 135, 136, 137, 139, 140, 141, 142, 143, 144, 146, 147, 148,
    149, 150, 151, 153, 154, 155, 156, 157, 158, 160, 161, 162, 163, 164, 165, 167,
    168, 169, 170, 171, 172, 173, 175, 176, 177, 178, 179, 180, 182, 183, 184, 185,
    186, 187, 189, 190, 191, 192, 193, 194, 196, 197, 198, 199, 200, 201, 203, 204,
    205, 206, 207, 208, 210, 211, 212, 213, 214, 215, 217, 218, 219, 220, 221, 222,
    224, 225, 226, 227, 228, 229, 231, 232, 233, 234, 235, 236, 238, 239, 240, 241,
    242, 243, 245, 246, 247, 248, 249, 250, 252, 253, 254, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
};

/* Rescale into the visible panel range without changing decoded Y. */
static const uint8_t video_rescale[256] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 10, 10, 10, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 11, 11, 11, 11, 11,
    11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12,
    12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 13, 13, 13,
    13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 14, 14,
    14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 15,
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
};

/* Center each cell average on the truncated plain level, retaining endpoints. */
static const uint8_t video_quarter[256] = {
    4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 6,
    6, 6, 6, 6, 7, 7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 9,
    9, 10, 10, 10, 10, 11, 11, 11, 11, 12, 12, 12, 12, 13, 13, 13,
    13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 17, 17,
    17, 17, 17, 18, 18, 18, 18, 19, 19, 19, 19, 20, 20, 20, 20, 21,
    21, 21, 21, 21, 22, 22, 22, 22, 23, 23, 23, 23, 24, 24, 24, 24,
    24, 25, 25, 25, 25, 26, 26, 26, 26, 27, 27, 27, 27, 28, 28, 28,
    28, 28, 29, 29, 29, 29, 30, 30, 30, 30, 31, 31, 31, 31, 32, 32,
    32, 32, 32, 33, 33, 33, 33, 34, 34, 34, 34, 35, 35, 35, 35, 36,
    36, 36, 36, 36, 37, 37, 37, 37, 38, 38, 38, 38, 39, 39, 39, 39,
    39, 40, 40, 40, 40, 41, 41, 41, 41, 42, 42, 42, 42, 43, 43, 43,
    43, 43, 44, 44, 44, 44, 45, 45, 45, 45, 46, 46, 46, 46, 47, 47,
    47, 47, 47, 48, 48, 48, 48, 49, 49, 49, 49, 50, 50, 50, 50, 51,
    51, 51, 51, 51, 52, 52, 52, 52, 53, 53, 53, 53, 54, 54, 54, 54,
    54, 55, 55, 55, 55, 56, 56, 56, 56, 57, 57, 57, 57, 58, 58, 58,
    58, 58, 59, 59, 59, 59, 60, 60, 60, 60, 60, 60, 60, 60, 60, 60,
};

static inline uint8_t video_gray_level(uint8_t y, int tv) {
    return video_rescale[tv ? video_tv_full[y] : y];
}

/* Anchor the Bayer phase to final physical panel coordinates. */
static inline uint8_t video_dither_level(uint8_t y, int tv, uint32_t x, uint32_t row) {
    uint32_t t = video_quarter[tv ? video_tv_full[y] : y];
    uint32_t threshold = (row & 1) ? ((x & 1) ? 1 : 3) : ((x & 1) ? 2 : 0);
    return (t >> 2) + ((t & 3) > threshold);
}

/* Style bits 0..1 select floor 1/0/0.25/0.5. Blend only below the first
 * solid-one point, retaining the approved quarter-step curve above it. */
static inline uint32_t video_dither_quarter(uint8_t y, int tv, uint32_t style) {
    uint32_t full = tv ? video_tv_full[y] : y;
    static const uint8_t floors[4] = {4,0,1,2};
    uint32_t floor = floors[style & 3];
    uint32_t t = full < 7 ? floor + (4-floor)*full/7 : video_quarter[full];
    return t;
}

static inline uint8_t video_dither_tuned(uint8_t y, int tv, uint32_t x, uint32_t row, uint32_t style) {
    uint32_t t = video_dither_quarter(y,tv,style);
    uint32_t threshold = (row & 1) ? ((x & 1) ? 1 : 3) : ((x & 1) ? 2 : 0);
    return (t >> 2) + ((t & 3) > threshold);
}

/* Thresholds by row, then column: video_dither_tuned's 2x2 cell repeated to four
 * columns, and the 4x4 Bayer matrix. */
static const uint8_t video_dither_cell2[2][4] = {{0, 2, 0, 2}, {3, 1, 3, 1}};
static const uint8_t video_dither_cell4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

/* The 4x4 pattern's target for mid sizes. Same curve as video_quarter, level
 * (Y + 8.5) / 17 held to 1..15, but in sixteenths: 225 tones instead of 57, and a
 * flat area stays within 0.19 of a level of the 2x2 average. Floors blend as in
 * video_dither_quarter. */
static inline uint32_t video_dither_sixteenth(uint8_t y, int tv, uint32_t style) {
    static const uint8_t floors[4] = {16, 0, 4, 8};
    uint32_t full = tv ? video_tv_full[y] : y;
    uint32_t floor = floors[style & 3], t = (32 * full + 289) / 34;
    return full < 7 ? floor + (16 - floor) * full / 7 : t < 16 ? 16 : t > 240 ? 240 : t;
}

static inline uint8_t video_dither_4x4(uint8_t y, int tv, uint32_t x, uint32_t row, uint32_t style) {
    uint32_t t = video_dither_sixteenth(y, tv, style);
    return (t >> 4) + ((t & 15) > video_dither_cell4[row & 3][x & 3]);
}
