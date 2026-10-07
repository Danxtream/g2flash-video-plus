/* Decoder allocation boundary. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tags are the vendored AllocationTag values; zero means no failure. */
typedef struct { uint32_t tag, size; } g2_h264_request;
typedef struct { uint32_t tag, size; } g2_h264_failure;

typedef enum {
    G2_H264_FAIL_ALLOC = 1,
    G2_H264_FAIL_BAD_ALLOC = 2,
    G2_H264_FAIL_ARRAY_LENGTH = 3,
    G2_H264_FAIL_LENGTH = 4,
    G2_H264_FAIL_FUNCTION = 5
} g2_h264_fail_reason;

typedef struct {
    /* Return suitably aligned storage, or NULL. release must accept every
     * allocation from this owner, including small temporary STL vectors. */
    void *(*alloc)(uint32_t size);
    void (*release)(void *memory);
    /* Reserve/check the entire requested batch before vector state mutates.
     * Return zero and fill failure on refusal. This does not replace alloc. */
    int (*preflight)(const g2_h264_request *, uint32_t, g2_h264_failure *);
    /* Must not return or unwind C++: report and park the worker. Its owner
     * must terminate it before reclaiming a partially constructed decoder.
     * The firmware worker integration supplies that lifecycle and the bounded
     * cached allocator. */
    void (*fail)(uint32_t reason);
} g2_h264_runtime;

/* Weak default is unbound (NULL). The future firmware owner supplies a strong
 * implementation using its writable context, never a fixed RAM address.
 * This provider must remain stable during all calls and destruction. */
#if defined(__arm__)
__attribute__((visibility("hidden")))
#endif
const g2_h264_runtime *g2_h264_runtime_current(void);

#ifdef __cplusplus
}
#endif
