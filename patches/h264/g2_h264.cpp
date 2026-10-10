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
    bool inputStarted = false;
    uint32_t width = 0, height = 0, references = 0, frameCapacity = 0;
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

extern "C" int g2_h264_limit_format(void *handle, uint32_t width, uint32_t height,
                                     uint32_t references, uint32_t frameCapacity) {
    if (!handle || !width || !height || width % 16 || height % 16 ||
        width > sub0h264::cMaxWidth || height > sub0h264::cMaxHeight ||
        references != 1 || frameCapacity != references + 1) return 0;
    auto *state = static_cast<State *>(handle);
    if (state->inputStarted) return 0;
    state->width = width; state->height = height;
    state->references = references; state->frameCapacity = frameCapacity;
    return 1;
}

extern "C" int g2_h264_dpb(const void *handle, g2_h264_dpb_info *info) {
    if (!info) return 0;
    *info = {};
    if (!handle) return 0;
    const auto &decoder = static_cast<const State *>(handle)->decoder;
    *info = {decoder.dpbFrameCapacity(), decoder.dpbAllocatedFrameCount(),
             static_cast<uint32_t>(decoder.dpbAllocatedFrameBytes())};
    return 1;
}

static bool validSps(const State &state, const sub0h264::Sps &sps) {
    return sps.valid_ && (sps.profileIdc_ == sub0h264::cProfileBaseline ||
        sps.profileIdc_ == sub0h264::cProfileMain || sps.profileIdc_ == sub0h264::cProfileHigh) &&
        sps.widthInMbs_ == state.width / 16 && sps.heightInMbs_ == state.height / 16 &&
        sps.numRefFrames_ == state.references &&
        sps.frameMbsOnly_ && !sps.frameCropping_ && sps.chromaFormatIdc_ == 1 &&
        sps.bitDepthLuma_ == 8 && sps.bitDepthChroma_ == 8;
}

static bool validPps(const sub0h264::Pps &pps) {
    /* Weighted prediction is outside the advertised player subset. Reject it
     * before parsing a weight table or entering reconstruction. */
    return pps.valid_ && pps.numSliceGroups_ == 1 && pps.numRefIdxL0Active_ == 1 &&
        !pps.weightedPredFlag_ && !pps.weightedBipredIdc_ &&
        (!pps.transform8x8Mode_ || pps.isCabac());
}

static bool validInput(State &state) {
    using namespace sub0h264;
    const auto &nal = state.nal;
    BitReader br(nal.rbspData.data(), nal.rbspData.size());
    auto &sets = state.decoder.paramSets();
    if (nal.type == NalType::Sps) {
        Sps sps;
        return parseSps(br, sps) == Result::Ok && br.hasBits(1) && validSps(state, sps);
    }
    if (nal.type == NalType::Pps) {
        Pps pps;
        return parsePps(br, sets.spsArray(), pps) == Result::Ok && br.hasBits(1) &&
            validPps(pps) && sets.getSps(pps.spsId_) && validSps(state, *sets.getSps(pps.spsId_));
    }
    if (nal.type != NalType::SliceIdr && nal.type != NalType::SliceNonIdr)
        return nal.type == NalType::Sei || nal.type == NalType::Aud ||
            nal.type == NalType::EndOfSequence || nal.type == NalType::EndOfStream ||
            nal.type == NalType::FillerData;
    uint32_t first = br.readUev(), type = br.readUev(), id = br.readUev();
    /* The current core completes one slice at a time. Advertise and enforce
     * that limitation rather than counting arbitrary VCL NALs as pictures. */
    if (!br.hasBits(1) || first || type > 9 || (type % 5 != 0 && type % 5 != 2) ||
        id >= cMaxPpsCount) return false;
    const Pps *pps = sets.getPps(static_cast<uint8_t>(id));
    const Sps *sps = pps ? sets.getSps(pps->spsId_) : nullptr;
    if (!pps || !sps || !validPps(*pps) || !validSps(state, *sps)) return false;
    br = BitReader(nal.rbspData.data(), nal.rbspData.size());
    SliceHeader header;
    return parseSliceHeader(br, *sps, *pps, nal.type == NalType::SliceIdr,
        nal.refIdc, header) == Result::Ok && br.hasBits(1) && !header.fieldPicFlag_ &&
        (header.sliceType_ != SliceType::P || header.numRefIdxActiveL0_ == state.references) &&
        header.disableDeblockingFilter_ <= 2 && header.cabacInitIdc_ <= 2;
}

extern "C" g2_h264_result g2_h264_decode(void *handle, const uint8_t *data, uint32_t size) {
    if (!handle) return G2_H264_ERROR;
    auto *state = static_cast<State *>(handle);
    state->frameReady = false;
    if (!data || !size || size > G2_H264_MAX_NAL_BYTES) return G2_H264_ERROR;
    state->inputStarted = true;
    if (!sub0h264::parseNalUnit(data, size, state->nal)) return G2_H264_ERROR;
    if (state->width && !validInput(*state)) return G2_H264_ERROR;
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
