/** Sub0h264 — CAVLC entropy decoder
 *
 *  Context-Adaptive Variable-Length Coding for H.264 Baseline profile.
 *  Decodes macroblock syntax elements: mb_type, prediction modes,
 *  motion vectors, CBP, QP delta, and residual coefficients.
 *
 *  Reference: ITU-T H.264 §9.2
 *
 *  Spec-annotated review (2026-04-09):
 *    §9.2 decode step sequence verified: [CHECKED §9.2]
 *      Step 1: coeff_token → (TotalCoeff, TrailingOnes)  [CHECKED §9.2.1]
 *      Step 2: trailing_ones_sign_flag (sign: 0→+1, 1→-1) [CHECKED §9.2.2]
 *      Step 3: levels (reverse scan order, suffixLen adaptation) [CHECKED §9.2.2]
 *      Step 4: total_zeros (gated on TotalCoeff < maxNumCoeff)  [CHECKED §9.2.3]
 *      Step 5: run_before (not decoded for last coeff / zerosLeft==0) [CHECKED §9.2.3]
 *      Step 6: position reconstruction from runs [CHECKED §9.2.4]
 *
 *  Validation status:
 *    coeff_token:    Tables 9-5(a)-(e) regenerated from libavc, verified
 *                    against spec. nC>=8 fixed-code decode confirmed correct.
 *                    nC ranges: <0=chromaDC, 0-1, 2-3, 4-7, >=8. [CHECKED §9.2.1]
 *    level decode:   §9.2.2 suffixLen adaptation uses two INDEPENDENT if
 *                    statements (not if/else-if). [CHECKED §9.2.2]
 *                    First non-trailing level ±1 offset when T1<3. [CHECKED §9.2.2]
 *                    suffixLen init: tc>10&&t1<3→1, else→0. [CHECKED §9.2.2]
 *    total_zeros:    Table 9-7 verified prefix-free at compile time
 *                    (static_assert in test_spec_tables.cpp). [CHECKED §9.2.3]
 *    run_before:     Table 9-10 verified prefix-free at compile time. [CHECKED §9.2.3]
 *    chroma DC:      Zigzag identity for maxCoeff<=4. [CHECKED §8.5.6]
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_CAVLC_HPP
#define CROG_SUB0H264_CAVLC_HPP

#include "bitstream.hpp"
#include "cavlc_tables.hpp"
#include "decode_trace.hpp"
#include "tables.hpp"

#include "sub0h264/sub0h264_types.hpp"

#include <cstdint>
#include <cstring>
#include <array>
#include <algorithm>

namespace sub0h264 {

/// Maximum coefficients in a 4x4 block.
inline constexpr uint32_t cMaxCoeff4x4 = 16U;

/// Maximum trailing ones — ITU-T H.264 §9.2.1.
inline constexpr uint32_t cMaxTrailingOnes = 3U;

/// Maximum suffix length for level decoding — ITU-T H.264 §9.2.2.
inline constexpr uint32_t cMaxSuffixLength = 6U;

// ── Coeff token decoding — ITU-T H.264 §9.2.1 ─────────────────────────

/** Result of coeff_token VLC decoding. */
struct CoeffToken
{
    uint8_t totalCoeff;    ///< Number of non-zero coefficients [0-16]
    uint8_t trailingOnes;  ///< Number of trailing ±1 coefficients [0-3]
};

/** Match a VLC code against the bitstream.
 *  Tries all (trailingOnes, totalCoeff) combinations for a given nC range.
 *  Returns the match with the shortest code that matches the peeked bits.
 */
