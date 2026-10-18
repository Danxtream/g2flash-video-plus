/* Retained decoder input evidence. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum { VIDEO_FAILURE_DIAGNOSTICS = 7, VIDEO_FAILURE_HISTORY = 8,
       VIDEO_FAILURE_BYTES = 128 };

typedef struct {
    uint32_t sequence, checksum;
    uint16_t length;
    uint8_t type, reserved;
} video_failure_nal;

typedef struct {
    volatile uint32_t valid;
    uint8_t detail[VIDEO_FAILURE_BYTES], history[VIDEO_FAILURE_BYTES];
} video_failure_record;

/* FNV-1a over exact raw NAL bytes, without Annex B prefix. Same phone algorithm.
 * This is diagnostic evidence, not a replacement for the transport CRC. */
static inline uint32_t video_failure_checksum(const uint8_t *bytes, uint32_t count) {
    uint32_t hash = 2166136261u;
    for (uint32_t i = 0; i < count; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
