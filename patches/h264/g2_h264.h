/* Inert Sub0h264 interface. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    G2_H264_RECORD_BYTES = 4096,
    G2_H264_MAX_NAL_BYTES = 4086,
    G2_H264_RANGE_FULL = 1,
    G2_H264_RANGE_SIGNALLED = 2
};

typedef enum {
    G2_H264_ERROR = -1,
    G2_H264_CONSUMED = 0,
    G2_H264_FRAME_READY = 1
} g2_h264_result;

typedef struct {
    const uint8_t *y;
    uint32_t width, height, stride, count;
    uint32_t range_flags; /* Completed active SPS, not the latest received SPS. */
} g2_h264_frame_info;

typedef struct {
    uint32_t capacity, allocated_frames, allocated_bytes;
} g2_h264_dpb_info;

/* Measuring build: cumulative decoder section cycles since construction.
 * Macroblock totals by kind contain the timed helper calls below them;
 * timer_cost is one empty timer bracket, to subtract per counted call. */
typedef struct {
    uint64_t intra_i, inter_p, intra_p, completion;
    uint64_t motion, residual, transform, intra;
    uint32_t motion_calls, residual_calls, transform_calls, intra_calls;
    uint32_t zero_transforms, pictures, timer_cost;
    uint64_t slice_setup, mb_loop, input; /* Time outside the macroblock totals. */
} g2_h264_sections_info;

/* Object storage only; runtime allocations are additional caller-owned RAM. */
uint32_t g2_h264_size(void);
uint32_t g2_h264_alignment(void);

/* Construct in aligned writable memory, always skipping chroma.
 * Returns NULL for invalid storage or an unbound/incomplete runtime.
 * The same runtime/owner must remain bound through destruction. Calls must be
 * serialized on that owner's worker; this interface has no thread/heap hooks.
 * Never reconstruct an initialized handle without destroying it first. */
void *g2_h264_init(void *memory, uint32_t size);

/* Destroy a valid handle (NULL is allowed); do not free caller object storage. */
void g2_h264_destroy(void *handle);

/* Choose a bounded progressive I/P format before the first input. Without
 * this call the adapter retains the decoder's usual format support. The
 * firmware uses one reference plus one working frame, with no crop or FMO. */
int g2_h264_limit_format(void *handle, uint32_t width, uint32_t height,
                         uint32_t references, uint32_t frame_capacity);

/* Query actual DPB storage, on the decoder worker outside publication locks.
 * Capacity is distinct from lazily allocated planes; no allocation occurs. */
int g2_h264_dpb(const void *handle, g2_h264_dpb_info *info);

/* Copy the section counters, on the decoder worker between decode calls. */
int g2_h264_sections(const void *handle, g2_h264_sections_info *info);

/* Feed one NAL, beginning with its header (no Annex-B prefix), at most 4086 B.
 * CONSUMED acknowledges input without promising a picture. FRAME_READY comes
 * only from the decoder's completion status; NAL and picture counts differ.
 * ERROR invalidates the output view. Restart after a decode/preflight error.
 * All input and any borrowed Y view must be disjoint. */
g2_h264_result g2_h264_decode(void *handle, const uint8_t *nal, uint32_t size);

/* Borrow the completed Y plane from the most recent FRAME_READY call.
 * Valid only until the next decode/destroy. No frame copy or ownership transfer.
 * Returns ERROR and clears info when no completed picture is available. */
g2_h264_result g2_h264_frame(const void *handle, g2_h264_frame_info *info);

#ifdef __cplusplus
}
#endif
