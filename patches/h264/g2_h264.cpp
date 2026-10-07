/* Sub0h264 adapter, inert until called. SPDX-License-Identifier: GPL-3.0-only */
#include "g2_h264.h"
#include "runtime.h"
#include <new>
#include <cstring>
#include <cstdlib>

/* Local definitions supplied by jim's C unit/runtime must not need a GOT. */
#if defined(__arm__)
extern "C" __attribute__((visibility("hidden"))) void *memcpy(void *, const void *, size_t);
extern "C" __attribute__((visibility("hidden"))) void *memmove(void *, const void *, size_t);
extern "C" __attribute__((visibility("hidden"))) void *memset(void *, int, size_t);
extern "C" __attribute__((visibility("hidden"))) int abs(int);
#endif

#include "config.hpp"
#include "decoder.hpp"
#include "nal.hpp"

[[noreturn]] static void fail(uint32_t reason) {
    const auto *runtime = g2_h264_runtime_current();
    if (runtime && runtime->fail) runtime->fail(reason);
    /* A missing/returning fatal callback violates the runtime contract.
     * The inert initializer rejects an unbound runtime before allocation. */
    __builtin_trap();
}

void *operator new(size_t size) {
    if (size > UINT32_MAX) fail(G2_H264_FAIL_LENGTH);
    const auto *runtime = g2_h264_runtime_current();
    void *memory = runtime && runtime->alloc
        ? runtime->alloc(static_cast<uint32_t>(size ? size : 1U)) : nullptr;
    if (!memory) fail(G2_H264_FAIL_ALLOC);
    return memory;
}
void operator delete(void *memory) noexcept {
    if (memory) g2_h264_runtime_current()->release(memory);
}
void operator delete(void *memory, size_t) noexcept { operator delete(memory); }

namespace std {
void __throw_bad_alloc() { fail(G2_H264_FAIL_BAD_ALLOC); }
void __throw_bad_array_new_length() { fail(G2_H264_FAIL_ARRAY_LENGTH); }
void __throw_length_error(const char *) { fail(G2_H264_FAIL_LENGTH); }
void __throw_bad_function_call() { fail(G2_H264_FAIL_FUNCTION); }
}

extern "C" bool g2_h264_preflight(const sub0h264::AllocationRequest *input, size_t count,
                                 sub0h264::AllocationFailure *failure) noexcept {
    /* Eight covers the decoder's largest batch: seven macroblock-context allocations. */
    constexpr size_t maxPreflightRequests = 8U;
    const auto *runtime = g2_h264_runtime_current();
    if (failure) *failure = {};
    if (!input || count > maxPreflightRequests || !runtime || !runtime->preflight) return false;
    g2_h264_request requests[maxPreflightRequests];
    for (size_t i = 0; i < count; ++i) {
        if (input[i].size > UINT32_MAX) {
            if (failure) *failure = {input[i].tag, input[i].size};
            return false;
        }
        requests[i] = {static_cast<uint32_t>(input[i].tag),
                       static_cast<uint32_t>(input[i].size)};
    }
    g2_h264_failure detail = {};
    int ok = runtime->preflight(requests, static_cast<uint32_t>(count), &detail);
    if (!ok && failure)
        *failure = {static_cast<sub0h264::AllocationTag>(detail.tag), detail.size};
    return ok != 0;
}

struct State {
    sub0h264::H264Decoder decoder;
    sub0h264::NalUnit nal;
    bool frameReady = false;
    State() : decoder(sub0h264::H264Decoder::Settings{true}) {
        /* EBSP removal never grows beyond a NAL. Reserve the entire record
         * once, so accepted 4086-byte NALs cannot reallocate scratch storage. */
        nal.rbspData.reserve(G2_H264_RECORD_BYTES);
    }
};
static_assert(sizeof(State) <= G2_H264_RECORD_BYTES);

extern "C" uint32_t g2_h264_size() { return sizeof(State); }
extern "C" uint32_t g2_h264_alignment() { return alignof(State); }
extern "C" void *g2_h264_init(void *memory, uint32_t size) {
    if (!memory || size < sizeof(State) ||
        reinterpret_cast<uintptr_t>(memory) % alignof(State)) return nullptr;
    const auto *runtime = g2_h264_runtime_current();
    if (!runtime || !runtime->alloc || !runtime->release ||
        !runtime->preflight || !runtime->fail) return nullptr;
    return new (memory) State();
}
extern "C" void g2_h264_destroy(void *handle) {
    if (handle) static_cast<State *>(handle)->~State();
}
extern "C" g2_h264_result g2_h264_decode(void *handle, const uint8_t *data, uint32_t size) {
    if (!handle) return G2_H264_ERROR;
    auto *state = static_cast<State *>(handle);
    state->frameReady = false;
    if (!data || !size || size > G2_H264_MAX_NAL_BYTES) return G2_H264_ERROR;
    if (!sub0h264::parseNalUnit(data, size, state->nal)) return G2_H264_ERROR;
    switch (state->decoder.processNal(state->nal)) {
        case sub0h264::DecodeStatus::FrameDecoded:
            state->frameReady = true;
            return G2_H264_FRAME_READY;
        case sub0h264::DecodeStatus::NeedMoreData: return G2_H264_CONSUMED;
        default: return G2_H264_ERROR;
    }
}
extern "C" g2_h264_result g2_h264_frame(const void *handle, g2_h264_frame_info *info) {
    if (!info) return G2_H264_ERROR;
    *info = {};
    if (!handle) return G2_H264_ERROR;
    const auto *state = static_cast<const State *>(handle);
    if (!state->frameReady) return G2_H264_ERROR;
    const auto *frame = state->decoder.currentFrame();
    if (!frame) return G2_H264_ERROR;
    *info = {frame->yData(), frame->width(), frame->height(), frame->yStride(),
             state->decoder.frameCount()};
    return G2_H264_FRAME_READY;
}
