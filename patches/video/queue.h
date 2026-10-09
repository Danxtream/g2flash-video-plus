/* Owned NAL snapshots. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_QUEUE_CONFLICT = -1,
    VIDEO_QUEUE_STALE = -2,
    VIDEO_QUEUE_WRAP = -3,
    VIDEO_QUEUE_INITIAL = 4,
    VIDEO_QUEUE_MAX = 6,
    VIDEO_SLOT_BYTES = 4096,
    VIDEO_NAL_HEADER_BYTES = 10,
    VIDEO_RAW_NAL_BYTES = VIDEO_SLOT_BYTES - VIDEO_NAL_HEADER_BYTES
};

typedef struct {
    uint8_t *bytes;
    uint32_t sequence;
    uint16_t length;
    uint8_t occupied, active;
} video_nal_slot;
typedef struct {
    uint32_t token, accepted, consumed, expected, pictures;
    uint8_t capacity, count;
    video_nal_slot slots[VIDEO_QUEUE_MAX];
} video_nal_queue;
typedef struct {
    const uint8_t *bytes;
    uint32_t token, sequence;
    uint16_t length;
    uint8_t slot;
} video_nal_view;

/* All operations require the owner's publication lock. Buffers are supplied by
 * its allocation ledger, never transport pointers. Private startup needs no
 * lock until publication. No queue operation allocates. */
void video_queue_init(video_nal_queue *, uint32_t token);
/* Publish a prepared slot before accepting input. Capacity increases last. */
int video_queue_attach(video_nal_queue *, uint8_t *bytes);
/* Publish two prepared extension slots together, never a capacity of five. */
int video_queue_extend(video_nal_queue *, uint8_t *bytes);
/* Free credits include the reserved expected slot; future distance is bounded. */
uint32_t video_queue_credits(const video_nal_queue *);
/* Copy expected/bounded future NALs; retain a slot for the missing expected
 * sequence. Positive means accepted or byte-identical replay, zero is full or
 * outside the window; negative distinguishes conflict/stale/wrap. */
int video_queue_push(video_nal_queue *, uint32_t sequence, const uint8_t *, uint16_t);
/* A later decoder consumer borrows only the expected owned slot, until release.
 * Receive-only firmware deliberately never calls these two consumer functions. */
int video_queue_claim(video_nal_queue *, uint32_t token, video_nal_view *);
int video_queue_release(video_nal_queue *, const video_nal_view *);
