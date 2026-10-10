/* Audited Even 2.2.9.22 services. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>
#include "stack.h"

/* CMSIS osThreadAttr_t on this donor uses a caller-owned 112-byte static TCB.
 * priority 8 yields to normal services; no timer or interrupt suppression. */
typedef struct {
    const char *name;
    uint32_t attr_bits;
    void *cb_mem;
    uint32_t cb_size;
    void *stack_mem;
    uint32_t stack_size;
    int32_t priority;
    uint32_t tz_module, reserved;
} video_thread_attr;

/* CMSIS event attributes use caller-owned storage. This donor's static event
 * group is 32 bytes, with a wait list at offset 4 and a static flag at 28. */
typedef struct {
    const char *name;
    uint32_t attr_bits;
    void *cb_mem;
    uint32_t cb_size;
} video_event_attr;

/* The donor pool copies this 20-byte item before waking one of its existing
 * 4 KiB workers. Payload is NULL; size carries only a generation token. */
typedef struct {
    void (*callback)(uint32_t, const uint8_t *, uint32_t, uint16_t);
    uint32_t app_id;
    uint16_t event, reserved;
    const uint8_t *data;
    uint32_t size;
} video_pool_item;

#ifndef VIDEO_POOL_DISPATCH
/* Bypass only the pool's blocking dispatch wrapper. CMSIS queue Put with a
 * zero timeout copies or refuses without a mutex wait or heap allocation. */
static int video_platform_dispatch(const video_pool_item *item) {
    if (!*(const volatile uint8_t *)0x200773f0U) return 0;
    uint32_t queue = *(const volatile uint32_t *)0x20076decU;
    if (queue != 0x20074d70U ||
        *(const uint32_t *)(uintptr_t)(queue + 60) != 150 ||
        *(const uint32_t *)(uintptr_t)(queue + 64) != sizeof(*item)) return 0;
    return ((int (*)(uint32_t, const void *, uint8_t, uint32_t))0x00443299U)
        (queue, item, 0, 0) == 0;
}
#define VIDEO_POOL_DISPATCH video_platform_dispatch
_Static_assert(sizeof(video_pool_item) == 20, "copied pool item ABI");
#endif

#ifndef VIDEO_OS_THREAD_NEW
#define VIDEO_OS_THREAD_NEW ((uint32_t (*)(void (*)(void *), void *, const video_thread_attr *))0x00442897U)
#define VIDEO_OS_THREAD_ID ((uint32_t (*)(void))0x0044295fU)
#define VIDEO_OS_THREAD_TERMINATE ((int (*)(uint32_t))0x004429b3U)
#define VIDEO_OS_DELAY ((int (*)(uint32_t))0x00442b2bU)
#define VIDEO_OS_MUTEX_NEW ((uint32_t (*)(void *))0x00442ef7U)
#define VIDEO_OS_MUTEX_TAKE ((int (*)(uint32_t, uint32_t))0x00442f91U)
#define VIDEO_OS_MUTEX_GIVE ((int (*)(uint32_t))0x00442ff7U)
#define VIDEO_OS_MUTEX_DELETE ((int (*)(uint32_t))0x00443049U)
#define VIDEO_OS_EVENT_NEW ((uint32_t (*)(const video_event_attr *))0x00442d45U)
#define VIDEO_OS_EVENT_SET ((uint32_t (*)(uint32_t, uint32_t))0x00442d99U)
#define VIDEO_OS_EVENT_WAIT ((uint32_t (*)(uint32_t, uint32_t, uint32_t, uint32_t))0x00442e77U)
#define VIDEO_TICK FW_MS_TICK
#define VIDEO_OWNER_ALLOC cfw_malloc
#define VIDEO_OWNER_FREE FW_FREE

#ifndef VIDEO_PLATFORM_STACK_SAMPLE
/* This donor has no CMSIS stack-space wrapper. Authenticate the current task
 * against its eight static pool TCBs before reading the 4 KiB sentinel area.
 * TCB offsets 48/84/109 hold stack base/word count/static-storage flag. */
