/* Owned completed-picture FIFO. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "present.h"
#include "timeline.h"

enum {
    VIDEO_DECODED_DEPTH = 2,
    VIDEO_DECODED_A4_BYTES = 30720,
    VIDEO_DECODED_Y8_BYTES = 61440,
    VIDEO_DECODED_FREE = 0,
    VIDEO_DECODED_FILLING,
    VIDEO_DECODED_READY,
    VIDEO_DECODED_PRESENTING
};

typedef struct {
    video_frame_time media;
    uint8_t *pixels;
    uint32_t serial, state, copied, range;
} video_decoded_slot;

/* Caller provides an admitted owned allocation; no decoder plane is retained.
 * Reservation/publication/claim/release run under the owner's publication lock.
 * Producer copying runs unlocked while its single filling slot is reserved. */
typedef struct {
    uint32_t generation, token, format, depth, slot_bytes, bytes;
    uint32_t head, count, serial, accepted, released, cancelled;
    video_geometry geometry;
    video_decoded_slot slots[VIDEO_DECODED_DEPTH];
} video_decoded_queue;

/* Initialize only after full mode-specific heap admission; depth is explicit. */
int video_decoded_init(video_decoded_queue *, uint32_t generation, uint32_t token,
                       uint32_t format, uint32_t depth, uint8_t *, uint32_t bytes);
/* Configure only an empty, unpublished queue; never grow past the admitted buffer.
 * A mid size lays its slots out at its own 4-bit size. */
int video_decoded_geometry(video_decoded_queue *, video_geometry);
/* Initialize media high marks for an explicit hidden seek prefix, not copies. */
int video_decoded_skip_prefix(video_decoded_queue *, uint32_t generation, uint32_t prefix);
/* Reserve the next FIFO picture. Zero means full, stale or exhausted; never overwrite. */
uint32_t video_decoded_reserve(video_decoded_queue *, uint32_t generation,
                              const video_frame_time *);
/* Copy/quantize borrowed Y before the producer's next decode. No heap or wait. */
int video_decoded_copy(video_decoded_queue *, uint32_t generation, uint32_t serial,
                       const uint8_t *y, uint32_t stride, uint32_t bytes);
/* Retain completed-picture range flags with the owned pixels, never an SPS pointer. */
int video_decoded_copy_range(video_decoded_queue *, uint32_t generation, uint32_t serial,
                             const uint8_t *y, uint32_t stride, uint32_t bytes, uint32_t range);
/* Publish only a finished copy with the matching finite reservation. */
int video_decoded_publish(video_decoded_queue *, uint32_t generation, uint32_t serial);
/* Claim exactly the head frame named by the coordinator; no independent advance. */
int video_decoded_claim(video_decoded_queue *, uint32_t generation, uint32_t ordinal,
                        uint32_t scale, video_frame_descriptor *, video_frame_time *);
/* A claimed buffer is reusable only after actual display-copy completion. */
int video_decoded_release(video_decoded_queue *, uint32_t generation, uint32_t ordinal);
/* Stop admission. False means a presenter still owns a slot; allocation stays pinned. */
int video_decoded_cancel(video_decoded_queue *, uint32_t generation);