inline CoeffToken matchCoeffTokenTable(BitReader& br, uint32_t tableIdx) noexcept
{
    // Peek enough bits for the longest possible code (16 bits)
    uint32_t peekBuf = br.peekBits(16U);

    // Search all (trailingOnes, totalCoeff) combinations, preferring shortest match
    CoeffToken best = { 0U, 0U };
    uint8_t bestSize = 255U;

    for (uint32_t to = 0U; to < 4U; ++to)
    {
        for (uint32_t tc = 0U; tc <= 16U; ++tc)
        {
            // Skip invalid combinations: trailing ones can't exceed total coefficients
            if (to > tc)
                continue;

            uint8_t codeSize = cCoeffTokenSize[tableIdx][to][tc];
            if (codeSize == 0U || codeSize > 16U)
                continue;

            uint16_t codeVal = cCoeffTokenCode[tableIdx][to][tc];

            // Extract top codeSize bits from peek buffer and compare
            uint32_t mask = (1U << codeSize) - 1U;
            uint32_t peeked = (peekBuf >> (16U - codeSize)) & mask;

            if (peeked == codeVal && codeSize < bestSize)
            {
                best.totalCoeff = static_cast<uint8_t>(tc);
                best.trailingOnes = static_cast<uint8_t>(to);
                bestSize = codeSize;
            }
        }
    }

    br.skipBits(bestSize);
    return best;
}

/** Decode coeff_token from bitstream using context nC.
 *
 *  Uses full ITU-T H.264 Table 9-5 VLC lookup tables for spec-compliant decoding.
 *  nC (number of coefficients context) is derived from neighboring blocks.
 *
 *  @param br   Bitstream reader
 *  @param nC   Context value [0-16, or -1 for chroma DC]
 *  @return Decoded coeff_token
 *
 *  Reference: ITU-T H.264 §9.2.1, Tables 9-5(a-e)
 */
inline CoeffToken decodeCoeffToken(BitReader& br, int32_t nC) noexcept
{
    if (nC < 0)
    {
        // Chroma DC: Table 9-5(d) — max 4 coefficients
        uint32_t peekBuf = br.peekBits(8U);
        CoeffToken best = { 0U, 0U };
        uint8_t bestSize = 255U;

        for (uint32_t to = 0U; to < 4U; ++to)
        {
            for (uint32_t tc = 0U; tc <= 4U; ++tc)
            {
                if (to > tc)
                    continue;
                uint8_t codeSize = cCoeffTokenSizeChroma[to][tc];
                if (codeSize == 0U)
                    continue;
                uint8_t codeVal = cCoeffTokenCodeChroma[to][tc];
                uint32_t peeked = (peekBuf >> (8U - codeSize)) & ((1U << codeSize) - 1U);
                if (peeked == codeVal && codeSize < bestSize)
                {
                    best.totalCoeff = static_cast<uint8_t>(tc);
                    best.trailingOnes = static_cast<uint8_t>(to);
                    bestSize = codeSize;
                }
            }
        }
        br.skipBits(bestSize);
        return best;
    }

    if (nC >= 8)
    {
        // Table 9-5(e): fixed 6-bit code for nC >= 8.
        // ITU-T H.264 Table 9-5(e): code 000011 → (tc=0, t1=0).
        // All other codes: tc = (code >> 2) + 1, t1 = code & 3.
        // Reference: libavc ih264d_cavlc_parse4x4coeff_n8(), line 1305.
        uint32_t code = br.readBits(6U);
        CoeffToken ct;
        /// Special code: 000011 (=3) maps to tc=0 — ITU-T H.264 Table 9-5(e).
        static constexpr uint32_t cNc8ZeroCode = 3U;
        if (code == cNc8ZeroCode)
        {
            ct.totalCoeff = 0U;
            ct.trailingOnes = 0U;
        }
        else
        {
            ct.totalCoeff = static_cast<uint8_t>((code >> 2U) + 1U);
            ct.trailingOnes = static_cast<uint8_t>(code & 3U);
            if (ct.trailingOnes > ct.totalCoeff)
                ct.trailingOnes = ct.totalCoeff;
        }
        return ct;
    }

    // Tables 9-5(a), (b), (c): select table by nC range
    uint32_t tableIdx;
    if (nC < 2)       tableIdx = 0U;
    else if (nC < 4)  tableIdx = 1U;
    else              tableIdx = 2U;  // 4 <= nC < 8

    return matchCoeffTokenTable(br, tableIdx);
}

