/* SPDX-License-Identifier: GPL-3.0-only
 * Request sizes are authoritative; feature tags never select a fixed pool. */
#include "storage.h"
#include <stddef.h>

/* Stock TLSF has four-byte payload alignment and a four-byte physical header.
 * Preserve the raw base for free, with room to align cached working storage. */
static int video_storage_sizes(uint32_t size, uint32_t *raw, uint32_t *charge) {
    if (!size || size > UINT32_MAX - VIDEO_ALLOC_ALIGNMENT - 7u) return 0;
    *raw = size + VIDEO_ALLOC_ALIGNMENT - 1u;
    *charge = ((*raw + 3u) & ~3u) + 4u;
    return 1;
}

static uint8_t video_storage_room(const video_storage *s, uint32_t charge, uint32_t raw,
                                   video_heap_view *cached) {
    const uint32_t heaps[] = {VIDEO_HEAP_CACHED, VIDEO_HEAP_DISPLAY, VIDEO_HEAP_OTHER};
    const uint32_t reserve[] = {VIDEO_CACHED_RESERVE, VIDEO_DISPLAY_RESERVE,
                                VIDEO_OTHER_RESERVE};
    const uint32_t extra[] = {s->cached_allowance, s->display_allowance, 0};
    if (cached) *cached = (video_heap_view){UINT32_MAX, UINT32_MAX};
    if (s->used > s->limit || charge > s->limit - s->used) return VIDEO_STORAGE_LIMIT_REACHED;
    for (uint32_t i = 0; i < 3; ++i) {
        video_heap_view v;
        if (!s->heap.view(s->heap.argument, heaps[i], &v) ||
            v.free_bytes == UINT32_MAX || v.max_alloc == UINT32_MAX ||
            v.max_alloc > v.free_bytes || extra[i] > UINT32_MAX - reserve[i]) return VIDEO_STORAGE_HEAP_VIEW;
        if (!i && cached) *cached = v;
        uint32_t keep = reserve[i] + extra[i];
        uint32_t needed = i == 0 ? charge : 0;
        if (v.free_bytes < keep || needed > v.free_bytes - keep ||
            (i == 0 && raw > v.max_alloc)) return VIDEO_STORAGE_CACHED + i;
    }
    return VIDEO_STORAGE_OK;
}

static void video_storage_account(video_storage *s, uint32_t held) {
    if (s->counter) __atomic_store_n(s->counter, s->used + held, __ATOMIC_RELAXED);
}
void video_storage_bind_counter(video_storage *s, volatile uint32_t *counter) {
    s->counter = counter; video_storage_account(s, 0);
}

int video_storage_init(video_storage *s, const video_heap_ops *heap,
                       uint32_t initial_charge, uint32_t display_allowance) {
    if (!s || !heap || !heap->allocate || !heap->release || !heap->view ||
        initial_charge > VIDEO_STORAGE_LIMIT) return 0;
    s->heap = *heap;
    s->limit = VIDEO_STORAGE_LIMIT;
    s->used = s->peak = s->initial_charge = initial_charge;
    s->cached_allowance = VIDEO_INPUT_ALLOWANCE;
    s->display_allowance = display_allowance;
    s->counter = 0;
    s->refused_heap = (video_heap_view){UINT32_MAX, UINT32_MAX};
    s->refusal = s->refused_live = 0;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i)
        s->entries[i] = (video_allocation){0};
    return video_storage_admit(s);
}

int video_storage_admit(const video_storage *s) {
    return s && s->heap.view && video_storage_room(s, 0, 0, 0) == VIDEO_STORAGE_OK;
}

