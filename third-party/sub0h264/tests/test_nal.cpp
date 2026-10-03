#include "doctest.h"
#include "../components/sub0h264/src/annexb.hpp"

#include "test_fixtures.hpp"

#include <cstdio>
#include <vector>

using namespace sub0h264;

TEST_CASE("findNalUnits with simple 3-byte start codes")
{
    // Two NALs: [00 00 01 <A>] [00 00 01 <B C>]
    const uint8_t data[] = {
        0x00, 0x00, 0x01, 0x65,             // NAL 1: IDR (type 5)
        0x00, 0x00, 0x01, 0x67, 0x42,       // NAL 2: SPS (type 7) + 1 byte
    };

    std::vector<NalBounds> nals;
    findNalUnits(data, sizeof(data), nals);

    REQUIRE(nals.size() == 2U);
    CHECK(nals[0].size == 1U);   // just 0x65
    CHECK(nals[1].size == 2U);   // 0x67 0x42
}

TEST_CASE("findNalUnits with 4-byte start codes")
{
    const uint8_t data[] = {
        0x00, 0x00, 0x00, 0x01, 0x67, 0x42,  // NAL 1: SPS
        0x00, 0x00, 0x00, 0x01, 0x68, 0x01,  // NAL 2: PPS
    };

    std::vector<NalBounds> nals;
    findNalUnits(data, sizeof(data), nals);

    REQUIRE(nals.size() == 2U);
    CHECK(data[nals[0].offset] == 0x67U); // SPS header byte
    CHECK(data[nals[1].offset] == 0x68U); // PPS header byte
}

TEST_CASE("removeEmulationPrevention strips 0x03 bytes")
{
    // 00 00 03 00 → 00 00 00 (emulation byte removed)
    const uint8_t ebsp[] = { 0x00, 0x00, 0x03, 0x00 };
    std::vector<uint8_t> rbsp;
    removeEmulationPrevention(ebsp, sizeof(ebsp), rbsp);

    REQUIRE(rbsp.size() == 3U);
    CHECK(rbsp[0] == 0x00U);
    CHECK(rbsp[1] == 0x00U);
    CHECK(rbsp[2] == 0x00U);
}

TEST_CASE("removeEmulationPrevention handles 00 00 03 01")
{
    // 00 00 03 01 → 00 00 01
    const uint8_t ebsp[] = { 0x00, 0x00, 0x03, 0x01 };
    std::vector<uint8_t> rbsp;
    removeEmulationPrevention(ebsp, sizeof(ebsp), rbsp);

    REQUIRE(rbsp.size() == 3U);
    CHECK(rbsp[0] == 0x00U);
    CHECK(rbsp[1] == 0x00U);
    CHECK(rbsp[2] == 0x01U);
}

TEST_CASE("removeEmulationPrevention passes through normal data")
{
    const uint8_t data[] = { 0x12, 0x34, 0x56, 0x78 };
    std::vector<uint8_t> rbsp;
    removeEmulationPrevention(data, sizeof(data), rbsp);

    REQUIRE(rbsp.size() == 4U);
    CHECK(rbsp[0] == 0x12U);
    CHECK(rbsp[3] == 0x78U);
}

TEST_CASE("parseNalUnit extracts header fields")
{
    // NAL byte: forbidden=0, ref_idc=3 (11), type=7 (00111) → 0b0_11_00111 = 0x67
    const uint8_t data[] = { 0x67, 0x42, 0x00 };
    NalUnit nal;
    REQUIRE(parseNalUnit(data, sizeof(data), nal));

    CHECK(nal.type == NalType::Sps);
    CHECK(nal.refIdc == 3U);
    CHECK(nal.rbspData.size() == 2U);
}

TEST_CASE("parseNalUnit rejects forbidden bit")
{
    // Forbidden bit set: 1_00_00001 = 0x81
    const uint8_t data[] = { 0x81, 0x00 };
    NalUnit nal;
    CHECK_FALSE(parseNalUnit(data, sizeof(data), nal));
}

