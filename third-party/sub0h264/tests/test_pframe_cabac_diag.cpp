#ifndef ESP_PLATFORM
/** Diagnostic: trace first P-frame MB(0,0) decode for CABAC High profile.
 *
 *  Decodes scrolling_texture_high.h264 and captures the CABAC engine state
 *  at each step of MB(0,0) in the first P-frame, to find where desync occurs.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include <memory>
#include <cstdio>
#include <vector>
#include "../components/sub0h264/src/decoder.hpp"
#include "../components/sub0h264/src/annexb.hpp"
#include "test_fixtures.hpp"

using namespace sub0h264;

TEST_CASE("CABAC P-frame diag: scrolling_texture_high first P-frame trace")
{
    auto h264 = getFixture("scrolling_texture_high.h264");
    if (h264.empty()) { MESSAGE("fixture not found"); return; }

    auto decoder = std::make_unique<H264Decoder>();

    uint32_t frameIdx = 0U;
    uint32_t firstPFrameIdx = UINT32_MAX;

    // Capture trace events for MB(0,0) and MB(1,0) of every frame
    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.mbX > 1U || e.mbY > 0U) return;
        if (e.type == TraceEventType::MbStart && e.a == 200U) // CABAC start
        {
            MESSAGE("Frame " << frameIdx << " MB(" << e.mbX << "," << e.mbY
                    << ") CABAC start: bitpos=" << e.b);
        }
        if (e.type == TraceEventType::MvPrediction)
        {
            // a=partIdx, b=mvpX, c=mvpY, d=bits consumed
            MESSAGE("Frame " << frameIdx << " MB(" << e.mbX << "," << e.mbY
                    << ") MV pred: partIdx=" << e.a
                    << " mvp=(" << static_cast<int16_t>(e.b) << ","
                    << static_cast<int16_t>(e.c) << ")");
        }
    });

    // Capture CABAC bins via the trace callback
    FILE* binLog = std::fopen("pframe_cabac_trace.txt", "w");
    REQUIRE(binLog != nullptr);
    std::fprintf(binLog, "# binIdx ctxIdx symbol range offset\n");
    auto origCallback = decoder->trace().callback_;
    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.type == TraceEventType::CabacBin)
        {
            std::fprintf(binLog, "%u %d %u %u %d\n",
                e.a, static_cast<int32_t>(e.b), e.c, e.d,
                e.data ? static_cast<int>(e.data[0]) : 0);
        }
        if (origCallback) origCallback(e);
    });

    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal)) continue;
        DecodeStatus status = decoder->processNal(nal);
        if (status == DecodeStatus::FrameDecoded)
        {
            const Frame* frame = decoder->currentFrame();
            if (frame && frameIdx == 0U)
            {
                MESSAGE("IDR frame " << frameIdx << " decoded: "
                        << frame->width() << "x" << frame->height());
            }
            if (frameIdx == 1U)
            {
                MESSAGE("=== First P-frame MB(0,0) first row ===");
                for (uint32_t c = 0; c < 16; ++c)
                    MESSAGE("  y(" << c << ",0) = " << (int)frame->y(c, 0));
                firstPFrameIdx = frameIdx;
            }
            ++frameIdx;
            if (frameIdx >= 3U) break;
        }
    }

    std::fclose(binLog);

    MESSAGE("Decoded " << frameIdx << " frames");
    MESSAGE("First P-frame at index: " << firstPFrameIdx);

    // Read the bin trace and report the first 100 lines
    binLog = std::fopen("pframe_cabac_trace.txt", "r");
    if (binLog)
    {
        int lineCount = 0;
        char line[512];
        while (std::fgets(line, sizeof(line), binLog) && lineCount < 100)
        {
            if (line[0] != '#')
            {
                MESSAGE("BIN[" << lineCount << "]: " << line);
                ++lineCount;
            }
        }
        std::fclose(binLog);
    }
}

#endif // ESP_PLATFORM