static int video_platform_stack_sample(uint32_t *task, uint32_t *used, uint32_t *bytes) {
    uint32_t id = VIDEO_OS_THREAD_ID();
    int index = video_stack_pool_index(id);
    if (index < 0) return 0;
    const volatile uint32_t *tcb = (const void *)(uintptr_t)id;
    uint32_t base = VIDEO_POOL_STACKS + (uint32_t)index * VIDEO_POOL_STACK_BYTES;
    if (!video_stack_pool_valid(id, ((const volatile uint32_t *)VIDEO_POOL_IDS)[index],
        tcb[12], tcb[21], ((const volatile uint8_t *)tcb)[109])) return 0;
    *task = id; *bytes = VIDEO_POOL_STACK_BYTES;
    *used = VIDEO_POOL_STACK_BYTES - video_stack_unused((const void *)(uintptr_t)base,
                                                      VIDEO_POOL_STACK_BYTES);
    return 1;
}
#define VIDEO_PLATFORM_STACK_SAMPLE video_platform_stack_sample
#endif

/* Descriptor/arena identities authenticated by the exact donor-image guard.
 * The other heap excludes the existing 1 KiB context-anchor tail. */
enum {
    VIDEO_CACHED_DESCRIPTOR = 0x20076e08U,
    VIDEO_CACHED_ARENA = 0x202020a8U,
    VIDEO_CACHED_ARENA_BYTES = 0x70800U,
    VIDEO_DISPLAY_ARENA = 0x201350a8U,
    VIDEO_DISPLAY_ARENA_BYTES = 0xcd000U,
    VIDEO_OTHER_DESCRIPTOR = 0x2000033cU,
    VIDEO_OTHER_ARENA = 0x202728a8U,
    VIDEO_OTHER_ARENA_BYTES = 0x2cc00U
};
static int video_platform_heap_view(void *argument, uint32_t heap, video_heap_view *out) {
    (void)argument;
    cfw_heap_stats stats;
    if (heap == VIDEO_HEAP_DISPLAY)
        stats = heap_object_stats(FW_HEAP_13_DESCRIPTOR, VIDEO_DISPLAY_ARENA,
                                  VIDEO_DISPLAY_ARENA_BYTES);
    else if (heap == VIDEO_HEAP_OTHER)
        stats = heap_object_stats(VIDEO_OTHER_DESCRIPTOR, VIDEO_OTHER_ARENA,
                                  VIDEO_OTHER_ARENA_BYTES);
    else if (heap == VIDEO_HEAP_CACHED &&
             CFW_HEAP_READ32(VIDEO_CACHED_DESCRIPTOR) == VIDEO_CACHED_ARENA)
        stats = tlsf_arena_stats(VIDEO_CACHED_ARENA, VIDEO_CACHED_ARENA_BYTES);
    else return 0;
    *out = (video_heap_view){stats.free_bytes, stats.max_alloc};
    return stats.free_bytes != TLSF_FREE_INVALID;
}
static void *video_platform_allocate(void *argument, uint32_t size) {
    (void)argument;
    return cfw_malloc(size);
}
static void video_platform_release(void *argument, void *memory) {
    (void)argument;
    FW_FREE(memory);
}
/* This donor's CMSIS block exposes no event-delete entry. Static groups have no
 * global registry: after all task-context users have drained, their empty
 * wait list and caller-owned flag permit reclaiming the supplied storage.
 * ISR setters are forbidden because they defer a pointer to the timer queue. */
static int video_platform_event_quiescent(uint32_t event) {
    const uint8_t *control = (const void *)(uintptr_t)event;
    return !event || (control[28] == 1 && *(const uint32_t *)(control + 4) == 0);
}
_Static_assert(sizeof(video_thread_attr) == 36, "static thread attribute ABI");
_Static_assert(sizeof(video_event_attr) == 16, "static event attribute ABI");
#endif
