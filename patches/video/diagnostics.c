/* Bounded task-context snapshots. SPDX-License-Identifier: GPL-3.0-only */
#include "control.h"
#include "worker.h"
#include "platform.h"
#include "../cfw_context.h"

#pragma clang section text=".text.video"

static void video_diag_write32(uint8_t *p, uint32_t value) {
    for (uint32_t i = 0; i < 4; ++i) p[i] = value >> (8 * i);
}

void video_controller_stack_sample(void) {
    customCfwContext *ctx = peekCustomCfwContext();
    uint32_t task = 0, used = 0, bytes = 0;
    if (!ctx || !VIDEO_PLATFORM_STACK_SAMPLE(&task, &used, &bytes) || !bytes || used > bytes)
        return;
    video_control_state *s = &ctx->video_control;
    /* One claimed controller publishes before clearing its in-flight job.
     * Readers make a bounded seqlock attempt, never scan another live stack. */
    if (bytes - used < s->stack_bytes - s->stack_used || !s->stack_bytes) {
        __atomic_fetch_add(&s->stack_sequence, 1, __ATOMIC_ACQ_REL);
        __atomic_store_n(&s->stack_task, task, __ATOMIC_RELAXED);
        __atomic_store_n(&s->stack_used, used, __ATOMIC_RELAXED);
        __atomic_store_n(&s->stack_bytes, bytes, __ATOMIC_RELAXED);
        __atomic_fetch_add(&s->stack_sequence, 1, __ATOMIC_RELEASE);
    }
}

int video_diagnostics_snapshot(uint8_t out[VIDEO_DIAGNOSTICS_BYTES]) {
    if (!out) return 0;
    bzero(out, VIDEO_DIAGNOSTICS_BYTES);
    out[0] = VIDEO_PROTOCOL_VERSION; out[1] = VIDEO_CONTROL_DIAGNOSTICS;
    video_diag_write32(out + 4, VIDEO_TICK);
    customCfwContext *ctx = peekCustomCfwContext();
    video_diag_write32(out + 8, ctx ? __atomic_load_n(&ctx->video.generation, __ATOMIC_ACQUIRE) : 0);
    const uint32_t heaps[3] = {VIDEO_HEAP_CACHED, VIDEO_HEAP_DISPLAY, VIDEO_HEAP_OTHER};
    for (uint32_t i = 0; i < 3; ++i) {
        video_heap_view view = {UINT32_MAX, UINT32_MAX};
        int valid = 0;
        for (uint32_t attempt = 0; attempt < 2 && !valid; ++attempt)
            valid = video_platform_heap_view(0, heaps[i], &view) &&
                view.free_bytes != UINT32_MAX && view.max_alloc <= view.free_bytes;
        video_diag_write32(out + 12 + i * 12, VIDEO_TICK);
        video_diag_write32(out + 16 + i * 12, valid ? view.free_bytes : UINT32_MAX);
        video_diag_write32(out + 20 + i * 12, valid ? view.max_alloc : UINT32_MAX);
        if (valid) out[3] |= 1u << i;
    }
    video_worker_report report = {0};
    if (!video_worker_get_report(&report)) return 0;
    _Static_assert(sizeof(report) == 68, "worker diagnostics wire size");
    /* Serialize explicitly: the wire is little-endian, independent of host. */
    uint32_t words[17]; memcpy(words, &report, sizeof(words));
    for (uint32_t i = 0; i < 17; ++i) video_diag_write32(out + 48 + i * 4, words[i]);
    if (report.state == VIDEO_QUARANTINED) out[3] |= 8;
    if (ctx) {
        video_control_state *s = &ctx->video_control;
        for (uint32_t attempt = 0; attempt < 2; ++attempt) {
            uint32_t first = __atomic_load_n(&s->stack_sequence, __ATOMIC_ACQUIRE);
            if (first & 1) continue;
            uint32_t task = __atomic_load_n(&s->stack_task, __ATOMIC_RELAXED);
            uint32_t used = __atomic_load_n(&s->stack_used, __ATOMIC_RELAXED);
            uint32_t bytes = __atomic_load_n(&s->stack_bytes, __ATOMIC_RELAXED);
            __atomic_thread_fence(__ATOMIC_ACQUIRE);
            if (first != __atomic_load_n(&s->stack_sequence, __ATOMIC_ACQUIRE)) continue;
            video_diag_write32(out + 116, task);
            video_diag_write32(out + 120, used);
            video_diag_write32(out + 124, bytes);
            if (bytes && used <= bytes) out[3] |= 16;
            break;
        }
    }
    return 1;
}

#pragma clang section text=""