// ── Level decoding — ITU-T H.264 §9.2.2 ────────────────────────────────

/** Decode one coefficient level value from the bitstream.
 *
 *  Reads level_prefix (leading zeros + 1) and level_suffix, computes
 *  levelCode per ITU-T H.264 §9.2.2, converts to signed level.
 *
 *  NOTE: suffixLen is NOT updated here — the caller must update it
 *  after applying the ±1 trailing-ones adjustment, so the adaptation
 *  threshold uses the correct final magnitude.
 *
 *  @param br         Bitstream reader
 *  @param suffixLen  Current suffix length [0-6] (read-only)
 *  @return Decoded signed level value (before trailing-ones adjustment)
 */
inline int32_t decodeLevel(BitReader& br, uint32_t suffixLen) noexcept
{
    /// Count leading zeros → level_prefix — ITU-T H.264 §9.2.2.1, Table 9-6.
    /// Use CLZ on peeked bits to avoid per-bit loop.
    uint32_t remaining = br.totalBits() - br.bitOffset();
    uint32_t peekN = (remaining < 32U) ? remaining : 32U;
    uint32_t bits = br.peekBits(peekN);
    uint32_t prefix;
    if (bits == 0U)
    {
        prefix = peekN;
        br.skipBits(peekN);
    }
    else
    {
        bits <<= (32U - peekN); // left-align for CLZ
#if defined(__GNUC__) || defined(__clang__)
        prefix = static_cast<uint32_t>(__builtin_clz(bits));
#elif defined(_MSC_VER)
        unsigned long idx;
        _BitScanReverse(&idx, bits);
        prefix = 31U - idx;
#else
        prefix = 0U;
        uint32_t tmp = bits;
        while ((tmp & 0x80000000U) == 0U) { ++prefix; tmp <<= 1U; }
#endif
        br.skipBits(prefix + 1U); // skip zeros + the '1' bit
    }

    /// Compute levelSuffixSize — ITU-T H.264 §9.2.2.
    ///   Normal:            levelSuffixSize = suffixLength
    ///   prefix==14, sL==0: levelSuffixSize = 4
    ///   prefix>=15:        levelSuffixSize = prefix - 3
    uint32_t suffixSize;
    if (prefix == 14U && suffixLen == 0U)
        suffixSize = 4U;
    else if (prefix >= 15U)
        suffixSize = prefix - 3U;
    else
        suffixSize = suffixLen;

    /// Read level_suffix (suffixSize bits).
    uint32_t suffix = 0U;
    if (suffixSize > 0U)
        suffix = br.readBits(suffixSize);

    /// Compute levelCode — ITU-T H.264 §9.2.2.
    /// levelCode = (min(15, level_prefix) << suffixLength) + level_suffix
    int32_t levelCode = static_cast<int32_t>(
        (static_cast<uint32_t>(prefix < 15U ? prefix : 15U) << suffixLen) + suffix);

    if (prefix >= 15U && suffixLen == 0U)
        levelCode += 15;
    if (prefix >= 16U)
        levelCode += static_cast<int32_t>((1U << (prefix - 3U)) - 4096U);

    /// Convert to signed — ITU-T H.264 §9.2.2.
    /// levelCode even → positive, odd → negative.
    int32_t absLevel = (levelCode + 2) >> 1;
    int32_t sign = (levelCode & 1) ? -1 : 1;
    return absLevel * sign;
}

// ── Total zeros decoding — ITU-T H.264 §9.2.3 ──────────────────────────

/** Decode total_zeros for 4x4 block using spec VLC tables.
 *  @param br          Bitstream reader
 *  @param totalCoeff  Number of non-zero coefficients [1-15]
 *  @return Total number of zero coefficients before the last non-zero
 *
 *  Reference: ITU-T H.264 §9.2.3, Tables 9-7/9-8
 */
