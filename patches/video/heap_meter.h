/* Passive snapshots at existing heap reads. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "storage.h"
enum { VIDEO_HEAP_DIAGNOSTICS = 9 };

typedef struct {
    uint32_t baseline, minimum, min_largest, decoder_at_min, presenter_at_min;
    uint32_t external_max, largest_at_min, samples;
} video_heap_meter_row;
typedef struct {
    volatile uint32_t busy, dropped;
    uint32_t generation, stream, token;
    volatile uint32_t decoder, presenter;
    video_heap_meter_row rows[3];
} video_heap_meter;

/* No heap read, allocation, wait or timer; invalid views are not samples. */
void video_heap_meter_sample(video_heap_meter *, uint32_t generation, uint32_t stream,
    uint32_t token, uint32_t heap_index, const video_heap_view *);
/* Freeze retained per-stream evidence into existing 128-byte replay storage. */
int video_heap_meter_snapshot(video_heap_meter *, uint32_t stream, uint8_t lens, uint8_t *);
