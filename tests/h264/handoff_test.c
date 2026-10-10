/* The production copied-display-queue adapter with fatal native checks.
 * SPDX-License-Identifier: GPL-3.0-only */
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include "../../patches/cfw_context.h"

static customCfwContext context;
static uint32_t puts_count, refusal;
static uint32_t copied[VIDEO_HANDOFF_RECORD_WORDS];
static customCfwContext *peekCustomCfwContext(void) { return &context; }
static customCfwContext *getCustomCfwContext(void) { return &context; }
static int video_control_wait(customCfwContext *ctx, uint32_t limit) {
    (void)ctx; (void)limit; assert(0); return 0;
}
static void video_control_give(customCfwContext *ctx) { (void)ctx; assert(0); }
static void zero(void *p, uint32_t n) { memset(p, 0, n); }
static int display_take(void) { assert(0); return 0; }
static void display_signal(void) { assert(0); }
static void original_copy(void) { assert(0); }
void display_copy_hook(void) { assert(0); }
int video_controller_request_locked(uint32_t reasons) { (void)reasons; assert(0); return 0; }
static int put(uint32_t queue, const void *record, uint8_t priority, uint32_t timeout) {
    assert(queue == 0x20001000U && !priority && !timeout);
    ++puts_count; memcpy(copied, record, sizeof(copied)); return refusal ? -1 : 0;
}
#define VIDEO_OS_THREAD_NEW 0
#define VIDEO_POOL_DISPATCH(x) ((void)(x), 0)
#define VIDEO_TICK 0u
#define VIDEO_OS_DELAY(x) ((void)(x))
#define VIDEO_HANDOFF_NATIVE 1
#define VIDEO_HANDOFF_PUT put
#define VIDEO_DISPLAY_TAKE display_take
#define VIDEO_DISPLAY_SIGNAL display_signal
#define FW_DISPLAY_FB ((uint8_t *)0)
#define FW_DISPLAY_COPY original_copy
#define FW_FLUSH(x) ((void)(x))
#define bzero zero
#include "../../patches/video/handoff.c"

int main(void) {
    void *ram = mmap((void *)0x20000000U, 0x2000, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    assert(ram == (void *)0x20000000U);
    volatile uint32_t *handle = (void *)0x20000760U;
    volatile uint32_t *queue = (void *)0x20001000U;
    uint32_t record[VIDEO_HANDOFF_RECORD_WORDS] = {3,0,0,0,0,640,480,91,0};
    assert(!video_handoff_queue(record) && !puts_count);
    *handle = 0x20001001U; assert(!video_handoff_queue(record) && !puts_count);
    *handle = 0x10001000U; assert(!video_handoff_queue(record) && !puts_count);
    *handle = 0x20001000U;
    queue[15] = 95; queue[16] = 36; assert(!video_handoff_queue(record) && !puts_count);
    queue[15] = 96; queue[16] = 4; assert(!video_handoff_queue(record) && !puts_count);
    queue[16] = 36; refusal = 1;
    assert(!video_handoff_queue(record) && puts_count == 1);
    refusal = 0; assert(video_handoff_queue(record) && puts_count == 2);
    memset(record, 0, sizeof(record));
    assert(copied[0] == 3 && copied[5] == 640 && copied[6] == 480 && copied[7] == 91 && !copied[8]);
    assert(!munmap(ram, 0x2000));
    puts("bounded zero-timeout copied display queue and refusal guards PASS");
}