inline uint32_t decodeTotalZeros(BitReader& br, uint32_t totalCoeff) noexcept
{
    if (totalCoeff == 0U || totalCoeff >= 16U)
        return 0U;

    uint32_t tableOffset = cTotalZerosIndex[totalCoeff - 1U];
    uint32_t tableLen = (totalCoeff < 15U)
        ? (cTotalZerosIndex[totalCoeff] - tableOffset)
        : (135U - tableOffset);

    uint32_t peekBuf = br.peekBits(9U); // Max total_zeros VLC is 9 bits

    // Search for matching VLC code
    for (uint32_t tzVal = 0U; tzVal < tableLen; ++tzVal)
    {
        uint8_t codeSize = cTotalZerosSize[tableOffset + tzVal];
        uint8_t codeVal  = cTotalZerosCode[tableOffset + tzVal];

        if (codeSize == 0U || codeSize > 9U)
            continue;

        uint32_t peeked = (peekBuf >> (9U - codeSize)) & ((1U << codeSize) - 1U);
        if (peeked == codeVal)
        {
            br.skipBits(codeSize);
            return tzVal;
        }
    }

    // Fallback: consume 1 bit, return 0
    br.skipBits(1U);
    return 0U;
}

/** Decode total_zeros for chroma DC 2x2 block using Table 9-9.
 *  @param br          Bitstream reader
 *  @param totalCoeff  Number of non-zero coefficients [1-3]
 *  @return Total number of zero coefficients
 *
 *  Reference: ITU-T H.264 §9.2.3, Table 9-9
 */
inline uint32_t decodeTotalZerosChromaDC(BitReader& br, uint32_t totalCoeff) noexcept
{
    if (totalCoeff == 0U || totalCoeff > 3U)
        return 0U;

    /// Chroma DC total_zeros index offsets by totalCoeff (1-based).
    static constexpr uint8_t cChromaTzIndex[3] = { 0, 4, 7 };

    uint32_t maxZeros = 4U - totalCoeff;
    uint32_t tableOffset = cChromaTzIndex[totalCoeff - 1U];
    uint32_t tableLen = ((totalCoeff < 3U)
        ? cChromaTzIndex[totalCoeff] : 9U) - tableOffset;

    uint32_t peekBuf = br.peekBits(3U);

    for (uint32_t tzVal = 0U; tzVal < tableLen; ++tzVal)
    {
        uint8_t codeSize = cTotalZerosSizeChroma[tableOffset + tzVal];
        uint8_t codeVal  = cTotalZerosCodeChroma[tableOffset + tzVal];

        if (codeSize == 0U || codeSize > 3U)
            continue;

        uint32_t peeked = (peekBuf >> (3U - codeSize)) & ((1U << codeSize) - 1U);
        if (peeked == codeVal)
        {
            br.skipBits(codeSize);
            return (tzVal < maxZeros) ? tzVal : maxZeros;
        }
    }

    br.skipBits(1U);
    return 0U;
}

// ── Run before decoding — ITU-T H.264 §9.2.3 ───────────────────────────

/** Decode run_before (zeros before a coefficient) using spec VLC tables.
 *  @param br         Bitstream reader
 *  @param zerosLeft  Remaining zeros to distribute
 *  @return Run length before this coefficient
 *
 *  Reference: ITU-T H.264 §9.2.3, Table 9-10
 */
