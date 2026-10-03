#ifndef ESP_PLATFORM
/** Diagnostic: trace CABAC engine state at key decode points.
 *
 *  Decodes cabac_flat_main.h264 with the main decoder, capturing
 *  the CABAC engine range/offset at MB(0,0) block boundaries.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include <memory>
#include "../components/sub0h264/src/decoder.hpp"
#include "../components/sub0h264/src/annexb.hpp"
#include "test_fixtures.hpp"

#include <cstdio>

using namespace sub0h264;

TEST_CASE("CABAC diag: flat_main MB(0,0) engine state trace")
{
    auto h264 = getFixture("cabac_flat_main.h264");
    REQUIRE(!h264.empty());

    auto decoder = std::make_unique<H264Decoder>();

    // Use trace callback to capture per-block decode info for MB(0,0)
    struct BlockInfo { uint32_t scanIdx; int16_t coeffs[16]; uint8_t pred[16]; uint8_t out[16]; };
    std::vector<BlockInfo> blocks;
    uint32_t mbStartBit = 0;
    uint32_t cbpValue = 0;

    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.mbX == 0 && e.mbY == 0)
        {
            if (e.type == TraceEventType::MbStart)
                mbStartBit = e.a;
            else if (e.type == TraceEventType::BlockResidual && e.a < 16)
            {
                // a=scanIdx, b=nC, c=totalCoeff, d=bitsConsumed
                MESSAGE("  blk_scan" << e.a << ": nC=" << (int)e.b
                        << " tc=" << e.c << " bits=" << e.d);
            }
            else if (e.type == TraceEventType::BlockCoeffs && e.a < 16)
            {
                BlockInfo bi;
                bi.scanIdx = e.a;
                if (e.data && e.dataLen >= 16)
                    std::memcpy(bi.coeffs, e.data, 16 * sizeof(int16_t));
                blocks.push_back(bi);
            }
            else if (e.type == TraceEventType::BlockPixels && e.a < 4)
            {
                // data = pred[16] then output[16]
                if (e.data && e.dataLen >= 32)
                {
                    MESSAGE("  blk_scan" << e.a << " pred: ["
                            << e.data[0] << "," << e.data[1] << ","
                            << e.data[2] << "," << e.data[3] << "]"
                            << " out: [" << e.data[16] << "," << e.data[17] << ","
                            << e.data[18] << "," << e.data[19] << "]");
                }
            }
        }
    });

    // Also capture CABAC bins via the existing callback (append bin events)
    FILE* binLog = std::fopen("cabac_diag_trace.txt", "w");
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
        // Forward to the original callback for structural events
        if (origCallback) origCallback(e);
    });

    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal)) continue;
        if (decoder->processNal(nal) == DecodeStatus::FrameDecoded)
            break;
    }

    std::fclose(binLog);

    MESSAGE("MB(0,0) start bit: " << mbStartBit);
    MESSAGE("Blocks with dequant coefficients captured: " << blocks.size());
    for (const auto& bi : blocks)
    {
        if (bi.scanIdx < 4)
        {
            char buf[200];
            std::snprintf(buf, sizeof(buf),
                "  blk_scan%u dequant: [%d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d, %d,%d,%d,%d]",
                bi.scanIdx,
                bi.coeffs[0],bi.coeffs[1],bi.coeffs[2],bi.coeffs[3],
                bi.coeffs[4],bi.coeffs[5],bi.coeffs[6],bi.coeffs[7],
                bi.coeffs[8],bi.coeffs[9],bi.coeffs[10],bi.coeffs[11],
                bi.coeffs[12],bi.coeffs[13],bi.coeffs[14],bi.coeffs[15]);
            MESSAGE(buf);
        }
    }

    const Frame* frame = decoder->currentFrame();
    REQUIRE(frame != nullptr);

    // Report first 4 blocks' pixel output
    MESSAGE("=== Main decoder MB(0,0) pixel output ===");
    for (uint32_t blkIdx = 0; blkIdx < 4; ++blkIdx)
    {
        uint32_t blkX = (blkIdx & 1) * 4;
        uint32_t blkY = (blkIdx >> 1) * 4;
        MESSAGE("Block " << blkIdx << " at (" << blkX << "," << blkY << "):");
        for (uint32_t r = 0; r < 4; ++r)
        {
            char buf[80];
            std::snprintf(buf, sizeof(buf), "  [%3d %3d %3d %3d]",
                frame->y(blkX+0, blkY+r), frame->y(blkX+1, blkY+r),
                frame->y(blkX+2, blkY+r), frame->y(blkX+3, blkY+r));
            MESSAGE(buf);
        }
    }

    // Count bins consumed (from trace file)
    binLog = std::fopen("cabac_diag_trace.txt", "r");
    if (binLog)
    {
        int lineCount = 0;
        char line[256];
        while (std::fgets(line, sizeof(line), binLog))
            if (line[0] != '#') ++lineCount;
        std::fclose(binLog);
        MESSAGE("Total context-coded bins traced: " << lineCount);
    }
}

TEST_CASE("CABAC diag: bouncing_ball_ionly_cabac MB(0,0) pixel output")
{
    auto h264 = getFixture("bouncing_ball_ionly_main.h264");
    if (h264.empty()) { MESSAGE("fixture not found"); return; }

    auto decoder = std::make_unique<H264Decoder>();

    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal)) continue;
        if (decoder->processNal(nal) == DecodeStatus::FrameDecoded)
            break;
    }

    const Frame* frame = decoder->currentFrame();
    REQUIRE(frame != nullptr);

    // Get raw reference
    auto raw = getFixture("bouncing_ball_ionly_main_raw.yuv");

    MESSAGE("=== Main decoder bouncing_ball_ionly_cabac MB(0,0) ===");
    for (uint32_t r = 0; r < 16; ++r)
    {
        char buf[200];
        char* p = buf;
        p += std::snprintf(p, 20, "row %2u main:", r);
        for (uint32_t c = 0; c < 16; ++c)
            p += std::snprintf(p, 6, " %3d", frame->y(c, r));
        MESSAGE(buf);
    }

    if (!raw.empty())
    {
        uint32_t w = frame->width();
        MESSAGE("=== Raw reference MB(0,0) ===");
        for (uint32_t r = 0; r < 16; ++r)
        {
            char buf[200];
            char* p = buf;
            p += std::snprintf(p, 20, "row %2u raw: ", r);
            for (uint32_t c = 0; c < 16; ++c)
                p += std::snprintf(p, 6, " %3d", raw[r * w + c]);
            MESSAGE(buf);
        }
    }
}

#endif // ESP_PLATFORM
