/** Paired luma comparison for fixtures and caller-supplied streams.
 *  SPDX-License-Identifier: MIT
 */
#ifndef SUB0H264_SKIP_CHROMA_COMPARE_HPP
#define SUB0H264_SKIP_CHROMA_COMPARE_HPP

#include "../components/sub0h264/src/decoder.hpp"

#include <array>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

namespace sub0h264::test {

struct ChromaComparison
{
    uint32_t frames = 0U;
    uint32_t parseErrors = 0U;
    uint32_t decodeErrors = 0U;
    uint32_t partialFrames = 0U;
    uint32_t outcomeDifferences = 0U;
    uint64_t yMismatches = 0U;
    uint32_t storageErrors = 0U;
    uint32_t syntaxDifferences = 0U;
};

/** Compare every output row, consuming every NAL even after a reported error.
 *  Matching decode failures are reported separately; they are not proof that
 *  the complete clip decoded. This deliberately avoids decodeStream(), which
 *  counts output but suppresses processNal() errors.
 */
inline ChromaComparison compareChromaModes(const std::vector<uint8_t>& data)
{
    using SyntaxEvent = std::array<uint32_t, 7>;
    std::vector<SyntaxEvent> fullSyntax, lumaSyntax;
    auto full = std::make_unique<H264Decoder>();
    auto luma = std::make_unique<H264Decoder>(H264Decoder::Settings{true});
    auto capture = [](std::vector<SyntaxEvent>& events, const TraceEvent& e) {
        if (e.type == TraceEventType::MbStart || e.type == TraceEventType::MbEnd ||
            e.type == TraceEventType::BlockResidual || e.type == TraceEventType::SliceStart)
            events.push_back({static_cast<uint32_t>(e.type), e.mbX, e.mbY, e.a, e.b, e.c, e.d});
    };
    full->trace().setCallback([&](const TraceEvent& e) { capture(fullSyntax, e); });
    luma->trace().setCallback([&](const TraceEvent& e) { capture(lumaSyntax, e); });
    ChromaComparison result;
    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(data.data() + b.offset, b.size, nal))
        {
            ++result.parseErrors;
            continue;
        }
        fullSyntax.clear();
        lumaSyntax.clear();
        DecodeStatus a = full->processNal(nal);
        DecodeStatus z = luma->processNal(nal);
        result.outcomeDifferences += (a != z || full->frameCount() != luma->frameCount());
        result.decodeErrors += (a == DecodeStatus::Error || z == DecodeStatus::Error);
        result.syntaxDifferences += (fullSyntax != lumaSyntax);
        result.syntaxDifferences += full->cabacContexts().countDifferences(luma->cabacContexts()) != 0U;
        result.syntaxDifferences += full->diagBitOffset() != luma->diagBitOffset();
        if (a != DecodeStatus::FrameDecoded || z != DecodeStatus::FrameDecoded)
            continue;
        ++result.frames;
        const Frame* af = full->currentFrame();
        const Frame* zf = luma->currentFrame();
        if (!af || !zf || af->width() != zf->width() || af->height() != zf->height())
        {
            ++result.outcomeDifferences;
            continue;
        }
        result.storageErrors += zf->hasChroma() || zf->uData() != nullptr ||
            zf->vData() != nullptr || zf->uvStride() != 0U;
        // The current pipeline can return FrameDecoded after an MB loop
        // breaks. Missing trailing MB events must not count as a full decode.
        uint32_t visitedMbs = 0U;
        for (const auto& e : fullSyntax)
            if (e[0] == static_cast<uint32_t>(TraceEventType::MbStart))
                visitedMbs = std::max(visitedMbs, e[2] * (af->width() / 16U) + e[1] + 1U);
        result.partialFrames += visitedMbs != (af->width() / 16U) * (af->height() / 16U);
        for (uint32_t row = 0U; row < af->height(); ++row)
        {
            if (std::memcmp(af->yRow(row), zf->yRow(row), af->width()) == 0)
                continue;
            for (uint32_t col = 0U; col < af->width(); ++col)
                result.yMismatches += af->yRow(row)[col] != zf->yRow(row)[col];
        }
    }
    return result;
}

} // namespace sub0h264::test

#endif