inline uint32_t decodeRunBefore(BitReader& br, uint32_t zerosLeft) noexcept
{
    if (zerosLeft == 0U)
        return 0U;

    if (zerosLeft <= 6U)
    {
        uint32_t tableOffset = cRunBeforeIndex[zerosLeft - 1U];
        uint32_t tableLen = (zerosLeft < 6U)
            ? (cRunBeforeIndex[zerosLeft] - tableOffset)
            : (27U - tableOffset);

        uint32_t peekBuf = br.peekBits(3U);

        for (uint32_t runVal = 0U; runVal < tableLen; ++runVal)
        {
            uint8_t codeSize = cRunBeforeSize[tableOffset + runVal];
            uint8_t codeVal  = cRunBeforeCode[tableOffset + runVal];

            uint32_t peeked = (peekBuf >> (3U - codeSize)) & ((1U << codeSize) - 1U);
            if (peeked == codeVal)
            {
                br.skipBits(codeSize);
                return runVal;
            }
        }
        br.skipBits(1U);
        return 0U;
    }

    // zerosLeft > 6: Table 9-10 row 7+ uses prefix coding
    // VLC: 0..6 have 3-bit codes, 7+ use leading-zeros prefix
    uint32_t tableOffset = cRunBeforeIndex[6U]; // zerosLeft=7 table
    uint32_t peekBuf = br.peekBits(11U);

    for (uint32_t runVal = 0U; runVal < 15U && runVal <= zerosLeft; ++runVal)
    {
        uint32_t idx = tableOffset + runVal;
        if (idx >= 42U) break;

        uint8_t codeSize = cRunBeforeSize[idx];
        uint8_t codeVal  = cRunBeforeCode[idx];

        if (codeSize > 11U) break;

        uint32_t peeked = (peekBuf >> (11U - codeSize)) & ((1U << codeSize) - 1U);
        if (peeked == codeVal)
        {
            br.skipBits(codeSize);
            return runVal;
        }
    }

    br.skipBits(1U);
    return 0U;
}

// ── 4x4 residual block decoder — ITU-T H.264 §9.2 ─────────────────────

/// Decoded residual coefficients for one 4x4 block.
struct ResidualBlock4x4
{
    int16_t coeffs[16]{};    ///< Coefficients in raster order
    uint8_t totalCoeff = 0U; ///< Non-zero coefficient count (for neighbor context)
};

/** Decode a 4x4 residual block using CAVLC.
 *
 *  @param br         Bitstream reader
 *  @param nC         Context from neighboring blocks
 *  @param maxCoeff   Maximum coefficients (16 for luma, 15 for AC-only)
 *  @param startIdx   Starting scan index (0 for DC+AC, 1 for AC-only)
 *  @param[out] block Decoded coefficients in raster order
 *  @return Result::Ok on success
 */
/** Decode one 4x4 residual block using CAVLC.
 *
 *  @param trace  Optional DecodeTrace pointer for entropy event emission
 *                (SUB0H264_TRACE builds). Pass nullptr (default) to skip.
 */
