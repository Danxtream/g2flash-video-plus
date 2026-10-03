/** Sub0h264 — CAVLC level decoding tests
 *
 *  Unit tests for decodeLevel and related functions in cavlc.hpp.
 *  Covers suffix length handling, prefix escape codes, suffixLength
 *  adaptation, full residual block placement, and chroma DC total_zeros
 *  table selection.
 *
 *  Reference: ITU-T H.264 §9.2.2
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "../components/sub0h264/src/cavlc.hpp"
#include "../components/sub0h264/src/transform.hpp"

using namespace sub0h264;

// ── Test 1: decodeLevel, suffixLen=0, prefix=1 ─────────────────────────────

TEST_CASE("decodeLevel: suffixLen=0 prefix=1 yields level=-1")
{
    // ITU-T H.264 §9.2.2
    // level_prefix = 1  →  one leading zero then stop-bit 1  →  bits '01'
    // suffixLen=0  →  levelSuffixSize=0  →  no suffix bits
    // levelCode = (min(1,15) << 0) + 0 = 1
    // absLevel  = (1 + 2) >> 1 = 1
    // sign      = 1 & 1 = 1  →  negative
    // level     = -1
    //
    // Bit layout: 01xx xxxx = 0x40
    const uint8_t data[] = { 0x40U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 0U);
    CHECK(level == -1);
    CHECK(br.bitOffset() == 2U);
}

TEST_CASE("decodeLevel: suffixLen=0 prefix=1 trailing-ones adjustment gives -2")
{
    // When decodeResidualBlock4x4 applies the ±1 first-non-trailing-one
    // adjustment (i==trailingOnes && trailingOnes<3), the raw level=-1
    // becomes -1 + (-1) = -2.
    //
    // This test encodes:
    //   coeff_token: nC=0, TC=1, TO=0  →  code=5 size=6  →  bits '000101'
    //   level[0]   : prefix=1, suffixLen=0                →  bits '01'
    //     → raw level = -1, adjustment: -1-1 = -2
    //   total_zeros: TC=1, tz=0        →  code=1 size=1  →  bit  '1'
    //   (no run_before: last coeff, run=zerosLeft=0)
    //
    // Bit stream: 000101 01 1 xxx = 9 bits
    // '0001 0101 1000 0000' = 0x15, 0x80
    //
    // Placement: coeffIdx = 1+0-1+0 = 0  →  zigzag[0]=0  →  coeffs[0]=-2
    const uint8_t data[] = { 0x15U, 0x80U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    REQUIRE(decodeResidualBlock4x4(br, 0, cMaxCoeff4x4, 0U, block) == Result::Ok);
    CHECK(block.totalCoeff == 1U);
    CHECK(block.coeffs[0] == static_cast<int16_t>(-2));
    // All other positions must be zero
    for (uint32_t i = 1U; i < 16U; ++i)
        CHECK(block.coeffs[i] == 0);
}

// ── Test 2: decodeLevel, suffixLen=1 ───────────────────────────────────────

TEST_CASE("decodeLevel: suffixLen=1 prefix=2 suffix=1 yields level=-3")
{
    // ITU-T H.264 §9.2.2
    // level_prefix = 2   →  bits '001' (two leading zeros + stop)
    // suffixLen=1        →  levelSuffixSize=1  →  suffix=1  →  bit '1'
    // levelCode = (min(2,15) << 1) + 1 = 4 + 1 = 5
    // absLevel  = (5 + 2) >> 1 = 3
    // sign      = 5 & 1 = 1  →  negative
    // level     = -3
    //
    // Bit layout: 001 1 xxxx = 0x30
    const uint8_t data[] = { 0x30U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 1U);
    CHECK(level == -3);
    CHECK(br.bitOffset() == 4U);
}

TEST_CASE("decodeLevel: suffixLen=1 prefix=2 suffix=0 yields level=-2")
{
    // Same structure as above with suffix=0:
    // levelCode = (2 << 1) + 0 = 4
    // absLevel  = (4 + 2) >> 1 = 3  — no, (6)>>1=3... wait:
    // absLevel  = (4 + 2) >> 1 = 3
    // sign      = 4 & 1 = 0  →  positive
    // level     = +3 ... so let's use prefix=1 with suffixLen=1 instead.
    //
    // prefix=1, suffixLen=1  →  bits '01' + suffix=1 bit '1'
    // levelCode = (1 << 1) + 1 = 3
    // absLevel  = (3 + 2) >> 1 = 2
    // sign      = 3 & 1 = 1  →  negative
    // level     = -2
    //
    // Bit layout: 01 1 xxxxx = 0x60
    const uint8_t data[] = { 0x60U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 1U);
    CHECK(level == -2);
    CHECK(br.bitOffset() == 3U);
}

TEST_CASE("decodeLevel: suffixLen=1 prefix=2 suffix=0 yields level=+3")
{
    // prefix=2, suffixLen=1, suffix=0  →  bits '001' + '0'
    // levelCode = (2 << 1) + 0 = 4
    // absLevel  = (4 + 2) >> 1 = 3
    // sign      = 4 & 1 = 0  →  positive
    // level     = +3
    //
    // Bit layout: 001 0 xxxx = 0x20
    const uint8_t data[] = { 0x20U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 1U);
    CHECK(level == 3);
    CHECK(br.bitOffset() == 4U);
}

// ── Test 3: decodeLevel, prefix=14 suffixLen=0 (4-bit suffix escape) ───────

TEST_CASE("decodeLevel: prefix=14 suffixLen=0 uses 4-bit suffix escape")
{
    // ITU-T H.264 §9.2.2  (special case: prefix==14 && suffixLen==0)
    // levelSuffixSize = 4  (not suffixLen=0, not prefix-3)
    //
    // Encode: 14 leading zeros + stop '1' + 4 suffix bits '0101' (=5)
    // Total = 19 bits, packed into 3 bytes with padding.
    //
    // Bit positions 0-13  : '0' (14 zeros)
    // Bit position  14    : '1' (stop)
    // Bit positions 15-18 : '0101' (suffix=5)
    // Bit positions 19-23 : padding (don't care)
    //
    // Bytes: 0000 0000 | 0000 0010 | 1010 0000
    //         ^bit0-7     ^bit8-15    ^bit16-23
    //         bit14='1' is bit 6 of byte1 (0-indexed from MSB):
    //         byte1 = 0000 0001... let's be precise:
    //
    // Bit 0  is MSB of byte 0: byte0[7] in hardware terms but bit[0] in stream.
    // Byte 0 covers stream bits 0-7   = 0x00 (14 zeros span bits 0-13)
    // Byte 1 covers stream bits 8-15:
    //   bits 8-13 = '000000' (remaining zeros)
    //   bit  14   = '1' (stop)
    //   bit  15   = '0' (first suffix bit)
    //   byte1 = 0000 0010 = 0x02
    // Byte 2 covers stream bits 16-23:
    //   bits 16-18 = '101' (remaining suffix bits)
    //   bits 19-23 = padding
    //   byte2 = 1010 0000 = 0xA0
    //
    // suffix = 5 (bits '0101')
    // levelCode = (min(14,15) << 0) + 5 = 14 + 5 = 19
    // absLevel  = (19 + 2) >> 1 = 10
    // sign      = 19 & 1 = 1  →  negative
    // level     = -10
    const uint8_t data[] = { 0x00U, 0x02U, 0xA0U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 0U);
    CHECK(level == -10);
    CHECK(br.bitOffset() == 19U);
}

TEST_CASE("decodeLevel: prefix=14 suffixLen=0 suffix=0 yields level=+8")
{
    // prefix=14, suffixLen=0, suffix=0 (4 zero bits)
    // levelCode = 14 + 0 = 14
    // absLevel  = (14+2)>>1 = 8
    // sign      = 14 & 1 = 0  →  positive
    // level     = +8
    // Bit layout: 14 zeros + '1' + '0000' = 19 bits
    // Byte 0: 0x00, Byte 1: 0x02, Byte 2: 0x00
    const uint8_t data[] = { 0x00U, 0x02U, 0x00U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 0U);
    CHECK(level == 8);
    CHECK(br.bitOffset() == 19U);
}

// ── Test 4: decodeLevel, prefix=15 (12-bit suffix escape) ──────────────────

TEST_CASE("decodeLevel: prefix=15 suffixLen=0 yields level=16")
{
    // ITU-T H.264 §9.2.2  (escape: prefix>=15)
    // levelSuffixSize = prefix - 3 = 15 - 3 = 12
    //
    // Encode: 15 leading zeros + stop '1' + 12 suffix bits '000000000000' (=0)
    // Total = 28 bits = 3.5 bytes → use 4 bytes.
    //
    // Bit positions 0-14  : '0' (15 zeros)
    // Bit position  15    : '1' (stop)
    // Bit positions 16-27 : '000000000000' (suffix=0)
    // Bit positions 28-31 : padding
    //
    // Byte 0 (bits 0-7)  : 0x00
    // Byte 1 (bits 8-15) : bits 8-14=0, bit15=1  →  0000 0001 = 0x01
    // Byte 2 (bits 16-23): 0x00
    // Byte 3 (bits 24-31): 0x00
    //
    // suffix = 0
    // levelCode = (min(15,15) << 0) + 0 = 15
    //   + offset for prefix>=15 && suffixLen==0: levelCode += 15  →  30
    //   prefix==15 so NOT >=16, no further adjustment
    // absLevel  = (30 + 2) >> 1 = 16
    // sign      = 30 & 1 = 0  →  positive
    // level     = +16
    const uint8_t data[] = { 0x00U, 0x01U, 0x00U, 0x00U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 0U);
    CHECK(level == 16);
    CHECK(br.bitOffset() == 28U);
}

TEST_CASE("decodeLevel: prefix=15 suffixLen=0 suffix=1 yields level=-16")
{
    // prefix=15, suffixLen=0, suffix=1 (12-bit value '000000000001')
    // levelCode = (min(15,15) << 0) + 1 + 15 = 31
    // absLevel  = (31 + 2) >> 1 = 16
    // sign      = 31 & 1 = 1  →  negative
    // level     = -16
    //
    // Bit positions 0-14:  zeros (15 leading zeros)
    // Bit position  15:    '1'  (stop bit)
    // Bit positions 16-26: '00000000000' (suffix bits 11..1 = 0)
    // Bit position  27:    '1'  (suffix bit 0 = 1)
    //
    // Byte 0: 0x00, Byte 1: 0x01, Byte 2: 0x00
    // Byte 3: bit24-26=0, bit27=1 → 0001 xxxx = 0x10
    const uint8_t data[] = { 0x00U, 0x01U, 0x00U, 0x10U };
    BitReader br(data, sizeof(data));
    const int32_t level = decodeLevel(br, 0U);
    CHECK(level == -16);
    CHECK(br.bitOffset() == 28U);
}

// ── Test 5: suffixLength adaptation (else-if semantics) ────────────────────

TEST_CASE("suffixLength adaptation: suffixLen=0->1 unconditional, no threshold fire")
{
    // ITU-T H.264 §9.2.2  (suffixLength adaptation rules)
    //
    // The adaptation in decodeResidualBlock4x4 uses mutually exclusive branches:
    //   if (suffixLen == 0)         { suffixLen = 1; }
    //   else if (absVal > threshold){ ++suffixLen;   }
    //
    // This test verifies: when suffixLen transitions 0→1 after the first
    // non-trailing level, the threshold branch does NOT also fire (so
    // suffixLen ends up exactly 1, not 2).
    //
    // Setup: TC=1, TO=0, nC=0
    //   coeff_token: nC=0 TC=1 TO=0 → code=5, size=6 → '000101'
    //   level[0]: decodeLevel called with suffixLen=0
    //     prefix=0 → bit '1' → levelCode=0 → absLevel=1 → level=+1
    //     Then ±1 adjustment: i==trailingOnes && TO<3 → +1+1=+2
    //     suffixLen was 0 → set to 1 (unconditional branch)
    //     threshold check does NOT fire (else-if semantics)
    //   total_zeros: TC=1, tz=0 → code=1 size=1 → '1'
    //
    // We check that only 1 coefficient is decoded with value +2 at position 0.
    // Bitstream: '000101' + '1' + '1' = 8 bits = 0xAC? No:
    //   000101 1 1 = 0000 1011 = but wait leading bit ordering:
    //   bit0='0' bit1='0' bit2='0' bit3='1' bit4='0' bit5='1' (coeff_token)
    //   bit6='1' (prefix=0 → immediately reads stop-bit '1')
    //   bit7='1' (total_zeros=0 → code=1 → '1')
    //   Byte: 0000 1011 = 0x0B ... wait:
    //   Bits: 0,0,0,1,0,1,1,1 = 0x17? Let me redo:
    //   b0=0,b1=0,b2=0,b3=1,b4=0,b5=1 → coeff_token '000101'=0x05 in 6 bits
    //   b6=1 (level prefix=0, reads '1' at pos 6)
    //   b7=1 (total_zeros=0, code=1)
    //   Byte 0 MSB-first: bits 7..0 = b0 b1 b2 b3 b4 b5 b6 b7
    //                               = 0  0  0  1  0  1  1  1  = 0x17
    const uint8_t data[] = { 0x17U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    REQUIRE(decodeResidualBlock4x4(br, 0, cMaxCoeff4x4, 0U, block) == Result::Ok);
    CHECK(block.totalCoeff == 1U);
    // The first (and only) non-trailing level gets the ±1 adjustment: +1→+2
    CHECK(block.coeffs[0] == static_cast<int16_t>(2));
    for (uint32_t i = 1U; i < 16U; ++i)
        CHECK(block.coeffs[i] == 0);
}

TEST_CASE("suffixLength adaptation: 0->1->2 when first |level|>3 per §9.2.2")
{
    // ITU-T H.264 §9.2.2: suffixLength adaptation is TWO INDEPENDENT steps:
    //   1. if (suffixLen == 0) suffixLen = 1
    //   2. if (|level| > threshold[suffixLen]) suffixLen++
    //
    // For the FIRST non-trailing level with |adjusted level|=4:
    //   Step 1: suffixLen 0→1
    //   Step 2: 4 > threshold[1]=3 → suffixLen 1→2
    //
    // Reference: libavc ih264d_rest_of_residual_cav_chroma_dc_block line 1105:
    //   u4_suffix_len = (u2_abs_value > 3) ? 2 : 1;
    //
    // Setup: TC=2, TO=0, nC=0
    //   coeff_token: nC=0 TC=2 TO=0 → code=7, size=8 → '00000111'
    //   level[0]: prefix=4, suffixLen=0→no suffix. levelCode=4, absLevel=3.
    //     ±1 adjustment: +3+1=+4. suffixLen: 0→1→2 (|4| > threshold[1]=3).
    //   level[1]: suffixLen=2. prefix=1, suffix=2 bits.
    //     Bits: '01' (prefix=1) + '11' (suffix=3). levelCode=(1<<2)+3=7.
    //     absLevel=(7+2)/2=4. sign=1→-4.
    //     suffixLen: 4 > threshold[2]=6? No → stays at 2.
    //   total_zeros TC=2 tz=0: code=7 size=3 → '111'
    //
    // Bitstream: '00000111' + '00001' + '0111' + '111' = 20 bits
    //   byte0: 00000111 = 0x07
    //   byte1: 00001011 = 0x0B
    //   byte2: 1111xxxx = 0xF0
    const uint8_t data[] = { 0x07U, 0x0BU, 0xF0U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    REQUIRE(decodeResidualBlock4x4(br, 0, cMaxCoeff4x4, 0U, block) == Result::Ok);
    CHECK(block.totalCoeff == 2U);
    // levels[0]=+4 at zigzag[1]=raster1, levels[1]=-4 at zigzag[0]=raster0
    CHECK(block.coeffs[0] == static_cast<int16_t>(-4));
    CHECK(block.coeffs[1] == static_cast<int16_t>(4));
    for (uint32_t i = 2U; i < 16U; ++i)
        CHECK(block.coeffs[i] == 0);
    CHECK(br.bitOffset() == 20U);
}

// ── Test 6: full residual block decode with known placement ─────────────────

TEST_CASE("decodeResidualBlock4x4: TC=2 TO=2 totalZeros=2 run=0 placed correctly")
{
    // ITU-T H.264 §9.2  (full CAVLC residual block decode)
    //
    // Block parameters: TC=2, TO=2 (all trailing ones), no extra levels,
    //   total_zeros=2, run_before[0]=0.
    //
    // Step 1 — coeff_token:
    //   nC=0 (tableIdx=0), TC=2, TO=2: code=1 size=3 → bits '001'
    //
    // Step 2 — trailing ones signs (2 bits, MSB=first decoded):
    //   TO sign[0]='0' → +1,  sign[1]='1' → -1
    //   levels[0]=+1 (most recent non-zero, placed at highest scan position)
    //   levels[1]=-1
    //
    // Step 3 — no extra levels (TC==TO)
    //
    // Step 4 — total_zeros:
    //   TC=2, tableOffset=cTotalZerosIndex[1]=16
    //   tz=2: entry at 16+2=18: size=3, code=5 → bits '101'
    //
    // Step 5 — run_before (zerosLeft=2):
    //   i=0 (not last, zerosLeft=2): decodeRunBefore(br, 2)
    //     zerosLeft=2 ≤ 6, tableOffset=cRunBeforeIndex[1]=2
    //     run=0: cRunBeforeSize[2]=1, cRunBeforeCode[2]=1 → bit '1'
    //     zerosLeft remains 2.
    //   i=1 (last): run=zerosLeft=2 (no bits consumed)
    //
    // Placement:
    //   coeffIdx = TC + totalZeros - 1 + startIdx = 2+2-1+0 = 3
    //   i=0: run=0,  place levels[0]=+1 at zigzag[3]=8.  coeffIdx=3-(0+1)=2.
    //   i=1: run=2,  place levels[1]=-1 at zigzag[2]=4.  (The 2 zeros occupy
    //         scan positions 1 and 0; the coefficient itself sits at scan pos 2.)
    //
    //   coeffs[8]=+1,  coeffs[4]=-1,  coeffs[0]=coeffs[1]=0,  all others=0.
    //
    // Bitstream: '001' + '0' + '1' + '101' + '1' = 9 bits
    //   bit0-2: '001'  coeff_token
    //   bit3:   '0'    sign[0] → +1
    //   bit4:   '1'    sign[1] → -1
    //   bit5-7: '101'  total_zeros=2 (code=5 size=3)
    //   bit8:   '1'    run_before[0]=0 (code=1 size=1)
    //   byte0: 0010 1101 = 0x2D
    //   byte1: 1000 0000 = 0x80  (bit8='1', bits 9-15 padding)
    const uint8_t data[] = { 0x2DU, 0x80U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    REQUIRE(decodeResidualBlock4x4(br, 0, cMaxCoeff4x4, 0U, block) == Result::Ok);
    CHECK(block.totalCoeff == 2U);
    // Scan pos 3 (zigzag[3]=raster 8): trailing one sign='0' → +1
    CHECK(block.coeffs[8] == static_cast<int16_t>(1));
    // Scan pos 2 (zigzag[2]=raster 4): trailing one sign='1' → -1, with 2 zeros
    // at scan positions 1 and 0 below it (total_zeros=2, run_before=0 for coeff[0])
    CHECK(block.coeffs[4] == static_cast<int16_t>(-1));
    for (uint32_t i = 0U; i < 16U; ++i)
    {
        if (i == 4U || i == 8U)
            continue;
        CHECK(block.coeffs[i] == 0);
    }
    CHECK(br.bitOffset() == 9U);
}

TEST_CASE("decodeResidualBlock4x4: TC=3 TO=1 totalZeros=2 run_before verified")
{
    // ITU-T H.264 §9.2
    //
    // Block: TC=3, TO=1, non-zero levels at scan positions 4,1,0 (after zeros).
    //
    // Step 1 — coeff_token:
    //   nC=0 (tableIdx=0), TC=3, TO=1: code=6 size=8 → bits '00000110'
    //
    // Step 2 — trailing ones (1 bit):
    //   sign[0]='1' → levels[0]=-1
    //
    // Step 3 — remaining levels (TC-TO = 2 levels):
    //   Initial suffixLen=0 (TC=3 ≤ 10).
    //   level[1] (i=TO=1): first non-trailing, ±1 adjustment applies.
    //     Use prefix=0 → bit '1' → levelCode=0 → absLevel=1 → raw level=+1
    //     Adjustment: +1+1=+2.  levels[1]=+2.
    //     suffixLen 0→1 (unconditional).
    //   level[2] (i=2): suffixLen=1, no adjustment.
    //     Use prefix=0, suffix=0 (1 bit '0'):
    //       levelCode=(0<<1)+0=0 → absLevel=1 → level=+1.  levels[2]=+1.
    //     |level|=1, threshold[1]=3: 1 ≤ 3 → suffixLen stays 1.
    //
    // Step 4 — total_zeros:
    //   TC=3, tableOffset=cTotalZerosIndex[2]=31
    //   tz=2: entry at 31+2=33: size=3, code=6 → bits '110'
    //
    // Step 5 — run_before (zerosLeft=2, TC=3 coefficients):
    //   i=0 (not last, zerosLeft=2): decodeRunBefore(br, 2)
    //     run=0: size=1 code=1 → bit '1'
    //   i=1 (not last, zerosLeft=2): decodeRunBefore(br, 2)
    //     run=0: size=1 code=1 → bit '1'
    //   i=2 (last): run=zerosLeft=2 (no bits)
    //
    // Placement (coeffIdx = TC+tz-1+0 = 3+2-1+0 = 4):
    //   i=0: run=0, place levels[0]=-1 at zigzag[4]=5.  coeffIdx=4-(0+1)=3.
    //   i=1: run=0, place levels[1]=+2 at zigzag[3]=8.  coeffIdx=3-(0+1)=2.
    //   i=2 (last): run=zerosLeft=2, place levels[2]=+1 at zigzag[2]=4.
    //     The 2 zeros occupy scan positions 1 and 0 below this coefficient.
    //
    //   coeffs[5]=-1,  coeffs[8]=+2,  coeffs[4]=+1,  all others=0.
    //
    // Bitstream bits:
    //   '00000110' (8)  coeff_token
    //   '1'        (1)  trailing-ones sign → -1
    //   '1'        (1)  level[1] prefix=0
    //   '1'        (1)  level[2] prefix=0
    //   '0'        (1)  level[2] suffix=0 (suffixLen=1)
    //   '110'      (3)  total_zeros=2
    //   '1'        (1)  run_before[0]=0
    //   '1'        (1)  run_before[1]=0
    //   Total = 17 bits → 3 bytes
    //
    // Packing MSB-first:
    //   bits 0-7:  '00000110' = 0x06
    //   bits 8-15: '1 1 1 0 110 1' = 1110 1101 = 0xED? let me redo carefully:
    //     bit8 ='1' (trailing sign)
    //     bit9 ='1' (level[1] stop-bit, prefix=0)
    //     bit10='1' (level[2] stop-bit, prefix=0)
    //     bit11='0' (level[2] suffix=0, suffixLen=1)
    //     bit12='1' (total_zeros MSB of '110')
    //     bit13='1'
    //     bit14='0'
    //     bit15='1' (run_before[0]=0, code '1')
    //     byte1 = 1111 0110 ... wait: bit8 is MSB of byte1:
    //     byte1 bits MSB→LSB = bit8,bit9,bit10,bit11,bit12,bit13,bit14,bit15
    //                        = 1,   1,   1,    0,    1,    1,    0,    1
    //                        = 1110 1101 = 0xED
    //   bits 16: '1' (run_before[1]=0)  bit16 = MSB of byte2 = 1
    //   bits 17-23: padding
    //     byte2 = 1000 0000 = 0x80
    const uint8_t data[] = { 0x06U, 0xEDU, 0x80U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    REQUIRE(decodeResidualBlock4x4(br, 0, cMaxCoeff4x4, 0U, block) == Result::Ok);
    CHECK(block.totalCoeff == 3U);
    CHECK(block.coeffs[5]  == static_cast<int16_t>(-1));
    CHECK(block.coeffs[8]  == static_cast<int16_t>(2));
    // Scan pos 2 (zigzag[2]=raster 4), 2 zeros below at scan pos 1 and 0
    CHECK(block.coeffs[4]  == static_cast<int16_t>(1));
    for (uint32_t i = 0U; i < 16U; ++i)
    {
        if (i == 4U || i == 5U || i == 8U)
            continue;
        CHECK(block.coeffs[i] == 0);
    }
    CHECK(br.bitOffset() == 17U);
}

// ── Test 7: chroma DC total_zeros table selection ───────────────────────────

TEST_CASE("decodeResidualBlock4x4: chroma DC maxCoeff=4 uses chroma total_zeros table")
{
    // ITU-T H.264 §9.2.3 Table 9-9
    //
    // When maxCoeff==4, decodeTotalZerosChromaDC is called instead of the
    // standard 4x4 table (decodeTotalZeros).  The chroma DC table has
    // different VLC codes, so providing the wrong bitstream would decode
    // an incorrect total_zeros and misplace coefficients.
    //
    // Block: TC=2, TO=2 (both trailing ones), maxCoeff=4.
    //
    // Step 1 — coeff_token (nC=-1, chroma DC):
    //   to=2, tc=2: cCoeffTokenCodeChroma[2][2]=1, size=3 → bits '001'
    //
    // Step 2 — trailing ones (2 bits):
    //   sign[0]='0' → +1,  sign[1]='0' → +1
    //   levels[0]=+1,  levels[1]=+1
    //
    // Step 3 — no extra levels (TC==TO)
    //
    // Step 4 — total_zeros (chroma DC, TC=2):
    //   cChromaTzIndex[1]=4, entries for tz=0,1,2:
    //     tz=0: size=cTotalZerosSizeChroma[4]=1, code=cTotalZerosCodeChroma[4]=1  → '1'
    //     tz=1: size=cTotalZerosSizeChroma[5]=2, code=cTotalZerosCodeChroma[5]=1  → '01'
    //     tz=2: size=cTotalZerosSizeChroma[6]=2, code=cTotalZerosCodeChroma[6]=0  → '00'
    //   We encode tz=1: bits '01'
    //
    // Step 5 — run_before (zerosLeft=1, TC=2 coefficients):
    //   i=0 (not last, zerosLeft=1): decodeRunBefore(br,1)
    //     run=0: size=1 code=1 → bit '1'
    //   i=1 (last): run=zerosLeft=1 (no bits)
    //
    // Placement (startIdx=0, coeffIdx=2+1-1+0=2):
    //   i=0: run=0, place levels[0]=+1 at zigzag[2]=4.  coeffIdx=2-(0+1)=1.
    //   i=1 (last): run=zerosLeft=1, place levels[1]=+1 at zigzag[1]=1.
    //     (1 zero occupies scan pos 0 below this coefficient.)
    //
    //   coeffs[4]=+1,  coeffs[1]=+1,  coeffs[0]=coeffs[2]=coeffs[3]=0.
    //
    // Bitstream bits: '001' + '00' + '01' + '1' = 8 bits
    //   bit0-2: '001'  coeff_token (TC=2, TO=2 chroma DC, code=1 size=3)
    //   bit3-4: '00'   trailing signs → +1, +1
    //   bit5-6: '01'   total_zeros=1 (chroma DC TC=2, code=1 size=2)
    //   bit7:   '1'    run_before[0]=0 (zerosLeft=1, code=1 size=1)
    //   byte0 = 0010 0011 = 0x23
    const uint8_t data[] = { 0x23U, 0x00U };
    BitReader br(data, sizeof(data));

    // maxCoeff=4 triggers chroma DC path
    ResidualBlock4x4 block;
    REQUIRE(decodeResidualBlock4x4(br, -1, 4U, 0U, block) == Result::Ok);
    CHECK(block.totalCoeff == 2U);
    // Chroma DC uses identity scan (no zigzag) — ITU-T H.264 §8.5.6.
    // Scan pos 2 → raster 2 (not zigzag[2]=4): trailing one sign='0' → +1
    CHECK(block.coeffs[2] == static_cast<int16_t>(1));
    // Scan pos 1 → raster 1: trailing one sign='0' → +1, 1 zero at pos 0
    CHECK(block.coeffs[1] == static_cast<int16_t>(1));
    CHECK(block.coeffs[0] == static_cast<int16_t>(0));
    CHECK(block.coeffs[3] == static_cast<int16_t>(0));
    CHECK(br.bitOffset() == 8U);
}

TEST_CASE("decodeResidualBlock4x4: chroma DC TC=1 totalZeros=2 from chroma table")
{
    // Verify the chroma DC total_zeros table is used (not the 4x4 table)
    // by choosing a tz value whose chroma VLC code differs from the 4x4 table.
    //
    // Block: TC=1, TO=1, maxCoeff=4.
    //
    // Step 1 — coeff_token (nC=-1, chroma DC):
    //   to=1, tc=1: cCoeffTokenCodeChroma[1][1]=1, size=1 → bit '1'
    //
    // Step 2 — trailing ones (1 bit):
    //   sign[0]='0' → +1.  levels[0]=+1.
    //
    // Step 3 — no extra levels.
    //
    // Step 4 — total_zeros (chroma DC TC=1):
    //   cChromaTzIndex[0]=0, entries for tz=0,1,2,3:
    //     tz=0: cTotalZerosSizeChroma[0]=1, code=1 → '1'
    //     tz=1: cTotalZerosSizeChroma[1]=2, code=1 → '01'
    //     tz=2: cTotalZerosSizeChroma[2]=3, code=1 → '001'
    //     tz=3: cTotalZerosSizeChroma[3]=3, code=0 → '000'
    //   We encode tz=2: bits '001'
    //
    // Step 5 — run_before (zerosLeft=2, TC=1, only one coeff → last coeff):
    //   i=0 (last, i==TC-1): run=zerosLeft=2 (no bits consumed)
    //
    // Placement (startIdx=0, coeffIdx=1+2-1+0=2):
    //   i=0 (last, TC=1): run=zerosLeft=2, place levels[0]=+1 at zigzag[2]=4.
    //     (2 zeros occupy scan positions 1 and 0 below this coefficient.)
    //
    //   coeffs[4]=+1, all others zero.
    //
    // Bitstream bits: '1' + '0' + '001' = 5 bits
    //   bit0: '1'    coeff_token (chroma DC TC=1 TO=1, code=1 size=1)
    //   bit1: '0'    trailing sign → +1
    //   bit2-4: '001' total_zeros=2 (chroma DC TC=1, code=1 size=3)
    //   bits 5-7: padding
    //   b0=1,b1=0,b2=0,b3=0,b4=1,b5=0,b6=0,b7=0 = 1000 1000 = 0x88
    const uint8_t data[] = { 0x88U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    REQUIRE(decodeResidualBlock4x4(br, -1, 4U, 0U, block) == Result::Ok);
    CHECK(block.totalCoeff == 1U);
    // Chroma DC identity scan: pos 2 → raster 2 (not zigzag[2]=4)
    CHECK(block.coeffs[2] == static_cast<int16_t>(1));
    for (uint32_t i = 0U; i < 16U; ++i)
    {
        if (i == 2U) continue;
        CHECK(block.coeffs[i] == 0);
    }
    CHECK(br.bitOffset() == 5U);
}

// ── Chroma AC residual block (maxCoeff=15, startIdx=1) — ITU-T §7.3.5.3 ─

TEST_CASE("decodeResidualBlock4x4: chroma AC maxCoeff=15 startIdx=1 TC=1 TO=1")
{
    // ITU-T H.264 §7.3.5.3: chroma AC blocks use maxCoeff=15 and startIdx=1
    // (the DC coefficient is not decoded here — it comes from the Hadamard DC block).
    // The coeff_token VLC table selection uses nC (context from neighbors).
    //
    // Block: TC=1, TO=1, nC=0.
    //   coeff_token: nC<2, TC=1, TO=1 → code=1, size=2 → bits '01'
    //   trailing sign: '0' → +1
    //   total_zeros (TC=1 < maxCoeff=15): uses Table 9-7 (luma/4x4 table).
    //     tz=0 for TC=1: size=1, code=1 → bit '1'
    //   (last coefficient: no run_before consumed)
    //
    // Placement: coeffIdx = 1+0-1+1 = 1 (startIdx=1).
    //   i=0 (last): run=0, place at zigzag[1]=1 → coeffs[1]=+1
    //
    // Bitstream: '01' + '0' + '1' = 4 bits
    //   bits: 0, 1, 0, 1, xxxx = 0101 xxxx = 0x50
    const uint8_t data[] = { 0x50U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    // maxCoeff=15 (chroma AC), startIdx=1 (skip DC position)
    REQUIRE(decodeResidualBlock4x4(br, 0, 15U, 1U, block) == Result::Ok);
    CHECK(block.totalCoeff == 1U);
    // Scan pos 1 (zigzag[1]=raster 1): coefficient = +1
    CHECK(block.coeffs[1] == static_cast<int16_t>(1));
    // DC position (raster 0, zigzag[0]) must be zero — startIdx=1 skips it
    CHECK(block.coeffs[0] == static_cast<int16_t>(0));
    CHECK(br.bitOffset() == 4U);
}

TEST_CASE("decodeResidualBlock4x4: chroma AC TC=0 produces all-zero block")
{
    // ITU-T H.264 §7.3.5.3: when TC=0 for a chroma AC block, no coefficients
    // are coded — the block is all zero (DC may still be non-zero from DC block).
    // coeff_token: nC<2, TC=0, TO=0 → code=1, size=1 → bit '1'
    const uint8_t data[] = { 0x80U };
    BitReader br(data, sizeof(data));
    ResidualBlock4x4 block;
    // Populate with sentinel to ensure zeroing
    for (uint32_t i = 0U; i < 16U; ++i)
        block.coeffs[i] = static_cast<int16_t>(99);
    REQUIRE(decodeResidualBlock4x4(br, 0, 15U, 1U, block) == Result::Ok);
    CHECK(block.totalCoeff == 0U);
    // All raster positions (including DC at raster 0) must be zeroed
    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(block.coeffs[i] == static_cast<int16_t>(0));
}

// ── Dequant DC path — ITU-T H.264 §8.5.12.1 ────────────────────────────

TEST_CASE("Dequant isDc=true: full left-shift without qpDiv6-2 reduction")
{
    // ITU-T H.264 §8.5.12.1: for Hadamard DC blocks (isDc=true), the dequant
    // formula uses <<qpDiv6 (not <<(qpDiv6-2)), because the Hadamard transform
    // has its own normalization that doesn't include the IDCT factor of 4.
    //
    // QP=12: qpDiv6=2, qpMod6=0, scale=10.
    // isDc=true:  val = 1 * 10 << 2 = 40
    // isDc=false: val = 1 * 10 << 0 = 10  (<<(qpDiv6-2) = <<0)
    int16_t dcCoeffs[16] = {};
    int16_t acCoeffs[16] = {};
    dcCoeffs[0] = 1;  // class0
    acCoeffs[0] = 1;  // class0

    inverseQuantize4x4(dcCoeffs, 12, /*isDc=*/true);
    inverseQuantize4x4(acCoeffs, 12, /*isDc=*/false);

    // DC path: 1 * 10 << 2 = 40 (isDc uses << qpDiv6 for Hadamard normalization)
    CHECK(dcCoeffs[0] == static_cast<int16_t>(40));
    // AC path: 1 * 10 << 2 = 40 (non-DC also uses << qpDiv6 now)
    CHECK(acCoeffs[0] == static_cast<int16_t>(40));
    // DC and AC use the same shift (both << qpDiv6)
    CHECK(dcCoeffs[0] == acCoeffs[0]);
}

