#include "capsule.h"
#include <new>
#include <cstring>
#include <cstdlib>
extern "C" __attribute__((visibility("hidden"))) void* memcpy(void*, const void*, size_t);
extern "C" __attribute__((visibility("hidden"))) void* memmove(void*, const void*, size_t);
extern "C" __attribute__((visibility("hidden"))) void* memset(void*, int, size_t);
extern "C" __attribute__((visibility("hidden"))) int abs(int);
#define SUB0H264_TRACE 0
#define SUB0H264_MAX_SPS_COUNT 1U
#define SUB0H264_MAX_PPS_COUNT 1U
#define SUB0H264_DISABLE_LEGACY_CURRENT_FRAME 1
#define SUB0H264_ALLOCATION_PREFLIGHT ds_preflight
#include "timing.hpp"
#include "decoder.hpp"
#include "nal.hpp"

#ifdef DS_HOST_TEST
ds_imports* ds_host_imports;
#endif
static ds_imports& imports() {
#ifdef DS_HOST_TEST
    return *ds_host_imports;
#else
    return **reinterpret_cast<ds_imports* volatile*>(DS_IMPORT_SLOT);
#endif
}
[[noreturn]] static void fail(uint32_t reason) {
    // The controller reports failure and parks this worker in MRAM. Its
    // owner terminates the thread before reclaiming any partial C++ state.
    imports().fail(reason);
    __builtin_unreachable();
}
void* operator new(size_t n) {
    void* p = imports().alloc(static_cast<uint32_t>(n));
    if (!p) fail(1);
    return p;
}
void operator delete(void* p) noexcept { if (p) imports().release(p); }
void operator delete(void* p, size_t) noexcept { if (p) imports().release(p); }
namespace std {
void __throw_bad_alloc() { fail(2); }
void __throw_bad_array_new_length() { fail(3); }
void __throw_length_error(const char*) { fail(4); }
void __throw_bad_function_call() { fail(5); }
}
extern "C" bool ds_preflight(const sub0h264::AllocationRequest* r, size_t n,
                              sub0h264::AllocationFailure* f) noexcept {
    if (!r || n > 8) return false;
    ds_request requests[8];
    for (size_t i = 0; i < n; ++i)
        requests[i] = {static_cast<uint32_t>(r[i].tag), static_cast<uint32_t>(r[i].size)};
    ds_failure failure = {};
    int ok = imports().preflight(requests, static_cast<uint32_t>(n), &failure);
    if (f) *f = {static_cast<sub0h264::AllocationTag>(failure.tag), failure.size};
    return ok != 0;
}
struct State {
    sub0h264::H264Decoder decoder;
    sub0h264::NalUnit nal;
    explicit State(bool skip) : decoder(sub0h264::H264Decoder::Settings{skip}) {
        nal.rbspData.reserve(2048);
    }
};
static_assert(sizeof(State) <= 4096U);
extern "C" uint32_t ds_size() { return sizeof(State); }
extern "C" uint32_t ds_selftest(uint32_t x) { return (x ^ 0xd35c2301U) + 17U; }
extern "C" void* ds_init(void* memory, uint32_t size, uint32_t skip) {
    if (!memory || size < sizeof(State) || reinterpret_cast<uintptr_t>(memory) % alignof(State)) return nullptr;
    return new (memory) State(skip != 0);
}
extern "C" void ds_destroy(void* handle) { if (handle) static_cast<State*>(handle)->~State(); }
extern "C" int ds_decode(void* handle, const uint8_t* data, uint32_t size) {
    if (!handle || !data || !size || size > 2048) return -1;
    auto* state = static_cast<State*>(handle);
    if (!sub0h264::parseNalUnit(data, size, state->nal)) return -1;
    switch (state->decoder.processNal(state->nal)) {
        case sub0h264::DecodeStatus::FrameDecoded: return 1;
        case sub0h264::DecodeStatus::NeedMoreData: return 0;
        default: return -1;
    }
}
extern "C" int ds_frame(const void* handle, ds_frame_info* info) {
    if (!handle || !info) return -1;
    const auto& d = static_cast<const State*>(handle)->decoder;
    const auto* f = d.currentFrame();
    if (!f) return -1;
    *info = {f->yData(), f->width(), f->height(), f->yStride(), d.frameCount()};
    return 0;
}