inline Result decodeResidualBlock4x4(BitReader& br, int32_t nC,
                                      uint32_t maxCoeff, uint32_t startIdx,
                                      ResidualBlock4x4& block,
                                      [[maybe_unused]] const DecodeTrace* trace = nullptr) noexcept
{
    // §9.2 Step 2: coeff_token → TotalCoeff, TrailingOnes [CHECKED §9.2.1]
    // ~60% of blocks in typical P-frames have totalCoeff==0, so deferring
    // the 32-byte memset saves significant work on in-order cores.
    CoeffToken ct = decodeCoeffToken(br, nC);
#if SUB0H264_TRACE
    if (trace && trace->hasCallback())
        trace->onCavlcCoeffToken(ct.totalCoeff, ct.trailingOnes, nC);
#endif
    block.totalCoeff = ct.totalCoeff;

    if (ct.totalCoeff == 0U)
    {
        std::memset(block.coeffs, 0, sizeof(block.coeffs));
        return Result::Ok;
    }

    // Zero coeffs before scatter-writing non-zero positions via zigzag.
    std::memset(block.coeffs, 0, sizeof(block.coeffs));

    // §9.2 Step 3a: trailing_ones_sign_flag — sign: 0→+1, 1→-1 [CHECKED §9.2.2]
    // FM-2: bit=0 is POSITIVE (inverted from intuition). Verified. [CHECKED FM-2]
    int16_t levels[16];
    uint32_t levelIdx = 0U;

    for (uint32_t i = 0U; i < ct.trailingOnes && i < ct.totalCoeff; ++i)
    {
        uint32_t sign = br.readBit();
        levels[levelIdx++] = sign ? -1 : 1;
    }

#if SUB0H264_TRACE
    // Dump levels array after all levels are decoded (at the end of function)
    // This trace is placed early but the actual dump happens after placement.
#endif

    // §9.2 Step 3a: remaining levels (reverse scan order) [CHECKED §9.2.2]
    // suffixLen init: tc>10 && t1<3 → 1, else → 0 [CHECKED §9.2.2]
    uint32_t suffixLen = 0U;
    if (ct.totalCoeff > 10U && ct.trailingOnes < cMaxTrailingOnes)
        suffixLen = 1U;

    for (uint32_t i = ct.trailingOnes; i < ct.totalCoeff; ++i)
    {
        int32_t level = decodeLevel(br, suffixLen);

        /// First non-trailing level has ±1 offset when trailingOnes < 3
        /// — ITU-T H.264 §9.2.2. Applied BEFORE suffixLength adaptation
        /// so the threshold comparison uses the correct final magnitude.
        if (i == ct.trailingOnes && ct.trailingOnes < cMaxTrailingOnes)
        {
            level += (level > 0) ? 1 : -1;
        }

        levels[levelIdx++] = static_cast<int16_t>(level);
#if SUB0H264_TRACE
        if (trace && trace->hasCallback())
            trace->onCavlcLevel(levelIdx - 1U, level);
#endif

        /// Update suffixLength — ITU-T H.264 §9.2.2.
        /// Step 1: if suffixLength is 0, unconditionally set to 1.
        /// Step 2: if |level| exceeds threshold for CURRENT suffixLength,
        ///         increment suffixLength.
        /// These steps are INDEPENDENT (not mutually exclusive).
        /// Reference: libavc ih264d_rest_of_residual_cav_chroma_dc_block
        ///   u4_suffix_len = (u2_abs_value > 3) ? 2 : 1;  (first level)
        ///   u4_suffix_len += (u2_abs_value > (3 << (u4_suffix_len - 1)));
        uint32_t absVal = static_cast<uint32_t>(std::abs(level));
        if (suffixLen == 0U)
            suffixLen = 1U;
        if (suffixLen < cMaxSuffixLength && absVal > cLevelSuffixThreshold[suffixLen])
            ++suffixLen;
    }

    // §9.2 Step 3b: total_zeros — ONLY if TotalCoeff < maxNumCoeff [CHECKED §9.2.3]
    // FM-3: if TotalCoeff == maxNumCoeff, do NOT read VLC. [CHECKED FM-3]
    uint32_t totalZeros = 0U;
    if (ct.totalCoeff < maxCoeff)
    {
        /// Chroma DC blocks (maxCoeff=4) use a separate total_zeros table
        /// — ITU-T H.264 Table 9-9 (2x2 block, 3 sub-tables for TC 1-3).
        static constexpr uint32_t cChromaDcMaxCoeff = 4U;
#if SUB0H264_TRACE
        uint32_t tzBitBefore = br.bitOffset();
#endif
        if (maxCoeff == cChromaDcMaxCoeff)
            totalZeros = decodeTotalZerosChromaDC(br, ct.totalCoeff);
        else
        {
            totalZeros = decodeTotalZeros(br, ct.totalCoeff);
            // Clamp to actual block size — §9.2.3: total_zeros <= maxNumCoeff - totalCoeff.
            // Table 9-7 entries go up to 16-tc, but chroma AC has maxCoeff=15.
            uint32_t maxTz = maxCoeff - ct.totalCoeff;
            if (totalZeros > maxTz)
                totalZeros = maxTz;
        }
#if SUB0H264_TRACE
        if (trace && trace->hasCallback())
            trace->onCavlcTotalZeros(totalZeros);
#endif
#if SUB0H264_TRACE
        // Trace total_zeros for all chroma AC blocks (maxCoeff=15) with tc 1..14
        if (maxCoeff == 15U && ct.totalCoeff >= 1U)
            std::printf("[CAVLC-TZ] tc=%u maxC=15 totalZeros=%u tzBits=%lu bitOff=%lu\n",
                ct.totalCoeff, totalZeros,
                (unsigned long)(br.bitOffset()-tzBitBefore),
                (unsigned long)br.bitOffset());
#endif
    }

    // §9.2 Step 3b: run_before + §9.2.4 position reconstruction [CHECKED §9.2.3/§9.2.4]
    // FM-3: last coeff (i==tc-1) → run = zerosLeft, do NOT read VLC. [CHECKED FM-3]
    // FM-3: zerosLeft==0 → do NOT read VLC. [CHECKED FM-3]
    uint32_t zerosLeft = totalZeros;
    uint32_t coeffIdx = ct.totalCoeff + totalZeros - 1U + startIdx;

    for (uint32_t i = 0U; i < ct.totalCoeff; ++i)
    {
        uint32_t run = 0U;
        if (zerosLeft > 0U && i < ct.totalCoeff - 1U)
        {
            run = decodeRunBefore(br, zerosLeft);
            zerosLeft -= run;
#if SUB0H264_TRACE
            if (trace && trace->hasCallback())
                trace->onCavlcRunBefore(i, run);
#endif
        }
        else if (i == ct.totalCoeff - 1U)
        {
            run = zerosLeft;
        }

        // Map scan position to raster position — ITU-T H.264 §8.5.6.
        // For 4x4 blocks (maxCoeff=16 or 15): use zigzag scan table.
        // For chroma DC 2x2 blocks (maxCoeff=4): identity mapping (no zigzag).
        // Reference: libavc uses pu1_inv_scan = {0,1,2,3} for chroma DC.
        if (coeffIdx < 16U)
        {
            uint32_t rasterPos = (maxCoeff <= 4U) ? coeffIdx : cZigzag4x4[coeffIdx];
            block.coeffs[rasterPos] = levels[i];
        }

        if (coeffIdx >= run)
            coeffIdx -= (run + 1U);
    }

#if SUB0H264_TRACE
    // Dump raw levels for debugging — controlled by a static call counter
    static uint32_t sResidualCallCount = 0U;
    ++sResidualCallCount;
    // Block 11 of MB 9 is the 2nd residual block call for MB 9
    // (block 10 has TC=0 so decodeResidualBlock returns early at TC=0 check,
    //  but the call still increments this counter)
    // MB 0-8 = 9 MBs × 1 DC block each = 9 calls (some with TC=0)
    // MB 5 has cbpC=1 → 2 chroma DC blocks = +2 calls
    // Total before MB 9: ~11 calls
    // MB 9: block 10 (TC=0) = 1 call, block 11 (TC=16) = 2nd call
    if (ct.totalCoeff > 0U)
    {
        std::printf("[CAVLC] call#%u tc=%u to=%u tz=%u levels=[",
            sResidualCallCount, ct.totalCoeff, ct.trailingOnes, totalZeros);
        for (uint32_t k = 0U; k < ct.totalCoeff; ++k)
            std::printf("%d ", levels[k]);
        std::printf("] coeffs=[");
        for (uint32_t k = 0U; k < 16U; ++k)
            std::printf("%d ", block.coeffs[k]);
        std::printf("]\n");
    }
#endif

    return Result::Ok;
}