// FM-23: Emulation prevention edge cases — §7.4.1
TEST_CASE("removeEmulationPrevention handles consecutive emulation bytes")
{
    // 00 00 03 00 00 03 05 → 00 00 00 00 05
    // Two back-to-back emulation sequences: each 00 00 03 is stripped once.
    // After first removal: stream is 00 00 [00 00 03] 05 → second 03 also stripped.
    // [CHECKED §7.4.1]
    const uint8_t ebsp[] = { 0x00, 0x00, 0x03, 0x00, 0x00, 0x03, 0x05 };
    std::vector<uint8_t> rbsp;
    removeEmulationPrevention(ebsp, sizeof(ebsp), rbsp);

    REQUIRE(rbsp.size() == 5U);
    CHECK(rbsp[0] == 0x00U);
    CHECK(rbsp[1] == 0x00U);
    CHECK(rbsp[2] == 0x00U);
    CHECK(rbsp[3] == 0x00U);
    CHECK(rbsp[4] == 0x05U);
}

TEST_CASE("removeEmulationPrevention handles emulation byte at end of buffer")
{
    // 00 00 03 at end with no trailing byte: 03 is still removed.
    // §7.4.1: emulation_prevention_three_byte must be removed regardless.
    // [CHECKED §7.4.1]
    const uint8_t ebsp[] = { 0xAB, 0x00, 0x00, 0x03 };
    std::vector<uint8_t> rbsp;
    removeEmulationPrevention(ebsp, sizeof(ebsp), rbsp);

    REQUIRE(rbsp.size() == 3U);
    CHECK(rbsp[0] == 0xABU);
    CHECK(rbsp[1] == 0x00U);
    CHECK(rbsp[2] == 0x00U);
}

TEST_CASE("removeEmulationPrevention handles emulation byte mid-stream")
{
    // AB 00 00 03 02 CD → AB 00 00 02 CD
    // §7.4.1: 03 between any two bytes is removed when preceded by 00 00.
    // [CHECKED §7.4.1]
    const uint8_t ebsp[] = { 0xAB, 0x00, 0x00, 0x03, 0x02, 0xCD };
    std::vector<uint8_t> rbsp;
    removeEmulationPrevention(ebsp, sizeof(ebsp), rbsp);

    REQUIRE(rbsp.size() == 5U);
    CHECK(rbsp[0] == 0xABU);
    CHECK(rbsp[1] == 0x00U);
    CHECK(rbsp[2] == 0x00U);
    CHECK(rbsp[3] == 0x02U);
    CHECK(rbsp[4] == 0xCDU);
}

TEST_CASE("Parse flat_black_640x480.h264 NAL sequence")
{
    auto data = getFixture("flat_black_baseline_640x480.h264");
    REQUIRE_FALSE(data.empty());

    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);

    REQUIRE(bounds.size() >= 3U); // At least SPS + PPS + IDR

    // Parse each NAL and collect types
    std::vector<NalType> types;
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (parseNalUnit(data.data() + b.offset, b.size, nal))
            types.push_back(nal.type);
    }

    // Expect SPS, PPS, then IDR (possibly with SEI in between)
    bool foundSps = false, foundPps = false, foundIdr = false;
    for (auto t : types)
    {
        if (t == NalType::Sps) foundSps = true;
        if (t == NalType::Pps) foundPps = true;
        if (t == NalType::SliceIdr) foundIdr = true;
    }
    CHECK(foundSps);
    CHECK(foundPps);
    CHECK(foundIdr);
}

TEST_CASE("Parse baseline_640x480_short.h264 NAL sequence")
{
    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);

    REQUIRE(bounds.size() >= 5U); // SPS + PPS + IDR + at least 2 P-frames

    // Parse all and count types
    uint32_t spsCount = 0U, ppsCount = 0U, idrCount = 0U, pCount = 0U;
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(data.data() + b.offset, b.size, nal))
            continue;

        switch (nal.type)
        {
            case NalType::Sps:         ++spsCount; break;
            case NalType::Pps:         ++ppsCount; break;
            case NalType::SliceIdr:    ++idrCount; break;
            case NalType::SliceNonIdr: ++pCount;   break;
            default: break;
        }
    }

    CHECK(spsCount >= 1U);
    CHECK(ppsCount >= 1U);
    CHECK(idrCount >= 1U);
    CHECK(pCount >= 2U);  // At least some P-frames

    MESSAGE("NALs found: " << bounds.size()
            << " (SPS=" << spsCount
            << " PPS=" << ppsCount
            << " IDR=" << idrCount
            << " P=" << pCount << ")");
}
