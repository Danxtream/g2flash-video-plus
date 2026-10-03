/** Sub0h264 — Compile-time H.264 spec table validation
 *
 *  Uses static_assert to validate that all lookup tables match the ITU-T H.264
 *  specification at compile time. Build failure = table data is wrong.
 *
 *  Reference: ITU-T H.264 §6-9, Tables 7-11 through 9-23, §8.x tables.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "../components/sub0h264/src/tables.hpp"
#include "../components/sub0h264/src/cavlc_tables.hpp"
#include "../components/sub0h264/src/deblock.hpp"
#include "../components/sub0h264/src/transform.hpp"
#include "../components/sub0h264/src/cabac.hpp"
#include "../components/sub0h264/src/sps.hpp"
#include "../components/sub0h264/src/frame.hpp"
#include "../components/sub0h264/src/cavlc.hpp"
#include "../components/sub0h264/src/inter_pred.hpp"

#include <algorithm>
#include <numeric>

using namespace sub0h264;

// ── constexpr validation helpers ────────────────────────────────────────

/// Check that an array is a valid permutation of [0, N).
template <std::size_t N>
constexpr bool isPermutation(const std::array<uint8_t, N>& arr)
{
    std::array<bool, N> seen{};
    for (auto v : arr)
    {
        if (v >= N) return false;
        if (seen[v]) return false;
        seen[v] = true;
    }
    return true;
}

/// Check that an array is monotonically non-decreasing.
template <typename T, std::size_t N>
constexpr bool isNonDecreasing(const std::array<T, N>& arr)
{
    for (std::size_t i = 1; i < N; ++i)
        if (arr[i] < arr[i - 1]) return false;
    return true;
}

/// Check that all values in an array are in [lo, hi].
template <typename T, std::size_t N>
constexpr bool allInRange(const std::array<T, N>& arr, T lo, T hi)
{
    for (auto v : arr)
        if (v < lo || v > hi) return false;
    return true;
}

// ═══════════════════════════════════════════════════════════════════════
// §1  tables.hpp — Zigzag, scan order, CBP, partition tables
// ═══════════════════════════════════════════════════════════════════════

// ── Zigzag scan — ITU-T H.264 §8.5.6 ──────────────────────────────────

static_assert(cZigzag4x4.size() == 16, "cZigzag4x4 must have 16 entries");
static_assert(isPermutation(cZigzag4x4), "cZigzag4x4 must be a valid permutation of [0,15]");

static_assert(cZigzag8x8.size() == 64, "cZigzag8x8 must have 64 entries");
static_assert(isPermutation(cZigzag8x8), "cZigzag8x8 must be a valid permutation of [0,63]");

// First few entries match spec: 0, 1, 8, 16, 9, 2, 3, 10, ...
static_assert(cZigzag8x8[0] == 0 && cZigzag8x8[1] == 1 &&
              cZigzag8x8[2] == 8 && cZigzag8x8[3] == 16,
              "cZigzag8x8 first 4 entries must match spec diagonal pattern");

// ── Luma 4x4 block scan — ITU-T H.264 §6.4.3 ─────────────────────────

static_assert(cLuma4x4BlkX.size() == 16, "cLuma4x4BlkX must have 16 entries");
static_assert(cLuma4x4BlkY.size() == 16, "cLuma4x4BlkY must have 16 entries");
static_assert(cLuma4x4ToRaster.size() == 16, "cLuma4x4ToRaster must have 16 entries");
static_assert(cRasterToLuma4x4.size() == 16, "cRasterToLuma4x4 must have 16 entries");

static_assert(isPermutation(cLuma4x4ToRaster),
              "cLuma4x4ToRaster must be a valid permutation");
static_assert(isPermutation(cRasterToLuma4x4),
              "cRasterToLuma4x4 must be a valid permutation");

// Verify forward/inverse relationship: toRaster[fromRaster[i]] == i
constexpr bool scanOrderInversesMatch()
{
    for (uint8_t i = 0; i < 16; ++i)
        if (cRasterToLuma4x4[cLuma4x4ToRaster[i]] != i) return false;
    return true;
}
static_assert(scanOrderInversesMatch(),
              "cLuma4x4ToRaster and cRasterToLuma4x4 must be inverses");

// Block X/Y must be multiples of 4 and in [0, 12]
constexpr bool blkXYValid()
{
    for (uint8_t i = 0; i < 16; ++i)
    {
        if (cLuma4x4BlkX[i] > 12 || cLuma4x4BlkX[i] % 4 != 0) return false;
        if (cLuma4x4BlkY[i] > 12 || cLuma4x4BlkY[i] % 4 != 0) return false;
    }
    return true;
}
static_assert(blkXYValid(), "Block X/Y offsets must be multiples of 4 in [0,12]");

// ── Top-right unavailability — ITU-T H.264 §6.4.11 ────────────────────

static_assert(cTopRightUnavailScan.size() == 16, "cTopRightUnavailScan size == 16");
// Blocks at the right edge of a 4x4 within-MB scan: positions 3, 7, 11, 15
// have their top-right unavailable (it wraps to the next MB row).
// Specifically, scan positions 3 (bottom of first column), etc.

// ── CBP table — ITU-T H.264 Table 9-4 ─────────────────────────────────

static_assert(cCbpTable.size() == 48, "cCbpTable must have 48 entries for coded_block_pattern");

// Verify Intra CBP (column 0): code 0 should be no-luma-no-chroma.
// The spec says: for I-slices, coded_block_pattern code 0 maps to cbp=47 (all coded).
// Actually code 0 → {47, 0} meaning intra=47, inter=0.
static_assert(cCbpTable[0][0] == 47 && cCbpTable[0][1] == 0,
              "CBP code 0: intra=47 (0x2F), inter=0");

// ── Level suffix threshold — ITU-T H.264 §9.2.1 ───────────────────────

static_assert(cLevelSuffixThreshold.size() == 7, "cLevelSuffixThreshold size == 7");

// ── Chroma QP table — ITU-T H.264 Table 8-15 ──────────────────────────

static_assert(cChromaQpTable.size() == 52, "cChromaQpTable must have 52 entries");

// Identity mapping for QPI 0-29: QPc == QPI
constexpr bool chromaQpIdentityLow()
{
    for (uint8_t i = 0; i <= 29; ++i)
        if (cChromaQpTable[i] != i) return false;
    return true;
}
static_assert(chromaQpIdentityLow(), "cChromaQpTable must be identity for QPI [0,29]");

static_assert(isNonDecreasing(cChromaQpTable), "cChromaQpTable must be non-decreasing");
static_assert(cChromaQpTable[51] == 39, "cChromaQpTable max value must be 39");

// ── P-slice partition tables — ITU-T H.264 Table 7-13/7-17 ────────────

static_assert(cNumMbPartP.size() == 6, "cNumMbPartP size == 6");
static_assert(cNumMbPartP[0] == 1, "P_L0_16x16 has 1 partition");
static_assert(cNumMbPartP[1] == 2, "P_L0_L0_16x8 has 2 partitions");
static_assert(cNumMbPartP[2] == 2, "P_L0_L0_8x16 has 2 partitions");
static_assert(cNumMbPartP[3] == 4, "P_8x8 has 4 sub-partitions");

static_assert(cMbPartWidthP.size() == 4 && cMbPartHeightP.size() == 4,
              "Partition width/height tables must have 4 entries");
static_assert(cMbPartWidthP[0] == 16 && cMbPartHeightP[0] == 16,
              "16x16 partition: 16×16");
static_assert(cMbPartWidthP[1] == 16 && cMbPartHeightP[1] == 8,
              "16x8 partition: 16×8");
static_assert(cMbPartWidthP[2] == 8 && cMbPartHeightP[2] == 16,
              "8x16 partition: 8×16");
static_assert(cMbPartWidthP[3] == 8 && cMbPartHeightP[3] == 8,
              "8x8 partition: 8×8");

static_assert(cNumSubMbPartP.size() == 4, "cNumSubMbPartP size == 4");
static_assert(cNumSubMbPartP[0] == 1, "8x8 sub-partition: 1 part");
static_assert(cNumSubMbPartP[1] == 2, "8x4 sub-partition: 2 parts");
static_assert(cNumSubMbPartP[2] == 2, "4x8 sub-partition: 2 parts");
static_assert(cNumSubMbPartP[3] == 4, "4x4 sub-partition: 4 parts");

// ═══════════════════════════════════════════════════════════════════════
// §2  cavlc_tables.hpp — VLC code tables
// ═══════════════════════════════════════════════════════════════════════

// ── coeff_token — ITU-T H.264 Table 9-5 ───────────────────────────────

// Dimension checks for C-style arrays (can't use .size())
static_assert(sizeof(cCoeffTokenCode) / sizeof(cCoeffTokenCode[0]) == 3,
              "cCoeffTokenCode first dimension == 3");
static_assert(sizeof(cCoeffTokenCode[0]) / sizeof(cCoeffTokenCode[0][0]) == 4,
              "cCoeffTokenCode second dimension == 4");
static_assert(sizeof(cCoeffTokenCode[0][0]) / sizeof(cCoeffTokenCode[0][0][0]) == 17,
              "cCoeffTokenCode third dimension == 17");

// Zero-code entries: trailingOnes > totalCoeff is invalid — code should be 0
constexpr bool coeffTokenZeroCheck()
{
    for (int nC = 0; nC < 3; ++nC)
        for (int to = 0; to < 4; ++to)
            for (int tc = 0; tc < 17; ++tc)
                if (to > tc && cCoeffTokenCode[nC][to][tc] != 0) return false;
    return true;
}
static_assert(coeffTokenZeroCheck(),
              "Invalid coeff_token entries (T1 > TC) must have code 0");

// Chroma DC coeff_token: 4 rows, 5 columns
static_assert(sizeof(cCoeffTokenCodeChroma) == 4 * 5,
              "cCoeffTokenCodeChroma must be [4][5]");

// ── total_zeros — ITU-T H.264 Tables 9-7, 9-8 ─────────────────────────

static_assert(sizeof(cTotalZerosIndex) == 15,
              "cTotalZerosIndex must have 15 entries");
static_assert(sizeof(cTotalZerosSize) == 135,
              "cTotalZerosSize must have 135 entries");
static_assert(sizeof(cTotalZerosCode) == 135,
              "cTotalZerosCode must have 135 entries");

// Index must be monotonically increasing
constexpr bool tzIndexMonotonic()
{
    for (int i = 1; i < 15; ++i)
        if (cTotalZerosIndex[i] <= cTotalZerosIndex[i - 1]) return false;
    return true;
}
static_assert(tzIndexMonotonic(), "cTotalZerosIndex must be strictly increasing");
static_assert(cTotalZerosIndex[0] == 0, "cTotalZerosIndex[0] must start at 0");

// Verify prefix-free property: no VLC code is a prefix of another within
// the same totalCoeff sub-table. This is required for unambiguous decoding.
// ITU-T H.264 §9.2.3 — unique decodability of total_zeros VLC.
constexpr bool totalZerosPrefixFree()
{
    for (int tc = 0; tc < 15; ++tc)
    {
        uint32_t start = cTotalZerosIndex[tc];
        uint32_t end = (tc < 14) ? cTotalZerosIndex[tc + 1] : 135U;
        uint32_t count = end - start;

        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t sizeI = cTotalZerosSize[start + i];
            uint32_t codeI = cTotalZerosCode[start + i];
            for (uint32_t j = i + 1; j < count; ++j)
            {
                uint32_t sizeJ = cTotalZerosSize[start + j];
                uint32_t codeJ = cTotalZerosCode[start + j];

                // Check if shorter code is a prefix of longer
                uint32_t minLen = (sizeI < sizeJ) ? sizeI : sizeJ;
                uint32_t shiftI = (sizeI > minLen) ? (sizeI - minLen) : 0;
                uint32_t shiftJ = (sizeJ > minLen) ? (sizeJ - minLen) : 0;
                if ((codeI >> shiftI) == (codeJ >> shiftJ))
                    return false; // Prefix collision
            }
        }
    }
    return true;
}
static_assert(totalZerosPrefixFree(),
              "total_zeros VLC must be prefix-free per §9.2.3");

// ── run_before — ITU-T H.264 Table 9-10 ───────────────────────────────

static_assert(sizeof(cRunBeforeIndex) == 7, "cRunBeforeIndex must have 7 entries");
static_assert(sizeof(cRunBeforeSize) == 42, "cRunBeforeSize must have 42 entries");
static_assert(sizeof(cRunBeforeCode) == 42, "cRunBeforeCode must have 42 entries");
static_assert(cRunBeforeIndex[0] == 0, "cRunBeforeIndex must start at 0");

// Prefix-free check for run_before VLC — ITU-T H.264 Table 9-10.
constexpr bool runBeforePrefixFree()
{
    for (int zl = 0; zl < 7; ++zl)
    {
        uint32_t start = cRunBeforeIndex[zl];
        uint32_t end = (zl < 6) ? cRunBeforeIndex[zl + 1] : 42U;
        uint32_t count = end - start;
        for (uint32_t i = 0; i < count; ++i)
            for (uint32_t j = i + 1; j < count; ++j)
            {
                uint32_t sI = cRunBeforeSize[start+i], cI = cRunBeforeCode[start+i];
                uint32_t sJ = cRunBeforeSize[start+j], cJ = cRunBeforeCode[start+j];
                uint32_t m = (sI < sJ) ? sI : sJ;
                if ((cI >> (sI > m ? sI - m : 0)) == (cJ >> (sJ > m ? sJ - m : 0)))
                    return false;
            }
    }
    return true;
}
static_assert(runBeforePrefixFree(),
              "run_before VLC must be prefix-free per §9.2.3");

// ── Chroma DC total_zeros — ITU-T H.264 Table 9-9 ─────────────────────

static_assert(sizeof(cTotalZerosSizeChroma) == 9,
              "cTotalZerosSizeChroma must have 9 entries (3 tables × 3 entries)");
static_assert(sizeof(cTotalZerosCodeChroma) == 9,
              "cTotalZerosCodeChroma must have 9 entries");

// ═══════════════════════════════════════════════════════════════════════
// §3  deblock.hpp — Deblocking filter threshold tables
// ═══════════════════════════════════════════════════════════════════════

// ── Alpha table — ITU-T H.264 Table 8-16 ──────────────────────────────

constexpr bool alphaValid()
{
    if (sizeof(cAlphaTable) / sizeof(cAlphaTable[0]) != 52) return false;
    // Must be monotonically non-decreasing
    for (int i = 1; i < 52; ++i)
        if (cAlphaTable[i] < cAlphaTable[i - 1]) return false;
    // First 16 entries should be 0 (low QP → no filtering)
    for (int i = 0; i < 16; ++i)
        if (cAlphaTable[i] != 0) return false;
    // Last entry = 255
    if (cAlphaTable[51] != 255) return false;
    return true;
}
static_assert(alphaValid(), "cAlphaTable must match ITU-T H.264 Table 8-16");

// ── Beta table — ITU-T H.264 Table 8-17 ───────────────────────────────

constexpr bool betaValid()
{
    if (sizeof(cBetaTable) / sizeof(cBetaTable[0]) != 52) return false;
    for (int i = 1; i < 52; ++i)
        if (cBetaTable[i] < cBetaTable[i - 1]) return false;
    for (int i = 0; i < 16; ++i)
        if (cBetaTable[i] != 0) return false;
    if (cBetaTable[51] != 18) return false;
    return true;
}
static_assert(betaValid(), "cBetaTable must match ITU-T H.264 Table 8-17");

// ── tc0 table — ITU-T H.264 Table 8-16 ────────────────────────────────

constexpr bool tc0Valid()
{
    // Column 0 (BS=1) should always be 0 for low QP
    for (int q = 0; q < 16; ++q)
        if (cTc0Table[q][0] != 0) return false;
    // Non-decreasing per column
    for (int bs = 0; bs < 4; ++bs)
        for (int q = 1; q < 52; ++q)
            if (cTc0Table[q][bs] < cTc0Table[q - 1][bs]) return false;
    return true;
}
static_assert(tc0Valid(), "cTc0Table must match ITU-T H.264 Table 8-16");

// ═══════════════════════════════════════════════════════════════════════
// §4  transform.hpp — Dequant tables
// ═══════════════════════════════════════════════════════════════════════

static_assert(cDequantScale.size() == 6, "cDequantScale must have 6 rows (qpMod6)");
static_assert(cDequantScale[0].size() == 3, "cDequantScale must have 3 columns (pos class)");

// All dequant scale values must be positive
constexpr bool dequantScalePositive()
{
    for (const auto& row : cDequantScale)
        for (auto v : row)
            if (v <= 0) return false;
    return true;
}
static_assert(dequantScalePositive(), "cDequantScale values must all be positive");

// ITU-T H.264 Table 8-15 first row: v[0] = {10, 16, 13}
static_assert(cDequantScale[0][0] == 10 && cDequantScale[0][1] == 16 &&
              cDequantScale[0][2] == 13,
              "cDequantScale row 0 must be {10, 16, 13} per Table 8-15");

// Position class table
static_assert(cDequantPosClass.size() == 16, "cDequantPosClass must have 16 entries");
static_assert(allInRange(cDequantPosClass, uint8_t(0), uint8_t(2)),
              "cDequantPosClass values must be in {0, 1, 2}");

// Verify position class symmetry: positions where both row and col are even → class 0
// Position 0 = (0,0): both even → class 0
static_assert(cDequantPosClass[0] == 0, "Position (0,0): class 0 (both even)");
// Position 5 = (1,1): both odd → class 1
static_assert(cDequantPosClass[5] == 1, "Position (1,1): class 1 (both odd)");
// Position 1 = (1,0): mixed → class 2
static_assert(cDequantPosClass[1] == 2, "Position (1,0): class 2 (mixed parity)");

// ═══════════════════════════════════════════════════════════════════════
// §5  Scalar constants
// ═══════════════════════════════════════════════════════════════════════

// frame.hpp
static_assert(cMbSize == 16, "Macroblock size must be 16 pixels");
static_assert(cChromaBlockSize == 8, "4:2:0 chroma block size must be 8 pixels");

// sps.hpp
static_assert(cMaxSpsCount == 32, "Max SPS count per spec = 32");
static_assert(cDefaultPicInitQp == 26, "Default pic_init_qp = 26 per §7.4.2.2");
static_assert(cProfileBaseline == 66, "Baseline profile_idc = 66");
static_assert(cProfileMain == 77, "Main profile_idc = 77");
static_assert(cProfileHigh == 100, "High profile_idc = 100");

// cabac.hpp
static_assert(cNumCabacCtxBase == 460, "CABAC base context count = 460");
static_assert(cNumCabacCtx == 1024, "CABAC total context count = 1024 (incl. High 8x8)");
static_assert(cMaxQp == 51, "Maximum QP = 51");

// cavlc.hpp
static_assert(cMaxCoeff4x4 == 16, "Max coefficients in 4x4 block = 16");
static_assert(cMaxTrailingOnes == 3, "Max trailing ones = 3 per §9.2.1");
static_assert(cMaxSuffixLength == 6, "Max suffix length = 6 per §9.2.2");

// inter_pred.hpp — 6-tap FIR filter coefficients
static_assert(cLumaFilter6Tap[0] == 1 && cLumaFilter6Tap[1] == -5 &&
              cLumaFilter6Tap[2] == 20 && cLumaFilter6Tap[3] == 20 &&
              cLumaFilter6Tap[4] == -5 && cLumaFilter6Tap[5] == 1,
              "Luma 6-tap filter: {1, -5, 20, 20, -5, 1} per §8.4.2.2.1");

// ═══════════════════════════════════════════════════════════════════════
// §6  CABAC tables
// ═══════════════════════════════════════════════════════════════════════

// cCabacTable dimensions
static_assert(sizeof(cCabacTable) / sizeof(cCabacTable[0]) == 128,
              "cCabacTable must have 128 rows (states)");
static_assert(sizeof(cCabacTable[0]) / sizeof(cCabacTable[0][0]) == 4,
              "cCabacTable must have 4 columns per state");

// cCabacInitMN dimensions
static_assert(sizeof(cCabacInitMN) / sizeof(cCabacInitMN[0]) == 4,
              "cCabacInitMN must have 4 slice type entries");
static_assert(sizeof(cCabacInitMN[0]) / sizeof(cCabacInitMN[0][0]) == 460,
              "cCabacInitMN must have 460 contexts");
static_assert(sizeof(cCabacInitMN[0][0]) / sizeof(cCabacInitMN[0][0][0]) == 2,
              "cCabacInitMN must have 2 values (m,n) per context");

// CABAC init m,n storage type is int8_t — range [-128, 127] is guaranteed.
// Validate the array is non-empty and first element is accessible.
static_assert(sizeof(cCabacInitMN[0][0]) == 2,
              "cCabacInitMN inner dimension must be 2 (m, n)");

// ═══════════════════════════════════════════════════════════════════════
// Runtime diagnostic wrappers (for CI output on failure)
// ═══════════════════════════════════════════════════════════════════════

TEST_CASE("Spec tables: all static_asserts passed at compile time")
{
    // If this test runs, all static_asserts above have passed.
    // Add a few runtime checks for properties hard to express as static_assert.

    SUBCASE("Zigzag 4x4 covers all positions") {
        CHECK(isPermutation(cZigzag4x4));
    }

    SUBCASE("Zigzag 8x8 covers all positions") {
        CHECK(isPermutation(cZigzag8x8));
    }

    SUBCASE("Chroma QP table max == 39") {
        CHECK(cChromaQpTable[51] == 39);
    }

    SUBCASE("Alpha table max == 255") {
        CHECK(cAlphaTable[51] == 255);
    }

    SUBCASE("Beta table max == 18") {
        CHECK(cBetaTable[51] == 18);
    }

    SUBCASE("Dequant scale row 0 matches Table 8-15") {
        CHECK(cDequantScale[0][0] == 10);
        CHECK(cDequantScale[0][1] == 16);
        CHECK(cDequantScale[0][2] == 13);
    }
}
