/* Exercise ownership with fragmented/racing heaps. SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/video/storage.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "storage check %d\n", __LINE__); exit(1); } } while (0)
typedef struct {
    video_heap_view heaps[3];
    void *raw[VIDEO_ALLOCATION_LIMIT];
    uint32_t charges[VIDEO_ALLOCATION_LIMIT];
    uint32_t calls, releases, fail_at, racing_bytes;
    int invalid;
} test_heap;

static int view(void *argument, uint32_t heap, video_heap_view *out) {
    test_heap *h = argument;
    if (h->invalid) return 0;
    uint32_t index = heap == VIDEO_HEAP_CACHED ? 0 : heap == VIDEO_HEAP_DISPLAY ? 1 : 2;
    *out = h->heaps[index];
    if (out->max_alloc > out->free_bytes) out->max_alloc = out->free_bytes;
    return 1;
}
static void *allocate(void *argument, uint32_t size) {
    test_heap *h = argument;
    ++h->calls;
    if (h->calls == h->fail_at) return 0;
    uint32_t charge = ((size + 3u) & ~3u) + 4u;
    CHECK(h->heaps[0].free_bytes >= charge + h->racing_bytes);
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) {
        if (!h->raw[i]) {
            h->raw[i] = malloc(size); CHECK(h->raw[i]);
            h->charges[i] = charge;
            h->heaps[0].free_bytes -= charge + h->racing_bytes;
            h->racing_bytes = 0;
            return h->raw[i];
        }
    }
    CHECK(0); return 0;
}
static void release(void *argument, void *raw) {
    test_heap *h = argument;
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) {
        if (h->raw[i] == raw) {
            free(raw); h->raw[i] = 0; ++h->releases;
            h->heaps[0].free_bytes += h->charges[i]; return;
        }
    }
    CHECK(0);
}
static void init(test_heap *h, video_storage *s) {
    memset(h, 0, sizeof(*h));
    h->heaps[0] = (video_heap_view){382756, 376832};
    h->heaps[1] = (video_heap_view){285680, 278528};
    h->heaps[2] = (video_heap_view){133924, 131072};
    video_heap_ops ops = {h, allocate, release, view};
    CHECK(video_storage_init(s, &ops, 2048, 153600));
}
static void clean(test_heap *h, video_storage *s) {
    video_storage_reclaim(s);
    CHECK(s->used == s->initial_charge);
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) CHECK(!h->raw[i]);
}
static void batch(void) {
    test_heap h; video_storage s; init(&h, &s);
    /* Unknown/reordered tags and different sizes do not select decoder pools. */
    g2_h264_request r[] = {{900, 12345}, {2, 61440}, {3, 0}, {55, 31}, {5, 1111}};
    g2_h264_failure f;
    CHECK(video_storage_preflight(&s, r, 5, &f) && !f.tag);
    uint32_t calls = h.calls;
    void *small = video_storage_alloc(&s, 7); CHECK(small && h.calls == calls + 1);
    void *p = video_storage_alloc(&s, 61440); CHECK(p && (uintptr_t)p % 32 == 0);
    memset(p, 0x5a, 61440);
    CHECK(video_storage_release(&s, p)); CHECK(!video_storage_release(&s, p));
    CHECK(video_storage_release(&s, small));
    video_storage_finish_call(&s); CHECK(s.used == s.initial_charge);
    CHECK(video_storage_preflight(&s, 0, 0, &f));
    clean(&h, &s);
}
static void rollback(void) {
    g2_h264_request r[] = {{11, 4096}, {12, 960}, {13, 38400}, {14, 61440}};
    for (uint32_t fail = 1; fail <= 4; ++fail) {
        test_heap h; video_storage s; init(&h, &s); h.fail_at = fail;
        g2_h264_failure f;
        CHECK(!video_storage_preflight(&s, r, 4, &f));
        CHECK(f.tag == r[fail - 1].tag && f.size == r[fail - 1].size);
        CHECK(s.used == s.initial_charge && h.heaps[0].free_bytes == 382756);
        clean(&h, &s);
    }
}
static void reserves(void) {
    test_heap h; video_storage s;
    init(&h, &s); h.heaps[0].max_alloc = 100; CHECK(!video_storage_alloc(&s, 100)); clean(&h, &s);
    init(&h, &s); h.heaps[0].free_bytes = 32768 + 16384; CHECK(!video_storage_alloc(&s, 1)); clean(&h, &s);
    init(&h, &s); h.heaps[1].free_bytes = 32768 + 153600 - 1; CHECK(!video_storage_alloc(&s, 1)); clean(&h, &s);
    init(&h, &s); h.heaps[2].free_bytes = 16383; CHECK(!video_storage_alloc(&s, 1)); clean(&h, &s);
    init(&h, &s); h.invalid = 1; CHECK(!video_storage_alloc(&s, 1)); clean(&h, &s);
    init(&h, &s); h.racing_bytes = 340000; CHECK(!video_storage_alloc(&s, 100));
    CHECK(h.releases == 1 && s.used == s.initial_charge); clean(&h, &s);
}
static void bounds(void) {
    test_heap h; video_storage s; init(&h, &s);
    CHECK(!video_storage_alloc(&s, 0)); CHECK(!video_storage_alloc(&s, UINT32_MAX));
    CHECK(!video_storage_alloc(&s, VIDEO_STORAGE_LIMIT));
    int foreign; CHECK(!video_storage_release(&s, &foreign)); CHECK(video_storage_release(&s, 0));
    for (uint32_t i = 0; i < VIDEO_ALLOCATION_LIMIT; ++i) CHECK(video_storage_alloc(&s, 1));
    CHECK(!video_storage_alloc(&s, 1)); clean(&h, &s);
}
static void growth(void) {
    test_heap h; video_storage s; init(&h, &s);
    void *old = video_storage_alloc(&s, 40000); CHECK(old);
    g2_h264_request r[] = {{500, 60000}}; g2_h264_failure f;
    CHECK(video_storage_preflight(&s, r, 1, &f));
    void *next = video_storage_alloc(&s, 60000); CHECK(next);
    CHECK(s.peak >= 100000 && video_storage_release(&s, old));
    CHECK(video_storage_release(&s, next));
    CHECK(video_storage_preflight(&s, r, 1, &f));
    /* Retained vector capacity can make an announced request unnecessary. */
    video_storage_finish_call(&s); CHECK(s.used == s.initial_charge);
    clean(&h, &s);
}
int main(int argc, char **argv) {
    CHECK(argc == 2);
    if (!strcmp(argv[1], "batch")) batch();
    else if (!strcmp(argv[1], "rollback")) rollback();
    else if (!strcmp(argv[1], "reserves")) reserves();
    else if (!strcmp(argv[1], "bounds")) bounds();
    else if (!strcmp(argv[1], "growth")) growth();
    else CHECK(0);
    puts("bounded storage PASS"); return 0;
}
