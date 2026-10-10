/* Singleton publication and allocation failures. SPDX-License-Identifier: GPL-3.0-only */
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../patches/cfw_context.h"

static customCfwContext *slot;
static unsigned allocations, frees;
static int refuse, racing;
static pthread_barrier_t allocation_barrier;
#undef CFW_CTX_SLOT
#define CFW_CTX_SLOT ((uintptr_t)&slot)
#define CFW_CONTEXT_POINTER_VALID(ctx) ((ctx) != NULL)

static void *cfw_malloc(uint32_t size) {
    __atomic_add_fetch(&allocations, 1, __ATOMIC_RELAXED);
    if (refuse) return NULL;
    void *memory = malloc(size);
    assert(memory);
    memset(memory, 0xa5, size);
    if (racing) pthread_barrier_wait(&allocation_barrier);
    return memory;
}
static void release(void *memory) {
    __atomic_add_fetch(&frees, 1, __ATOMIC_RELAXED);
    free(memory);
}
#define FW_FREE release
static void zero_bytes(uint8_t *p, uint32_t size) { memset(p, 0, size); }
#define bzero zero_bytes
#include "../../patches/cfw_context.c"

static void *get_concurrently(void *unused) {
    (void)unused;
    return getCustomCfwContext();
}
int main(void) {
    assert(!peekCustomCfwContext() && allocations == 0);
    refuse = 1;
    assert(!getCustomCfwContext() && !slot && allocations == 1 && frees == 0);
    refuse = 0;
    customCfwContext invalid = {0};
    slot = &invalid;
    customCfwContext *first = getCustomCfwContext();
    assert(first && first != &invalid && slot == first && allocations == 2);
    assert(first->magic == CFW_CTX_MAGIC && first->diag_hide == 1);
    for (unsigned i = 0; i < CFW_FID_RING; ++i) assert(first->recent_fids[i] == 0xffff);
    customCfwContext expected = {0};
    expected.magic = CFW_CTX_MAGIC; expected.diag_hide = 1;
    for (unsigned i = 0; i < CFW_FID_RING; ++i) expected.recent_fids[i] = 0xffff;
    assert(!memcmp(first, &expected, sizeof(expected)));
    first->diag_hide = 63;
    for (unsigned i = 0; i < 10000; ++i) assert(getCustomCfwContext() == first);
    assert(allocations == 2 && frees == 0 && first->diag_hide == 63);
    release(first); slot = NULL;

    racing = 1;
    assert(!pthread_barrier_init(&allocation_barrier, NULL, 2));
    pthread_t tasks[2]; void *owners[2];
    for (unsigned i = 0; i < 2; ++i) assert(!pthread_create(&tasks[i], NULL, get_concurrently, NULL));
    for (unsigned i = 0; i < 2; ++i) assert(!pthread_join(tasks[i], &owners[i]));
    assert(owners[0] && owners[0] == owners[1] && owners[0] == slot);
    assert(allocations == 4 && frees == 2 && slot->magic == CFW_CTX_MAGIC);
    racing = 0; release(slot); slot = NULL;
    assert(!pthread_barrier_destroy(&allocation_barrier));
    puts("first use, warm pointer reads, invalid state, refusal and competing creators PASS");
}
