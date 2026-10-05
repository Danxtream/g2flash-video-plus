#pragma once
#include "capsule.h"

/* A preflight leases slots that were all reserved before the worker started.
 * No firmware allocator is called from C++ new/delete or frame timing. */
#define DS_POOL_SLOTS 34U
typedef struct {
    uint8_t *data;
    uint32_t tag, capacity, requested;
    uint8_t leased, pending;
} ds_pool_slot;
typedef struct {
    ds_pool_slot slots[DS_POOL_SLOTS];
    uint8_t queue[8];
    uint32_t count, queued, cursor, highwater, live;
} ds_pool;

static int ds_pool_add(ds_pool *p, uint32_t tag, uint8_t *data, uint32_t size) {
    if (p->count == DS_POOL_SLOTS || !size || !data) return 0;
    ds_pool_slot *s = &p->slots[p->count++];
    s->data = data; s->tag = tag; s->capacity = size;
    s->requested = 0; s->leased = s->pending = 0;
    return 1;
}
static int ds_pool_preflight(ds_pool *p, const ds_request *r, uint32_t n, ds_failure *failure) {
    if (!r || n > 8 || p->cursor != p->queued) return 0;
    p->cursor = p->queued = 0;
    for (uint32_t i = 0; i < n; ++i) {
        if (!r[i].size) continue;
        uint32_t j;
        for (j = 0; j < p->count; ++j) {
            ds_pool_slot *s = &p->slots[j];
            if (s->tag == r[i].tag && !s->leased && !s->pending && r[i].size <= s->capacity) {
                s->pending = 1; s->requested = r[i].size;
                p->queue[p->queued++] = (uint8_t)j;
                break;
            }
        }
        if (j == p->count) {
            if (failure) { failure->tag = r[i].tag; failure->size = r[i].size; }
            while (p->queued) p->slots[p->queue[--p->queued]].pending = 0;
            return 0;
        }
    }
    return 1;
}
static void *ds_pool_alloc(ds_pool *p, uint32_t n) {
    ds_pool_slot *s = 0;
    if (p->cursor < p->queued) {
        s = &p->slots[p->queue[p->cursor]];
        if (n != s->requested) return 0;
        ++p->cursor;
    } else {
        /* RBSP reserve and tiny DPB reference-list vectors have no preflight
         * hook. Their separate cached slots are bounded by this clip profile. */
        for (uint32_t i = 0; i < p->count; ++i) {
            ds_pool_slot *candidate = &p->slots[i];
            if (candidate->tag == 0 && !candidate->leased && !candidate->pending && n <= candidate->capacity) {
                s = candidate; break;
            }
        }
        if (!s) return 0;
        s->requested = n;
    }
    s->pending = 0; s->leased = 1;
    p->live += s->requested;
    if (p->live > p->highwater) p->highwater = p->live;
    return s->data;
}
static int ds_pool_free(ds_pool *p, void *pointer) {
    for (uint32_t i = 0; i < p->count; ++i) {
        ds_pool_slot *s = &p->slots[i];
        if (s->data == pointer && s->leased) {
            p->live -= s->requested; s->leased = 0; return 1;
        }
    }
    return pointer == 0;
}
static int ds_pool_reset(ds_pool *p) {
    if (p->live || p->cursor != p->queued) return 0;
    p->cursor = p->queued = 0;
    return 1;
}
