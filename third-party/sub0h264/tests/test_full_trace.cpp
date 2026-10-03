#ifndef ESP_PLATFORM
/** Sub0h264 — Full decode trace for bouncing ball
 *
 *  Generates a comprehensive per-block trace file that can be diffed
 *  against the equivalent trace from libavc to find decode divergences.
 *
 *  Format: one line per event, structured for direct diff comparison.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "test_fixtures.hpp"
#include "../components/sub0h264/src/decoder.hpp"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>

using namespace sub0h264;

static uint16_t crc16_buf(const int16_t* data, uint32_t count) {
    uint16_t crc = 0xFFFFU;
    for (uint32_t i = 0; i < count; ++i) {
        uint16_t v = static_cast<uint16_t>(data[i]);
        crc ^= v;
        for (int b = 0; b < 16; ++b)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xA001U : 0U);
    }
    return crc;
}

TEST_CASE("Full trace: bouncing ball IDR block-level decode")
{
    auto h264 = getFixture("bouncing_ball_ionly_baseline.h264");
    if (h264.empty()) { MESSAGE("fixture not found"); return; }

    FILE* fp = std::fopen("trace_sub0h264.txt", "w");
    REQUIRE(fp != nullptr);

    auto decoder = std::make_unique<H264Decoder>();
    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.mbY > 0U) return; // Only first row for now

        switch (e.type)
        {
        case TraceEventType::MbStart:
            std::fprintf(fp, "MB %u %u bit=%u type=%u\n",
                         e.mbX, e.mbY, e.b, e.a);
            break;

        case TraceEventType::MbEnd:
            // QP trace: a=qp, b=qpDelta, c=cbp, d=bitOffset
            std::fprintf(fp, "QP %u %u qp=%u delta=%d cbp=0x%02x bit=%u\n",
                         e.mbX, e.mbY, e.a, static_cast<int32_t>(e.b), e.c, e.d);
            break;

        case TraceEventType::BlockPredMode:
            std::fprintf(fp, "PRED %u %u scan=%u raster=%u mode=%u mpm=%u\n",
                         e.mbX, e.mbY, e.a, e.b, e.c, e.d);
            break;

        case TraceEventType::BlockResidual:
            if (e.data && e.dataLen == 16U)
            {
                uint16_t crc = crc16_buf(e.data, 16);
                std::fprintf(fp, "RAW %u %u scan=%u nC=%u tc=%u bits=%u crc=%04x coeffs=[",
                             e.mbX, e.mbY, e.a, e.b, e.c, e.d, crc);
                for (uint32_t i = 0; i < 16; ++i)
                    std::fprintf(fp, "%d%s", e.data[i], i < 15 ? " " : "");
                std::fprintf(fp, "]\n");
            }
            break;

        case TraceEventType::BlockCoeffs:
            if (e.data && e.dataLen == 16U)
            {
                uint16_t crc = crc16_buf(e.data, 16);
                // Also show column 0 values explicitly (key for row-constant errors)
                std::fprintf(fp, "DQ %u %u scan=%u crc=%04x col0=[%d %d %d %d]\n",
                             e.mbX, e.mbY, e.a, crc,
                             e.data[0], e.data[4], e.data[8], e.data[12]);
            }
            break;

        case TraceEventType::BlockPixels:
            if (e.data && e.dataLen == 32U)
            {
                uint16_t predCrc = crc16_buf(e.data, 16);
                uint16_t outCrc = crc16_buf(e.data + 16, 16);
                std::fprintf(fp, "PIX %u %u scan=%u pred_crc=%04x out_crc=%04x",
                             e.mbX, e.mbY, e.a, predCrc, outCrc);
                // Show first row of pred and output
                std::fprintf(fp, " pred0=[%d %d %d %d] out0=[%d %d %d %d]\n",
                             e.data[0], e.data[1], e.data[2], e.data[3],
                             e.data[16], e.data[17], e.data[18], e.data[19]);
            }
            break;

        default:
            break;
        }
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

    std::fclose(fp);
    MESSAGE("Trace written to trace_sub0h264.txt (in build dir)");

    // Verify file has content
    fp = std::fopen("trace_sub0h264.txt", "r");
    REQUIRE(fp != nullptr);
    std::fseek(fp, 0, SEEK_END);
    long size = std::ftell(fp);
    std::fclose(fp);
    MESSAGE("Trace file size: " << size << " bytes");
    CHECK(size > 1000);
}

#endif // ESP_PLATFORM