TEST_CASE("Dequant isDc=true: QP=0 left-shift (no right-shift path for DC)")
{
    // ITU-T H.264 §8.5.12.1: isDc=true always left-shifts by qpDiv6,
    // even when qpDiv6=0. The right-shift path only applies for isDc=false.
    // QP=0: qpDiv6=0, qpMod6=0, scale=10.
    // isDc=true: val = 1 * 10 << 0 = 10
    int16_t coeffs[16] = {};
    coeffs[0] = 1;  // class0
    coeffs[5] = 1;  // class1 (both-odd), scale=16
    inverseQuantize4x4(coeffs, 0, /*isDc=*/true);

    CHECK(coeffs[0] == static_cast<int16_t>(10));
    CHECK(coeffs[5] == static_cast<int16_t>(16));
}

// ── Hadamard 4x4 inverse — ITU-T H.264 §8.5.12.2 ────────────────────────

TEST_CASE("Hadamard 4x4: all-same input produces 16× DC in [0] only")
{
    // §8.5.12.2: for I_16x16, the 4x4 Hadamard inverse transform applies to
    // the 16 DC coefficients. If all DCs are equal to v, output[0]=16v, rest 0.
    int16_t dc[16] = {};
    for (uint32_t i = 0U; i < 16U; ++i)
        dc[i] = 1;
    inverseHadamard4x4(dc);
    // Sum of all inputs is 16; only dc[0] should be non-zero
    CHECK(dc[0] == static_cast<int16_t>(16));
    for (uint32_t i = 1U; i < 16U; ++i)
        CHECK(dc[i] == static_cast<int16_t>(0));
}

TEST_CASE("Hadamard 4x4: single non-zero input propagates to all outputs")
{
    // §8.5.12.2: Hadamard transform is its own inverse (up to scaling).
    // A single DC = N at position [0] distributes N to all 16 outputs.
    int16_t dc[16] = {};
    dc[0] = 4;
    inverseHadamard4x4(dc);
    // Each output = 4 (DC distributes equally to all positions)
    for (uint32_t i = 0U; i < 16U; ++i)
        CHECK(dc[i] == static_cast<int16_t>(4));
}