// ── Macroblock types — ITU-T H.264 Tables 7-11, 7-13 ───────────────────

/// I-slice macroblock types — ITU-T H.264 Table 7-11.
enum class IMbType : uint8_t
{
    I_4x4    = 0U,
    I_16x16_0_0_0 = 1U,  ///< I_16x16 with pred_mode=0, cbp_luma=0, cbp_chroma=0
    I_16x16_1_0_0 = 2U,
    I_16x16_2_0_0 = 3U,
    I_16x16_3_0_0 = 4U,
    I_16x16_0_1_0 = 5U,
    I_16x16_1_1_0 = 6U,
    I_16x16_2_1_0 = 7U,
    I_16x16_3_1_0 = 8U,
    I_16x16_0_2_0 = 9U,
    I_16x16_1_2_0 = 10U,
    I_16x16_2_2_0 = 11U,
    I_16x16_3_2_0 = 12U,
    I_16x16_0_0_15 = 13U,
    I_16x16_1_0_15 = 14U,
    I_16x16_2_0_15 = 15U,
    I_16x16_3_0_15 = 16U,
    I_16x16_0_1_15 = 17U,
    I_16x16_1_1_15 = 18U,
    I_16x16_2_1_15 = 19U,
    I_16x16_3_1_15 = 20U,
    I_16x16_0_2_15 = 21U,
    I_16x16_1_2_15 = 22U,
    I_16x16_2_2_15 = 23U,
    I_16x16_3_2_15 = 24U,
    I_PCM         = 25U,
};

