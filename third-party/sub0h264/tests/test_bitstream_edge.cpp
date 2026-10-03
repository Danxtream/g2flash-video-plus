#ifndef ESP_PLATFORM
/** BitReader edge case tests — cache refill boundaries.
 *
 *  The BitReader uses a 64-bit (8-byte) cache window. These tests verify
 *  correct behavior when reads cross cache boundaries, which is critical
 *  for CABAC renormalization (continuous single-bit reads over many bytes).
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "../components/sub0h264/src/bitstream.hpp"
#include "../components/sub0h264/src/annexb.hpp"
#include "../components/sub0h264/src/sps.hpp"
#include "test_fixtures.hpp"

#include <vector>

using sub0h264::BitReader;

TEST_CASE("BitReader: readBit across cache refill boundary (byte 7→8)")
{
    // 16 bytes of known pattern — cache holds bytes 0-7, then refills at 8
    const uint8_t data[] = {
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xAA,  // cache window 1
        0x55, 0xCC, 0x33, 0xF0, 0x0F, 0xA5, 0x5A, 0x00,  // cache window 2
    };
    BitReader br(data, 16U);

    // Read all 64 bits of first cache window
    for (uint32_t i = 0U; i < 56U; ++i)
        CHECK(br.readBit() == 1U); // 7 bytes of 0xFF

    // Byte 7 = 0xAA = 10101010
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);

    // Now at bit 64 = byte 8 — cache refill happens here
    // Byte 8 = 0x55 = 01010101
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);

    // Byte 9 = 0xCC = 11001100
    CHECK(br.readBits(8U) == 0xCCU);
}

TEST_CASE("BitReader: readBits(N) spanning cache boundary")
{
    const uint8_t data[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF,  // byte 7 = 0xFF
        0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // byte 8 = 0x80
    };
    BitReader br(data, 16U);

    // Read 60 bits to get near the boundary (7.5 bytes)
    br.skipBits(60U);
    // Now at bit 60 (in byte 7). Next 8 bits span byte 7→8:
    // byte 7 bits 4-7 = 1111 (from 0xFF)
    // byte 8 bits 0-3 = 1000 (from 0x80)
    // Combined: 11111000 = 0xF8
    CHECK(br.readBits(8U) == 0xF8U);
}

TEST_CASE("BitReader: peekBits triggers cache refill correctly")
{
    const uint8_t data[] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // 8 zero bytes
        0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x00, 0x00, 0x00,  // known pattern
    };
    BitReader br(data, 16U);

    // Skip to byte 8
    br.skipBits(64U);
    // peekBits should refill cache starting at byte 8
    CHECK(br.peekBits(32U) == 0xDEADBEEFU);
    // bitOffset should not advance
    CHECK(br.bitOffset() == 64U);
    // readBits should return same value
    CHECK(br.readBits(32U) == 0xDEADBEEFU);
    CHECK(br.bitOffset() == 96U);
}

TEST_CASE("BitReader: readBit alternating with readBits near boundary")
{
    // Simulate CABAC pattern: many readBit() then readBits(N) for renorm
    const uint8_t data[] = {
        0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA,  // 8 bytes 0xAA
        0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,  // 8 bytes 0x55
    };
    BitReader br(data, 16U);

    // Read 62 bits one at a time (7 bytes + 6 bits of 0xAA)
    for (uint32_t i = 0U; i < 62U; ++i)
    {
        uint32_t expected = (i % 2 == 0) ? 1U : 0U; // 0xAA = 10101010
        CHECK(br.readBit() == expected);
    }
    CHECK(br.bitOffset() == 62U);

    // Now readBits(4) spanning byte 7 (0xAA) → byte 8 (0x55)
    // Bits 62-63 of 0xAA = 10, bits 64-65 of 0x55 = 01
    // Combined: 1001 = 9
    CHECK(br.readBits(4U) == 0b1001U);
    CHECK(br.bitOffset() == 66U);
}

TEST_CASE("BitReader: readUev large value (leading zeros > 8)")
{
    // ue(v) for value 255: codeNum=255 = 2^8 - 1 + suffix
    // Encoding: 00000000 1 XXXXXXXX where XXXXXXXX = 0 for codeNum=255
    // Wait: codeNum = (1 << zeros) - 1 + suffix
    // For 8 leading zeros: codeNum = 255 + suffix (0..255)
    // ue(255) = 8 zeros + 1 + 8-bit suffix 00000000 = 00000000 1 00000000
    const uint8_t data[] = { 0x00, 0x80, 0x00 }; // 00000000 10000000 00000000
    BitReader br(data, 3U);

    CHECK(br.readUev() == 255U); // 8 leading zeros → (1<<8)-1+0 = 255
}

TEST_CASE("BitReader: readUev value 0 is single bit")
{
    const uint8_t data[] = { 0x80 }; // 1xxxxxxx
    BitReader br(data, 1U);

    CHECK(br.readUev() == 0U);
    CHECK(br.bitOffset() == 1U);
}

TEST_CASE("BitReader: readSev negative values")
{
    // se(-3) → ue(6) = 00111 (5 bits: 2 zeros + 1 + 2 suffix bits = 11)
    // se(-1) → ue(2) = 011 (3 bits)
    // Pack: 00111 011 = 0b00111011 = 0x3B
    const uint8_t data[] = { 0x3B };
    BitReader br(data, 1U);

    CHECK(br.readSev() == -3);  // ue=6 → se=-3
    CHECK(br.readSev() == -1);  // ue=2 → se=-1
}

TEST_CASE("BitReader: readBits(0) returns 0")
{
    const uint8_t data[] = { 0xFF };
    BitReader br(data, 1U);

    CHECK(br.readBits(0U) == 0U);
    CHECK(br.bitOffset() == 0U);
}

TEST_CASE("BitReader: SPS for CABAC fixture parses correctly")
{
    // Verify the Main profile CABAC fixture SPS parses without error
    // This exercises the BitReader on real CABAC slice data
    auto fixture = getFixture("cabac_flat_main.h264");
    REQUIRE_FALSE(fixture.empty());

    std::vector<sub0h264::NalBounds> bounds;
    sub0h264::findNalUnits(fixture.data(), static_cast<uint32_t>(fixture.size()), bounds);

    bool foundSps = false;
    for (const auto& b : bounds)
    {
        sub0h264::NalUnit nal;
        if (!sub0h264::parseNalUnit(fixture.data() + b.offset, b.size, nal))
            continue;
        if (nal.type != sub0h264::NalType::Sps)
            continue;

        sub0h264::BitReader br(nal.rbspData.data(),
                                static_cast<uint32_t>(nal.rbspData.size()));
        sub0h264::Sps sps;
        CHECK(sub0h264::parseSps(br, sps) == sub0h264::Result::Ok);
        CHECK(sps.profileIdc_ == sub0h264::cProfileMain);
        CHECK(sps.width() == 320U);
        CHECK(sps.height() == 240U);
        CHECK(sps.picOrderCntType_ == 2U);
        foundSps = true;
    }
    CHECK(foundSps);
}

#endif // ESP_PLATFORM
