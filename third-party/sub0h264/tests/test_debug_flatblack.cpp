/** Debug test: inspect flat_black decoded pixel values for root cause analysis.
 *  Temporary — will be removed once CRC matches.
 */
#include "doctest.h"
#include "test_fixtures.hpp"
#include "../components/sub0h264/src/decoder.hpp"
#include "../components/sub0h264/src/frame_verify.hpp"

#include <cstdio>
#include <memory>

using namespace sub0h264;

TEST_CASE("Debug: flat_black NAL + mb_type analysis")
{
    auto data = getFixture("flat_black_baseline_640x480.h264");
    REQUIRE_FALSE(data.empty());

    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "NAL count: %lu", (unsigned long)bounds.size());
    MESSAGE(buf);

    // Parse SPS/PPS, then inspect first few bytes of IDR slice data
    for (uint32_t i = 0U; i < bounds.size() && i < 5U; ++i)
    {
        NalUnit nal;
        if (!parseNalUnit(data.data() + bounds[i].offset, bounds[i].size, nal))
            continue;

        if (nal.type == NalType::SliceIdr && nal.rbspData.size() > 10U)
        {
            // Parse slice header to get past it, then peek at MB data
            const Sps* sps = nullptr;
            const Pps* pps = nullptr;

            // Quick SPS/PPS parse
            for (uint32_t j = 0U; j < i; ++j)
            {
                NalUnit prev;
                if (!parseNalUnit(data.data() + bounds[j].offset, bounds[j].size, prev))
                    continue;
                // Just report what we see
            }

            // Report first bytes of RBSP for manual analysis
            std::snprintf(buf, sizeof(buf),
                "IDR RBSP (first 10 bytes): %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                nal.rbspData[0], nal.rbspData[1], nal.rbspData[2], nal.rbspData[3],
                nal.rbspData[4], nal.rbspData[5], nal.rbspData[6], nal.rbspData[7],
                nal.rbspData[8], nal.rbspData[9]);
            MESSAGE(buf);

            // Parse first few fields of slice header manually
            BitReader br(nal.rbspData.data(), static_cast<uint32_t>(nal.rbspData.size()));
            uint32_t firstMb = br.readUev();
            uint32_t sliceType = br.readUev();
            uint32_t ppsId = br.readUev();
            std::snprintf(buf, sizeof(buf),
                "Slice header: first_mb=%lu slice_type=%lu pps_id=%lu",
                (unsigned long)firstMb, (unsigned long)sliceType, (unsigned long)ppsId);
            MESSAGE(buf);
        }
    }
}

TEST_CASE("Debug: dump baseline frame 0 to file for comparison")
{
    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);

    auto decoder = std::make_unique<H264Decoder>();
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(data.data() + b.offset, b.size, nal))
            continue;
        DecodeStatus status = decoder->processNal(nal);
        if (status == DecodeStatus::FrameDecoded)
            break;
    }

    const Frame* frame = decoder->currentFrame();
    REQUIRE(frame != nullptr);

    // Dump Y plane to file for comparison with ffmpeg reference
#ifndef ESP_PLATFORM
    FILE* f = std::fopen("debug_baseline_frame0.yuv", "wb");
    if (f)
    {
        // Write I420: Y plane, then U, then V (no stride padding)
        for (uint32_t r = 0U; r < frame->height(); ++r)
            std::fwrite(frame->yRow(r), 1, frame->width(), f);
        for (uint32_t r = 0U; r < frame->height() / 2U; ++r)
            std::fwrite(frame->uRow(r), 1, frame->width() / 2U, f);
        for (uint32_t r = 0U; r < frame->height() / 2U; ++r)
            std::fwrite(frame->vRow(r), 1, frame->width() / 2U, f);
        std::fclose(f);
        MESSAGE("Dumped baseline frame 0 to debug_baseline_frame0.yuv");
    }
#endif

    uint32_t crc = frameCrc32(*frame);
    char buf[128];
    std::snprintf(buf, sizeof(buf), "Baseline frame 0 CRC: 0x%08lx (ref 0x62f18fa1)",
                  (unsigned long)crc);
    MESSAGE(buf);
}

TEST_CASE("Debug: flat_black pixel inspection")
{
    auto data = getFixture("flat_black_baseline_640x480.h264");
    REQUIRE_FALSE(data.empty());

    // Decode just the first IDR frame
    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);

    auto decoder = std::make_unique<H264Decoder>();

    // Process NALs until we get the first frame
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(data.data() + b.offset, b.size, nal))
            continue;
        DecodeStatus status = decoder->processNal(nal);
        if (status == DecodeStatus::FrameDecoded)
            break;
    }

    const Frame* frame = decoder->currentFrame();
    REQUIRE(frame != nullptr);
    CHECK(frame->width() == 640U);
    CHECK(frame->height() == 480U);

    // Inspect pixel values
    // Reference: Y=16, U=128, V=128 (BT.601 black)
    uint8_t y00 = frame->y(0U, 0U);
    uint8_t y_mid = frame->y(320U, 240U);
    uint8_t u00 = frame->u(0U, 0U);
    uint8_t v00 = frame->v(0U, 0U);

    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "Pixel values: Y(0,0)=%u Y(320,240)=%u U(0,0)=%u V(0,0)=%u "
        "(expected Y=16 U=128 V=128)",
        y00, y_mid, u00, v00);
    MESSAGE(buf);

    // Check a few Y values across the frame
    bool allYSame = true;
    uint8_t firstY = frame->y(0U, 0U);
    for (uint32_t row = 0U; row < frame->height() && allYSame; row += 16U)
        for (uint32_t col = 0U; col < frame->width() && allYSame; col += 16U)
            if (frame->y(col, row) != firstY)
                allYSame = false;

    std::snprintf(buf, sizeof(buf),
        "All Y pixels same value: %s (value=%u)", allYSame ? "yes" : "no", firstY);
    MESSAGE(buf);

    // CRC of our output vs reference
    uint32_t ourCrc = frameCrc32(*frame);
    std::snprintf(buf, sizeof(buf),
        "Our CRC: 0x%08lx  Reference: 0x%08lx  Match: %s",
        (unsigned long)ourCrc, (unsigned long)cRefCrcFlatBlack[0],
        ourCrc == cRefCrcFlatBlack[0] ? "YES" : "NO");
    MESSAGE(buf);

    // Expected values for BT.601 black
    /// BT.601 "legal black" luma value (Y=16).
    static constexpr uint8_t cBt601BlackY = 16U;
    /// BT.601 neutral chroma value (U=V=128).
    static constexpr uint8_t cBt601NeutralChroma = 128U;
    CHECK(y00 == cBt601BlackY);
    CHECK(u00 == cBt601NeutralChroma);
    CHECK(v00 == cBt601NeutralChroma);
}
