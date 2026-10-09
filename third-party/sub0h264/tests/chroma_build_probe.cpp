/** Differential tests use isolated namespaces for separately compiled builds.
 *  SPDX-License-Identifier: MIT
 */
#include "chroma_build_probe.h"
#include "decoder.hpp"

#include <memory>

#ifndef CHROMA_PROBE_PREFIX
#define CHROMA_PROBE_PREFIX chroma_reference_
#endif
#define CHROMA_JOIN_IMPL(a, b) a ## b
#define CHROMA_JOIN(a, b) CHROMA_JOIN_IMPL(a, b)
#define CHROMA_NAME(name) CHROMA_JOIN(CHROMA_PROBE_PREFIX, name)

namespace {
using namespace sub0h264;
struct Probe
{
    H264Decoder decoder;
    std::vector<NalUnit> nals;
    std::vector<uint32_t> syntax;
    explicit Probe(bool skip) : decoder(H264Decoder::Settings{skip}) {}
};
}

extern "C" void *CHROMA_NAME(create)(const uint8_t *data, uint32_t size,
                                    int skip, int capture)
{
    auto probe = std::make_unique<Probe>(skip != 0);
    std::vector<NalBounds> bounds;
    findNalUnits(data, size, bounds);
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(data + b.offset, b.size, nal))
            nal.type = static_cast<NalType>(0);
        probe->nals.push_back(std::move(nal));
    }
    if (capture)
        probe->decoder.trace().setCallback([p = probe.get()](const TraceEvent& e) {
            if (e.type == TraceEventType::MbStart || e.type == TraceEventType::MbEnd ||
                e.type == TraceEventType::BlockResidual || e.type == TraceEventType::SliceStart)
                p->syntax.insert(p->syntax.end(), {static_cast<uint32_t>(e.type),
                    e.mbX, e.mbY, e.a, e.b, e.c, e.d});
        });
    return probe.release();
}

extern "C" uint32_t CHROMA_NAME(nals)(void *handle)
{
    return static_cast<uint32_t>(static_cast<Probe *>(handle)->nals.size());
}
extern "C" int CHROMA_NAME(step)(void *handle, uint32_t index)
{
    auto& p = *static_cast<Probe *>(handle);
    p.syntax.clear();
    if (index >= p.nals.size() || static_cast<uint32_t>(p.nals[index].type) == 0U)
        return -1;
    return static_cast<int>(p.decoder.processNal(p.nals[index]));
}
extern "C" chroma_probe_view CHROMA_NAME(view)(void *handle)
{
    auto& p = *static_cast<Probe *>(handle);
    chroma_probe_view result{};
    result.frames = p.decoder.frameCount();
    result.bit_offset = p.decoder.diagBitOffset();
    result.entered = p.decoder.diagMbEntered();
    result.completed = p.decoder.diagMbCompleted();
    auto& contexts = p.decoder.cabacContexts();
    static_assert(sizeof(CabacCtx) == 1);
    result.contexts = reinterpret_cast<const uint8_t *>(contexts.data());
    result.context_bytes = contexts.size();
    result.syntax = p.syntax.data();
    result.syntax_words = static_cast<uint32_t>(p.syntax.size());
    if (const Frame *frame = p.decoder.currentFrame())
    {
        result.y = frame->yData();
        result.width = frame->width();
        result.height = frame->height();
        result.stride = frame->yStride();
        result.has_chroma = frame->hasChroma() || frame->uData() ||
                            frame->vData() || frame->uvStride();
    }
    return result;
}
extern "C" void CHROMA_NAME(destroy)(void *handle)
{
    delete static_cast<Probe *>(handle);
}

#if defined(SUB0H264_ENABLE_CHROMA_RECONSTRUCTION) && !SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
namespace {
uint32_t planeRequests;
bool onlyY(const AllocationRequest *requests, size_t count, AllocationFailure *) noexcept
{
    planeRequests += static_cast<uint32_t>(count);
    return count == 1U && requests[0].tag == AllocationTag::FrameY;
}
bool rejectY(const AllocationRequest *requests, size_t count,
             AllocationFailure *failure) noexcept
{
    if (count != 1U || requests[0].tag != AllocationTag::FrameY) return false;
    if (failure) *failure = {requests[0].tag, requests[0].size};
    return false;
}
}
extern "C" int chroma_trimmed_storage_check(void)
{
    H264Decoder defaults, color(H264Decoder::Settings{false});
    if (!defaults.skipChroma() || !color.skipChroma()) return 1;
    setAllocationPreflightForTesting(onlyY);
    Frame frame;
    planeRequests = 0U;
    bool allocated = frame.allocate(320U, 192U, false);
    setAllocationPreflightForTesting(nullptr);
    if (!allocated || planeRequests != 1U || frame.allocatedBytes() != 61440U ||
        frame.hasChroma() || frame.uData() || frame.vData() || frame.uvStride() ||
        frame.uRow(0U) || frame.vRow(0U) || frame.uMb(0U, 0U) || frame.vMb(0U, 0U)) return 2;
    frame.fill(77U, 111U, 222U);
    if (frame.y(0U, 0U) != 77U || !frame.allocate(16U, 16U, false) ||
        frame.hasChroma() || frame.uData() || frame.vData()) return 3;
    Dpb dpb;
    if (!dpb.init(320U, 192U, 1U, false)) return 4;
    Frame *first = dpb.getDecodeTarget(0U, 16U);
    if (!first) return 5;
    dpb.markAsReference(0U);
    Frame *second = dpb.getDecodeTarget(1U, 16U);
    if (!second || second == first || dpb.allocatedFrameBytes() != 122880U) return 6;
    dpb.flush();
    if (!dpb.getDecodeTarget(0U, 16U) || dpb.allocatedFrameCount() != 2U) return 7;
    Frame denied;
    setAllocationPreflightForTesting(rejectY);
    bool refused = !denied.allocate(320U, 192U, false);
    setAllocationPreflightForTesting(nullptr);
    if (!refused || denied.isAllocated() || denied.width() || denied.height() ||
        denied.uData() || denied.vData() ||
        denied.allocationFailure().tag != AllocationTag::FrameY ||
        denied.allocationFailure().size != 61440U) return 10;
    if (!denied.allocate(320U, 192U, false) || denied.hasChroma()) return 11;
    for (int32_t offset : {0, 40})
    {
        Frame luma;
        if (!luma.allocate(32U, 32U, false)) return 8;
        luma.fill(128U, 128U, 128U);
        for (uint32_t y = 0U; y < 32U; ++y)
            for (uint32_t x = 16U; x < 32U; ++x) luma.y(x, y) = 132U;
        uint8_t nnz[64] = {}, transforms[4] = {};
        MbMotionInfo motion[64]{};
        for (auto& m : motion) m.refIdx = -1;
        int32_t qps[4] = {51, 0, 51, 0};
        deblockMb(luma, 1U, 1U, true, 0, 0, nnz, motion, qps, transforms,
                  offset, 2U, 2U, false);
        if ((luma.y(15U, 16U) == 128U) != (offset == 0)) return 9;
    }
    return 0;
}
#endif
