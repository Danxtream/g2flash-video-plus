/* Bounded cached allocation ownership. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "../h264/runtime.h"

enum {
    VIDEO_HEAP_CACHED = 20,
    VIDEO_HEAP_DISPLAY = 13,
    VIDEO_HEAP_OTHER = 27,
    VIDEO_CACHED_RESERVE = 32768,
    VIDEO_DISPLAY_RESERVE = 32768,
    VIDEO_OTHER_RESERVE = 16384,
    VIDEO_INPUT_ALLOWANCE = 4 * 4096,
    /* The player budget includes the future four input snapshots. */
    VIDEO_STORAGE_LIMIT = 223288 - VIDEO_INPUT_ALLOWANCE,
    VIDEO_ALLOC_ALIGNMENT = 32,
    /* This bounds bookkeeping, not the decoder's number of allocations. */
    VIDEO_ALLOCATION_LIMIT = 64
};

typedef struct { uint32_t free_bytes, max_alloc; } video_heap_view;
typedef struct {
    void *argument;
    void *(*allocate)(void *, uint32_t);
    void (*release)(void *, void *);
    int (*view)(void *, uint32_t heap, video_heap_view *);
} video_heap_ops;

typedef struct {
    void *raw, *aligned;
    uint32_t requested, charge, tag;
    uint8_t live, pending;
} video_allocation;

typedef struct {
    video_heap_ops heap;
    video_allocation entries[VIDEO_ALLOCATION_LIMIT];
    uint32_t limit, used, peak, initial_charge;
    uint32_t cached_allowance, display_allowance;
} video_storage;

/* Initialize a private owner. initial_charge includes its own raw allocation. */
int video_storage_init(video_storage *, const video_heap_ops *, uint32_t initial_charge,
                       uint32_t display_allowance);
/* Validate live heap identities/reserves; approximate views never promise malloc. */
int video_storage_admit(const video_storage *);
/* Allocate aligned, ledger-owned bytes, including untagged temporary vectors. */
void *video_storage_alloc(video_storage *, uint32_t size);
/* Release an exact owned pointer; NULL is allowed, foreign/double frees fail. */
int video_storage_release(video_storage *, void *);
/* Reserve the entire request batch atomically before decoder mutation. */
int video_storage_preflight(video_storage *, const g2_h264_request *, uint32_t count,
                            g2_h264_failure *);
/* Release reservations not consumed by new at a decoder-call boundary. */
void video_storage_finish_call(video_storage *);
/* Reclaim abandoned allocations only after their worker has been terminated. */
void video_storage_reclaim(video_storage *);
