#include "doctest.h"
#include "../components/sub0h264/src/bitstream.hpp"

using sub0h264::BitReader;

TEST_CASE("readBits reads single bits")
{
    // 0xA5 = 1010 0101
    const uint8_t data[] = { 0xA5 };
    BitReader br(data, 1U);

    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.readBit() == 0U);
    CHECK(br.readBit() == 1U);
    CHECK(br.isExhausted());
}

TEST_CASE("readBits reads multi-bit values")
{
    // 0xAB 0xCD = 1010 1011  1100 1101
    const uint8_t data[] = { 0xAB, 0xCD };
    BitReader br(data, 2U);

    CHECK(br.readBits(4U) == 0x0AU);  // 1010
    CHECK(br.readBits(4U) == 0x0BU);  // 1011
    CHECK(br.readBits(8U) == 0xCDU);  // 1100 1101
}

TEST_CASE("readBits crosses byte boundaries")
{
    // 0xFF 0x00 = 1111 1111  0000 0000
    const uint8_t data[] = { 0xFF, 0x00 };
    BitReader br(data, 2U);

    br.skipBits(4U);
    CHECK(br.readBits(8U) == 0xF0U);  // 1111 0000 (crosses boundary)
}

TEST_CASE("readBits 32-bit value")
{
    const uint8_t data[] = { 0x12, 0x34, 0x56, 0x78 };
    BitReader br(data, 4U);

    CHECK(br.readBits(32U) == 0x12345678U);
}

TEST_CASE("peekBits does not advance offset")
{
    const uint8_t data[] = { 0xAB };
    BitReader br(data, 1U);

    CHECK(br.peekBits(8U) == 0xABU);
    CHECK(br.bitOffset() == 0U);
    CHECK(br.peekBits(4U) == 0x0AU);
    CHECK(br.bitOffset() == 0U);
}

TEST_CASE("readUev decodes unsigned Exp-Golomb")
{
    // ue(v) encodings:
    //   0 → 1           (1 bit)
    //   1 → 010          (3 bits)
    //   2 → 011          (3 bits)
    //   3 → 00100        (5 bits)
    //   4 → 00101        (5 bits)
    //   5 → 00110        (5 bits)
    //   6 → 00111        (5 bits)
    //   7 → 0001000      (7 bits)

    // Encode: 0, 1, 2, 3 → binary: 1 010 011 00100
    // = 1_010_011_00100 = 1010 0110 0100 xxxx
    // Pad with zeros: 0xA6 0x40
    const uint8_t data[] = { 0xA6, 0x40 };
    BitReader br(data, 2U);

    CHECK(br.readUev() == 0U);  // 1
    CHECK(br.readUev() == 1U);  // 010
    CHECK(br.readUev() == 2U);  // 011
    CHECK(br.readUev() == 3U);  // 00100
}

TEST_CASE("readSev decodes signed Exp-Golomb")
{
    // se(v) mapping: ue=0→0, ue=1→+1, ue=2→-1, ue=3→+2, ue=4→-2

    // Encode: se=0, se=+1, se=-1, se=+2 → ue=0, 1, 2, 3
    // Same bitstream as readUev test
    const uint8_t data[] = { 0xA6, 0x40 };
    BitReader br(data, 2U);

    CHECK(br.readSev() == 0);   // ue=0 → 0
    CHECK(br.readSev() == 1);   // ue=1 → +1
    CHECK(br.readSev() == -1);  // ue=2 → -1
    CHECK(br.readSev() == 2);   // ue=3 → +2
}

TEST_CASE("isAligned and alignToByte")
{
    const uint8_t data[] = { 0xFF, 0xFF };
    BitReader br(data, 2U);

    CHECK(br.isAligned());
    br.readBits(3U);
    CHECK_FALSE(br.isAligned());
    br.alignToByte();
    CHECK(br.isAligned());
    CHECK(br.bitOffset() == 8U);
}

TEST_CASE("hasBits and isExhausted")
{
    const uint8_t data[] = { 0xFF };
    BitReader br(data, 1U);

    CHECK(br.hasBits(8U));
    CHECK_FALSE(br.hasBits(9U));
    CHECK_FALSE(br.isExhausted());

    br.skipBits(8U);
    CHECK(br.isExhausted());
    CHECK_FALSE(br.hasBits(1U));
}