static video_allocation *video_storage_new_inner(video_storage *s, uint32_t size,
                                           uint32_t tag, int pending) {
    uint32_t raw_size, charge;
    s->refused_live = 0;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) s->refused_live += s->entries[i].live != 0;
    s->refusal = VIDEO_STORAGE_SIZE;
    s->refused_heap = (video_heap_view){UINT32_MAX, UINT32_MAX};
    if (!video_storage_sizes(size, &raw_size, &charge)) return 0;
    s->refusal = video_storage_room(s, charge, raw_size, &s->refused_heap);
    if (s->refusal) return 0;
    video_allocation *entry = 0;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) {
        if (!s->entries[i].live) { entry = &s->entries[i]; break; }
    }
    if (!entry) { s->refusal = VIDEO_STORAGE_SLOTS; return 0; }
    void *raw = s->heap.allocate(s->heap.argument, raw_size);
    if (!raw) { s->refusal = VIDEO_STORAGE_ALLOCATE; return 0; }
    /* The raw allocation exists during the postcheck. Count that held charge
     * before an existing heap read, not as external traffic or a ledger leak. */
    video_storage_account(s, charge);
    /* A racing stock allocation can consume the reserve after our snapshot. */
    if (video_storage_room(s, 0, 0, &s->refused_heap) != VIDEO_STORAGE_OK) {
        s->refusal = VIDEO_STORAGE_RACE;
        s->heap.release(s->heap.argument, raw);
        video_storage_account(s, 0);
        return 0;
    }
    uintptr_t base = (uintptr_t)raw;
    if (base > UINTPTR_MAX - VIDEO_ALLOC_ALIGNMENT + 1u) {
        s->refusal = VIDEO_STORAGE_POINTER;
        s->heap.release(s->heap.argument, raw);
        video_storage_account(s, 0);
        return 0;
    }
    uintptr_t aligned = (base + VIDEO_ALLOC_ALIGNMENT - 1u) &
                        ~(uintptr_t)(VIDEO_ALLOC_ALIGNMENT - 1u);
    *entry = (video_allocation){raw, (void *)aligned, size, charge, tag, 1,
                                (uint8_t)(pending != 0)};
    s->used += charge;
    if (s->used > s->peak) s->peak = s->used;
    return entry;
}

static video_allocation *video_storage_new(video_storage *s, uint32_t size,
                                           uint32_t tag, int pending) {
    video_allocation *e = video_storage_new_inner(s, size, tag, pending);
#ifdef VIDEO_STORAGE_REFUSED
    if (!e) VIDEO_STORAGE_REFUSED();
#endif
    return e;
}

void *video_storage_alloc(video_storage *s, uint32_t size) {
    if (!s || !size) return 0;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) {
        video_allocation *e = &s->entries[i];
        /* Exact size prevents a small temporary allocation stealing a large
         * reserved frame. Larger vector growth still uses the bounded path. */
        if (e->live && e->pending && e->requested == size) {
            s->refusal = video_storage_room(s, 0, 0, &s->refused_heap);
            if (s->refusal) {
#ifdef VIDEO_STORAGE_REFUSED
                VIDEO_STORAGE_REFUSED();
#endif
                s->refused_live = 0;
                for (uint32_t j = 0; j < VIDEO_ALLOCATION_LIMIT; ++j) s->refused_live += s->entries[j].live != 0;
                return 0;
            }
            e->pending = 0;
            return e->aligned;
        }
    }
    video_allocation *e = video_storage_new(s, size, 0, 0);
    return e ? e->aligned : 0;
}

static void video_storage_drop(video_storage *s, video_allocation *e) {
    s->heap.release(s->heap.argument, e->raw);
    s->used -= e->charge;
    video_storage_account(s, 0);
    *e = (video_allocation){0};
}

int video_storage_release(video_storage *s, void *memory) {
    if (!memory) return 1;
    if (!s) return 0;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) {
        video_allocation *e = &s->entries[i];
        if (e->live && !e->pending && e->aligned == memory) {
            video_storage_drop(s, e);
            return 1;
        }
    }
    return 0;
}

void video_storage_finish_call(video_storage *s) {
    if (!s) return;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i)
        if (s->entries[i].live && s->entries[i].pending)
            video_storage_drop(s, &s->entries[i]);
}

int video_storage_preflight(video_storage *s, const g2_h264_request *requests,
                            uint32_t count, g2_h264_failure *failure) {
    if (failure) *failure = (g2_h264_failure){0};
    if (!s || (!requests && count)) return 0;
    video_storage_finish_call(s);
    if (count > VIDEO_ALLOCATION_LIMIT) return 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (!requests[i].size) continue;
        if (!video_storage_new(s, requests[i].size, requests[i].tag, 1)) {
            if (failure) *failure = (g2_h264_failure){requests[i].tag, requests[i].size};
            video_storage_finish_call(s);
            return 0;
        }
    }
    return 1;
}

void video_storage_reclaim(video_storage *s) {
    if (!s) return;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i)
        if (s->entries[i].live) video_storage_drop(s, &s->entries[i]);
}