/** Extract I_16x16 macroblock properties from mb_type.
 *  For I_16x16 types (1-24): mb_type = 1 + predMode + cbpChroma*4 + cbpLuma12*12.
 *  where cbpLuma12=0→cbpLuma=0, cbpLuma12=1→cbpLuma=15.
 *  Reference: ITU-T H.264 Table 7-11. [CHECKED Table 7-11]
 */
inline bool isI16x16(uint8_t mbType) noexcept { return mbType >= 1U && mbType <= 24U; }
inline uint8_t i16x16PredMode(uint8_t mbType) noexcept { return (mbType - 1U) % 4U; }
inline uint8_t i16x16CbpLuma(uint8_t mbType) noexcept { return ((mbType - 1U) / 4U < 3U) ? 0U : 15U; }
inline uint8_t i16x16CbpChroma(uint8_t mbType) noexcept { return ((mbType - 1U) / 4U) % 3U; }

/// P-slice macroblock types — ITU-T H.264 Table 7-13.
enum class PMbType : uint8_t
{
    P_L0_16x16  = 0U,
    P_L0_L0_16x8 = 1U,
    P_L0_L0_8x16 = 2U,
    P_8x8       = 3U,
    P_8x8ref0   = 4U,
};

// ── Macroblock data — decoded syntax elements for one MB ────────────────

/// Decoded macroblock data from CAVLC parsing.
struct MacroblockData
{
    uint8_t mbType = 0U;
    bool isIntra = false;           ///< True for I-MB types
    bool isSkipped = false;         ///< True for P_Skip
    bool isI16x16 = false;          ///< True for I_16x16 subtypes

    // Intra prediction
    uint8_t intraPredMode16x16 = 0U;
    uint8_t intraPredMode4x4[16]{};
    uint8_t intraChromaPredMode = 0U;

    // Inter prediction
    int16_t mvdL0[4][2]{};         ///< Motion vector differences [partition][x,y]
    uint8_t refIdxL0[4]{};         ///< Reference frame indices
    uint8_t numPartitions = 0U;

    // Coded block pattern
    uint8_t cbpLuma = 0U;          ///< Bits [3:0] for 4 8x8 luma blocks
    uint8_t cbpChroma = 0U;        ///< 0=none, 1=DC only, 2=DC+AC

    // QP
    int8_t qpDelta = 0;
    int32_t qp = 0;                ///< Effective QP for this MB

    // Residual coefficients
    ResidualBlock4x4 lumaDc;                ///< I_16x16 DC block
    ResidualBlock4x4 lumaBlocks[16]{};      ///< 16 luma 4x4 blocks
    ResidualBlock4x4 chromaDcCb;            ///< Chroma Cb DC
    ResidualBlock4x4 chromaDcCr;            ///< Chroma Cr DC
    ResidualBlock4x4 chromaBlocksCb[4]{};   ///< 4 chroma Cb AC blocks
    ResidualBlock4x4 chromaBlocksCr[4]{};   ///< 4 chroma Cr AC blocks

    // Non-zero coefficient counts (for neighbor context)
    uint8_t nnz[16]{};             ///< Non-zero count per 4x4 luma block
    uint8_t nnzCb[4]{};            ///< Non-zero count per chroma Cb block
    uint8_t nnzCr[4]{};            ///< Non-zero count per chroma Cr block
};

} // namespace sub0h264

#endif // CROG_SUB0H264_CAVLC_HPP
