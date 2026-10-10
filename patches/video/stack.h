/* Bounded donor stack diagnostics. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

enum {
    VIDEO_POOL_TCBS = 0x2006e168U, VIDEO_POOL_STACKS = 0x20033da0U,
    VIDEO_POOL_IDS = 0x20075a74U, VIDEO_POOL_TASKS = 8,
    VIDEO_POOL_TCB_BYTES = 112, VIDEO_POOL_STACK_BYTES = 4096
};

/* Reject non-pool or misaligned IDs before dereferencing any task metadata. */
static inline int video_stack_pool_index(uint32_t id) {
    if (id < VIDEO_POOL_TCBS || id >= VIDEO_POOL_TCBS + VIDEO_POOL_TASKS * VIDEO_POOL_TCB_BYTES ||
        (id - VIDEO_POOL_TCBS) % VIDEO_POOL_TCB_BYTES) return -1;
    return (id - VIDEO_POOL_TCBS) / VIDEO_POOL_TCB_BYTES;
}

/* The exact donor owns this static allocation for the task's lifetime. */
static inline int video_stack_pool_valid(uint32_t id, uint32_t stored_id,
                                         uint32_t base, uint32_t words, uint8_t flag) {
    int index = video_stack_pool_index(id);
    return index >= 0 && id == stored_id && flag == 2 &&
        base == VIDEO_POOL_STACKS + (uint32_t)index * VIDEO_POOL_STACK_BYTES &&
        words == VIDEO_POOL_STACK_BYTES / 4;
}

/* FreeRTOS fills downward-growing stacks with 0xa5. Restrict reads to the
 * authenticated allocation, even if its sentinel is entirely untouched. */
static inline uint32_t video_stack_unused(const volatile uint8_t *stack, uint32_t bytes) {
    uint32_t unused = 0;
    while (unused < bytes && stack[unused] == 0xa5) ++unused;
    return unused & ~3u; /* Match the donor's whole-word high-water units. */
}
