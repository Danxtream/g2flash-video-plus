/* Audited Even 2.2.9.22 services. SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdint.h>

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
