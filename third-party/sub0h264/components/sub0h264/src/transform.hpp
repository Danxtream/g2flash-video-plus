/** Sub0h264 — Inverse transform + quantization + reconstruction
 *
 *  Implements the H.264 4x4 inverse integer DCT, Hadamard transforms,
 *  inverse quantization, and pixel reconstruction.
 *
 *  Reference: ITU-T H.264 §8.5.12 (inverse transform)
 *
 *  Spec-annotated review (2026-04-09):
 *    §8.5.12.2 4x4 IDCT: column-then-row butterfly, (val+32)>>6 rounding [CHECKED §8.5.12.2]
 *    §8.5.12.2 8x8 IDCT: even/odd decomposition, §8-325..8-333 equations [CHECKED §8.5.12.2]
 *    §8.5.12.1 4x4 dequant: c * LevelScale << qpDiv6 [CHECKED §8.5.12.1]
 *    §8.5.12.1 8x8 dequant: two-branch shift (qpDiv6>=2 left, else right+round) [CHECKED §8.5.12.1]
 *    §8.5.12.1 Table 8-15 scale factors: 6 static_asserts [CHECKED FM-14]
 *    §8.5.12.1 Table 8-16 8x8 scale factors: 2 static_asserts [CHECKED FM-14]
 *    §8.5.10 4x4 Hadamard: H4 matrix verified (sign pattern +++/++--/+--+/+-+-) [CHECKED §8.5.10]
 *    §8.5.11 2x2 Hadamard: H2⊗H2 matrix verified [CHECKED §8.5.11]
 *    §8.1 clipU8: Clip1Y(x) = Clip3(0, 255, x) [CHECKED §8.1]
 *    FM-6: All rounding biases = 2^(shift-1) (midpoint rounding) [CHECKED FM-6]
 *    FM-10: DC dequant uses dedicated functions (dequantLumaDcValues, dequantChromaDcValues)
 *           which use LevelScale(qp%6, 0, 0) for ALL positions. [CHECKED FM-10]
 *    NOTE: isDc parameter in inverseQuantize4x4 is dead code (never called with true).
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_TRANSFORM_HPP
#define CROG_SUB0H264_TRANSFORM_HPP

// Uncomment to enable spec-correct dequant (closes CABAC-CAVLC gap to 0.5 dB
// but drops absolute PSNR due to 16× normalization factor — see cabac-quality-fix.md)
// #define SUB0H264_SPEC_DEQUANT

// When defined, apply >>6 normalization at dequant level (per-coefficient),
// not at the IDCT output level. This matches ffmpeg's convention and produces
// correct rounding for multi-coefficient blocks. Without this, the IDCT output
// >>6 causes different rounding behavior that diverges from ffmpeg on noisy content.
// #define SUB0H264_DEQUANT_NORM

#include <cstdint>
#include <algorithm>
#include <array>

namespace sub0h264 {

/// Clip value to [0, 255] for 8-bit pixel output.
inline constexpr int32_t clipU8(int32_t val) noexcept
{
    return val < 0 ? 0 : (val > 255 ? 255 : val);
}

/// Clamp QP-derived index to [0, 51]. Avoids std::min/max type deduction issues.
inline constexpr int32_t clampQpIdx(int32_t val) noexcept
{
    return val < 0 ? 0 : (val > 51 ? 51 : val);
}

// ── Default dequantization scaling — ITU-T H.264 §8.5.12.1 ─────────────

/// Dequantization scale factors for 4x4 blocks, indexed by QP%6 and position class.
/// v[qp%6][position_class] where:
///   class 0 = both-even (e.g., (0,0),(0,2),(2,0),(2,2))
///   class 1 = both-odd  (e.g., (1,1),(1,3),(3,1),(3,3))
///   class 2 = mixed     (all other positions)
/// Reference: ITU-T H.264 Table 8-15, LevelScale4x4[k][i][j].
/// Column order: {class0, class1, class2} = {both-even, both-odd, mixed}.
inline constexpr std::array<std::array<int32_t, 3>, 6> cDequantScale = {{
    {{ 10, 16, 13 }},
    {{ 11, 18, 14 }},
    {{ 13, 20, 16 }},
    {{ 14, 23, 18 }},
    {{ 16, 25, 20 }},
    {{ 18, 29, 23 }},
}};

/// Position class for each raster position in a 4x4 block — §8.5.12.1.
/// Derived from (row, col) parity:
///   both-even (0,0),(0,2),(2,0),(2,2) → class 0
///   both-odd  (1,1),(1,3),(3,1),(3,3) → class 1
///   mixed                             → class 2
/// Determines which normAdjust column is used in dequantization.
inline constexpr std::array<uint8_t, 16> cDequantPosClass = []() constexpr
{
    std::array<uint8_t, 16> t{};
    for (uint32_t i = 0U; i < 16U; ++i)
    {
        uint32_t col = i % 4U;
        uint32_t row = i / 4U;
        if (col % 2U == 0U && row % 2U == 0U)
            t[i] = 0U;
        else if (col % 2U == 1U && row % 2U == 1U)
            t[i] = 1U;
        else
            t[i] = 2U;
    }
    return t;
}();

// Compile-time spot-checks — ITU-T H.264 §8.5.12.1 Table 8-15.
static_assert(cDequantPosClass[0]  == 0U, "pos(0,0) → class 0");
static_assert(cDequantPosClass[5]  == 1U, "pos(1,1) → class 1");
static_assert(cDequantPosClass[1]  == 2U, "pos(0,1) → class 2");
static_assert(cDequantPosClass[15] == 1U, "pos(3,3) → class 1");
// Scale value checks: both-odd (class 1) must be larger than both-even (class 0)
// per Table 8-15, e.g., qp%6=0 → class0=10, class1=16, class2=13.
static_assert(cDequantScale[0][0] == 10, "qp%6=0, class0 = 10 per Table 8-15");
static_assert(cDequantScale[0][1] == 16, "qp%6=0, class1 = 16 per Table 8-15");
static_assert(cDequantScale[0][2] == 13, "qp%6=0, class2 = 13 per Table 8-15");
static_assert(cDequantScale[3][1] == 23, "qp%6=3, class1 = 23 per Table 8-15");

// ── 4x4 Inverse Integer DCT — ITU-T H.264 §8.5.12 ─────────────────────

/** Apply inverse 4x4 integer DCT and add to prediction, storing result.
 *
 *  Implements the butterfly in ITU-T H.264 §8.5.12.2.
 *  The >> 6 normalization is absorbed from the dequant scaling.
 *
 *  @param coeffs     Input: 16 dequantized coefficients in raster order
 *  @param pred       Input: 4x4 prediction block (stride = predStride)
 *  @param out        Output: 4x4 reconstructed block (stride = outStride)
 *  @param predStride Stride of prediction buffer
 *  @param outStride  Stride of output buffer
 */
inline void inverseDct4x4AddPred(const int16_t* coeffs,
                                  const uint8_t* pred, uint32_t predStride,
                                  uint8_t* out, uint32_t outStride) noexcept
{
    // ITU-T H.264 §8.5.12.2: 4x4 inverse integer transform. [CHECKED §8.5.12.2]
    // ffmpeg-style: add rounding bias to block[0] before butterfly, then >>6 at output.
    // This matches ffmpeg's ff_h264_idct_add exactly. [CHECKED FM-6]
    int16_t block[16];
    std::memcpy(block, coeffs, 16 * sizeof(int16_t));

    // ffmpeg: block[0] += 1 << 5 (rounding bias for the >>6 at output)
    block[0] += 32;

    // Pass 1: Column transform
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        int32_t z0 = block[i + 4 * 0] + block[i + 4 * 2];
        int32_t z1 = block[i + 4 * 0] - block[i + 4 * 2];
        int32_t z2 = (block[i + 4 * 1] >> 1) - block[i + 4 * 3];
        int32_t z3 = block[i + 4 * 1] + (block[i + 4 * 3] >> 1);

        block[i + 4 * 0] = static_cast<int16_t>(z0 + z3);
        block[i + 4 * 1] = static_cast<int16_t>(z1 + z2);
        block[i + 4 * 2] = static_cast<int16_t>(z1 - z2);
        block[i + 4 * 3] = static_cast<int16_t>(z0 - z3);
    }

    // Pass 2: Row transform + prediction + clip
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        int32_t z0 = block[0 + 4 * i] + block[2 + 4 * i];
        int32_t z1 = block[0 + 4 * i] - block[2 + 4 * i];
        int32_t z2 = (block[1 + 4 * i] >> 1) - block[3 + 4 * i];
        int32_t z3 = block[1 + 4 * i] + (block[3 + 4 * i] >> 1);

        // ffmpeg-style: >>6 at output. Rounding bias was added to block[0] before butterfly.
        out[i * outStride + 0] = static_cast<uint8_t>(clipU8(pred[i * predStride + 0] + ((z0 + z3) >> 6)));
        out[i * outStride + 1] = static_cast<uint8_t>(clipU8(pred[i * predStride + 1] + ((z1 + z2) >> 6)));
        out[i * outStride + 2] = static_cast<uint8_t>(clipU8(pred[i * predStride + 2] + ((z1 - z2) >> 6)));
        out[i * outStride + 3] = static_cast<uint8_t>(clipU8(pred[i * predStride + 3] + ((z0 - z3) >> 6)));
    }
}

/** DC-only fast path: add constant DC value to prediction.
 *  Used when only the DC coefficient is non-zero.
 */
inline void inverseDcOnly4x4AddPred(int16_t dcCoeff,
                                     const uint8_t* pred, uint32_t predStride,
                                     uint8_t* out, uint32_t outStride) noexcept
{
    // ffmpeg-style: rounding at block[0] then >>6
    int32_t dcVal = (dcCoeff + 32) >> 6;

    for (uint32_t row = 0U; row < 4U; ++row)
    {
        for (uint32_t col = 0U; col < 4U; ++col)
        {
            out[row * outStride + col] = static_cast<uint8_t>(
                clipU8(pred[row * predStride + col] + dcVal));
        }
    }
}

// ── 4x4 Hadamard Inverse Transform (Luma DC) — ITU-T H.264 §8.5.12.2 ──

/** Inverse 4x4 Hadamard transform for I_16x16 luma DC coefficients.
 *  §8.5.10: 2-pass separable H4 transform.
 *  Row pattern verified: [++++], [++--], [+--+], [+-+-]. [CHECKED §8.5.10]
 *  @param[in,out] dc  16 DC coefficients, transformed in-place
 */
inline void inverseHadamard4x4(int16_t* dc) noexcept
{
    int32_t tmp[16];

    // Horizontal transform
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        int32_t s0 = dc[i * 4 + 0];
        int32_t s1 = dc[i * 4 + 1];
        int32_t s2 = dc[i * 4 + 2];
        int32_t s3 = dc[i * 4 + 3];

        int32_t x0 = s0 + s3;
        int32_t x1 = s1 + s2;
        int32_t x2 = s1 - s2;
        int32_t x3 = s0 - s3;

        tmp[i * 4 + 0] = x0 + x1;
        tmp[i * 4 + 1] = x2 + x3;
        tmp[i * 4 + 2] = x0 - x1;
        tmp[i * 4 + 3] = x3 - x2;
    }

    // Vertical transform
    for (uint32_t j = 0U; j < 4U; ++j)
    {
        int32_t t0 = tmp[0 * 4 + j];
        int32_t t1 = tmp[1 * 4 + j];
        int32_t t2 = tmp[2 * 4 + j];
        int32_t t3 = tmp[3 * 4 + j];

        int32_t x0 = t0 + t3;
        int32_t x1 = t1 + t2;
        int32_t x2 = t1 - t2;
        int32_t x3 = t0 - t3;

        dc[0 * 4 + j] = static_cast<int16_t>(x0 + x1);
        dc[1 * 4 + j] = static_cast<int16_t>(x2 + x3);
        dc[2 * 4 + j] = static_cast<int16_t>(x0 - x1);
        dc[3 * 4 + j] = static_cast<int16_t>(x3 - x2);
    }
}

// ── 2x2 Hadamard Inverse Transform (Chroma DC) — ITU-T H.264 §8.5.12.2 ─

/** Inverse 2x2 Hadamard transform for chroma DC coefficients.
 *  §8.5.11: H2⊗H2 transform verified. [CHECKED §8.5.11]
 *  @param[in,out] dc  4 DC coefficients, transformed in-place
 */
inline void inverseHadamard2x2(int16_t* dc) noexcept
{
    // ITU-T H.264 §8.5.12.2: 2x2 Hadamard for chroma DC (4:2:0).
    // Reference: libavc ih264d_cavlc_parse_chroma_dc, lines 1385-1400.
    // The butterfly matches libavc's column-first ordering:
    //   z0=dc[0]+dc[2], z1=dc[0]-dc[2], z2=dc[1]-dc[3], z3=dc[1]+dc[3]
    //   out[0]=z0+z3, out[1]=z0-z3, out[2]=z1+z2, out[3]=z1-z2
    int32_t z0 = dc[0] + dc[2];
    int32_t z1 = dc[0] - dc[2];
    int32_t z2 = dc[1] - dc[3];
    int32_t z3 = dc[1] + dc[3];

    dc[0] = static_cast<int16_t>(z0 + z3);
    dc[1] = static_cast<int16_t>(z0 - z3);
    dc[2] = static_cast<int16_t>(z1 + z2);
    dc[3] = static_cast<int16_t>(z1 - z2);
}

// ── Inverse Quantization — ITU-T H.264 §8.5.12.1 ───────────────────────

/** Inverse quantize a 4x4 block of coefficients.
 *
 *  @param[in,out] coeffs  16 coefficients, dequantized in-place
 *  @param qp              Quantization parameter [0-51]
 *  @param isDc            True if this is a DC-only block (different scaling)
 */
inline void inverseQuantize4x4(int16_t* coeffs, int32_t qp, bool isDc = false) noexcept
{
    // Ensure QP is in valid range [0, 51] — protects against bitstream errors.
    qp = ((qp % 52) + 52) % 52;
    int32_t qpDiv6 = qp / 6;
    int32_t qpMod6 = qp % 6;

    for (uint32_t i = 0U; i < 16U; ++i)
    {
        if (coeffs[i] == 0)
            continue;

        int32_t posClass = cDequantPosClass[i];
        int32_t normAdjust = cDequantScale[qpMod6][posClass];

        if (isDc)
        {
            // DC path: not changed (used by I_16x16 Hadamard which has its own normalization)
            int32_t val = static_cast<int32_t>(coeffs[i]) * normAdjust;
            val <<= qpDiv6;
            coeffs[i] = static_cast<int16_t>(val);
            continue;
        }

        // AC path: ffmpeg-style qmul with default flat scaling matrix (weightScale=16).
        // qmul = normAdjust * weightScale << (qpDiv6 + 2)
        //      = normAdjust * 16 << (qpDiv6 + 2)
        // Then: d = (coeff * qmul + 32) >> 6
        // This produces the IDCT input which the IDCT then applies another >> 6 to.
        // Net: coeff * normAdjust * 16 << (qpDiv6+2) >> 6 >> 6 = coeff * normAdjust << (qpDiv6-6)
        // Matches the spec when combined with the weightScale.
        static constexpr int32_t cDefaultWeightScale = 16;
        int32_t qmul = normAdjust * cDefaultWeightScale << (qpDiv6 + 2);
        int32_t val = (static_cast<int32_t>(coeffs[i]) * qmul + 32) >> 6;

        coeffs[i] = static_cast<int16_t>(val);
    }
}

/** Inverse quantize a 4x4 block using a custom scaling list.
 *
 *  §8.5.12.1: with a scaling list `weightScale[i]` (raster order), the
 *  per-sample multiplier becomes `normAdjust * weightScale[i]` instead of
 *  `normAdjust * 16`. Output formula identical to the flat-scaling fast path:
 *      qmul = normAdjust * weightScale[i] << (qpDiv6 + 2)
 *      d    = (coeff * qmul + 32) >> 6
 *
 *  @param[in,out] coeffs       16 coefficients in raster order
 *  @param qp                   Quantization parameter [0-51]
 *  @param weightScaleRaster    16-entry scaling list in raster order
 */
inline void inverseQuantize4x4Scaled(int16_t* coeffs, int32_t qp,
                                      const int16_t* weightScaleRaster) noexcept
{
    qp = ((qp % 52) + 52) % 52;
    int32_t qpDiv6 = qp / 6;
    int32_t qpMod6 = qp % 6;

    for (uint32_t i = 0U; i < 16U; ++i)
    {
        if (coeffs[i] == 0)
            continue;

        int32_t posClass = cDequantPosClass[i];
        int32_t normAdjust = cDequantScale[qpMod6][posClass];
        int32_t qmul = normAdjust * weightScaleRaster[i] << (qpDiv6 + 2);
        int32_t val = (static_cast<int32_t>(coeffs[i]) * qmul + 32) >> 6;
        coeffs[i] = static_cast<int16_t>(val);
    }
}

// ── 8x8 Default dequantization scaling — ITU-T H.264 §8.5.12.1 ────────

/// Default 8x8 scaling matrix for intra blocks — ITU-T H.264 Table 7-3.
/// Flat (all 16) is the default when no scaling list is signalled.
/// Row-major 8x8 layout; indexed as [row * 8 + col].
inline constexpr std::array<uint8_t, 64> cDefaultScalingList8x8Intra = {
     6, 10, 10, 13, 11, 13, 16, 16,
    16, 16, 18, 18, 18, 18, 18, 23,
    23, 23, 23, 23, 23, 25, 25, 25,
    25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29,
    29, 29, 29, 31, 31, 31, 31, 31,
    31, 33, 33, 33, 33, 33, 36, 36,
    36, 36, 38, 38, 38, 40, 40, 42,
};

/// Default 8x8 scaling matrix for inter blocks — ITU-T H.264 Table 7-4.
inline constexpr std::array<uint8_t, 64> cDefaultScalingList8x8Inter = {
     9, 13, 13, 15, 13, 15, 17, 17,
    17, 17, 19, 19, 19, 19, 19, 21,
    21, 21, 21, 21, 21, 22, 22, 22,
    22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25,
    25, 25, 25, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 30, 30,
    30, 30, 32, 32, 32, 33, 33, 35,
};

/// Dequantization scale factors for 8x8 blocks — ITU-T H.264 Table 8-16.
/// normAdjust8x8(m, i, j) depends on (i%4, j%4) parity; 6 position classes.
///
/// Position classes (row%4, col%4):
///   class 0: both %4==0         (e.g. (0,0))
///   class 1: one %4==0, one %4==2 (e.g. (0,2), (2,0))
///   class 2: both %4==2         (e.g. (2,2))
///   class 3: one %4==0, one odd (e.g. (0,1), (1,0))
///   class 4: one %4==2, one odd (e.g. (1,2), (2,1))
///   class 5: both odd           (e.g. (1,1))
///
/// Cross-checked against JM 19.0 `dequant_coef8` (quant.h) and libavc
/// `ih264_iquant_itrans_recon_8x8` for bit-exact correctness.
inline constexpr std::array<std::array<int32_t, 6>, 6> cDequantScale8x8 = {{
    {{ 20, 25, 32, 19, 24, 18 }},  // qp%6 = 0
    {{ 22, 28, 35, 21, 26, 19 }},  // qp%6 = 1
    {{ 26, 33, 42, 24, 31, 23 }},  // qp%6 = 2
    {{ 28, 35, 45, 26, 33, 25 }},  // qp%6 = 3
    {{ 32, 40, 51, 30, 38, 28 }},  // qp%6 = 4
    {{ 36, 46, 58, 34, 43, 32 }},  // qp%6 = 5
}};

/// Position class for each raster position in an 8x8 block — §8.5.12.1.
/// Derived from (row%4, col%4) parity per Table 8-16.
inline constexpr std::array<uint8_t, 64> cDequantPosClass8x8 = []() constexpr
{
    std::array<uint8_t, 64> t{};
    for (uint32_t i = 0U; i < 64U; ++i)
    {
        uint32_t rowMod4 = (i / 8U) % 4U;
        uint32_t colMod4 = (i % 8U) % 4U;
        bool rowDivBy4 = (rowMod4 == 0U);
        bool colDivBy4 = (colMod4 == 0U);
        bool rowEven2  = (rowMod4 == 2U);
        bool colEven2  = (colMod4 == 2U);
        bool rowOdd    = ((rowMod4 & 1U) != 0U);
        bool colOdd    = ((colMod4 & 1U) != 0U);

        if (rowDivBy4 && colDivBy4)
            t[i] = 0U; // both %4==0
        else if ((rowDivBy4 && colEven2) || (rowEven2 && colDivBy4))
            t[i] = 1U; // one %4==0, one %4==2
        else if (rowEven2 && colEven2)
            t[i] = 2U; // both %4==2
        else if ((rowDivBy4 && colOdd) || (rowOdd && colDivBy4))
            t[i] = 3U; // one %4==0, one odd
        else if ((rowEven2 && colOdd) || (rowOdd && colEven2))
            t[i] = 4U; // one %4==2, one odd
        else
            t[i] = 5U; // both odd
    }
    return t;
}();

// Spot-checks against JM dequant_coef8[0] row 0 (20, 19, 25, 19, 20, 19, 25, 19).
static_assert(cDequantPosClass8x8[0]  == 0U, "pos(0,0) class 0"); // JM 20
static_assert(cDequantPosClass8x8[1]  == 3U, "pos(0,1) class 3"); // JM 19
static_assert(cDequantPosClass8x8[2]  == 1U, "pos(0,2) class 1"); // JM 25
static_assert(cDequantPosClass8x8[3]  == 3U, "pos(0,3) class 3"); // JM 19
static_assert(cDequantPosClass8x8[8]  == 3U, "pos(1,0) class 3"); // JM 19
static_assert(cDequantPosClass8x8[9]  == 5U, "pos(1,1) class 5"); // JM 18
static_assert(cDequantPosClass8x8[10] == 4U, "pos(1,2) class 4"); // JM 24
static_assert(cDequantPosClass8x8[16] == 1U, "pos(2,0) class 1"); // JM 25
static_assert(cDequantPosClass8x8[18] == 2U, "pos(2,2) class 2"); // JM 32
static_assert(cDequantScale8x8[0][0] == 20, "qp%6=0 class0 20");
static_assert(cDequantScale8x8[0][1] == 25, "qp%6=0 class1 25");
static_assert(cDequantScale8x8[0][2] == 32, "qp%6=0 class2 32");
static_assert(cDequantScale8x8[0][3] == 19, "qp%6=0 class3 19");
static_assert(cDequantScale8x8[0][4] == 24, "qp%6=0 class4 24");
static_assert(cDequantScale8x8[0][5] == 18, "qp%6=0 class5 18");

// ── 8x8 Inverse Quantization — ITU-T H.264 §8.5.12.1 ─────────────────

/** Inverse quantize an 8x8 block of coefficients (default flat scaling).
 *
 *  Uses the normAdjust8x8 scale factors from Table 8-16 with no custom
 *  scaling matrix (flat_4x4_16/flat_8x8_16). For custom scaling matrices,
 *  the caller should multiply by the scaling list entries.
 *
 *  @param[in,out] coeffs  64 coefficients in raster order, dequantized in-place
 *  @param qp              Quantization parameter [0-51]
 */
inline void inverseQuantize8x8(int16_t* coeffs, int32_t qp) noexcept
{
    // Ensure QP is in valid range [0, 51] — protects against bitstream errors.
    qp = ((qp % 52) + 52) % 52;
    int32_t qpDiv6 = qp / 6;
    int32_t qpMod6 = qp % 6;

    for (uint32_t i = 0U; i < 64U; ++i)
    {
        if (coeffs[i] == 0)
            continue;

        int32_t posClass = cDequantPosClass8x8[i];
        int32_t scale = cDequantScale8x8[qpMod6][posClass];
        int32_t val = static_cast<int32_t>(coeffs[i]) * scale;

        // ITU-T H.264 §8.5.12.1 with default flat scaling list (all 16):
        //   d = c * normAdjust8x8 * 16 << (qP/6 - 6)
        //     = c * normAdjust8x8 << (qP/6 - 2)
        // Combined with IDCT >> 12 (two passes of >>6 equivalent).
        // Verified against libavc ih264_iquant_itrans_recon_8x8 which uses:
        //   INV_QUANT: pi2_tmp[i] = (pi2_src[i] * pu2_iscale_mat[i] * g_scal_coff[u4_rem6]) >> (5 - u4_qp_div6 + shift)
        if (qpDiv6 >= 2)
            val <<= (qpDiv6 - 2);
        else
            val = (val + (1 << (1 - qpDiv6))) >> (2 - qpDiv6);

        coeffs[i] = static_cast<int16_t>(val);
    }
}

/** Inverse quantize an 8x8 block using a custom scaling list.
 *
 *  §8.5.12.1 Eq. 8-329 (8x8 residual):
 *    r[i][j] = (c * normAdjust8x8 * weightScale) << (qP/6 - 6)           if qP/6 >= 6
 *    r[i][j] = (c * normAdjust8x8 * weightScale + 2^(5-qP/6)) >> (6-qP/6) if qP/6 < 6
 *
 *  Note: the 8x8 shift exponent is -6 (not -4 as for 4x4) because the 8x8
 *  IDCT has an extra factor of 4 relative to the 4x4 in the normalisation
 *  pipeline. Matches JM `itrans8x8()` with weight-list applied.
 *
 *  @param[in,out] coeffs       64 coefficients in raster order
 *  @param qp                   Quantization parameter [0-51]
 *  @param scalingList          64-entry scaling list in raster order
 */
inline void inverseQuantize8x8Scaled(int16_t* coeffs, int32_t qp,
                                      const int16_t* scalingList) noexcept
{
    qp = ((qp % 52) + 52) % 52;
    int32_t qpDiv6 = qp / 6;
    int32_t qpMod6 = qp % 6;

    for (uint32_t i = 0U; i < 64U; ++i)
    {
        if (coeffs[i] == 0)
            continue;

        int32_t posClass = cDequantPosClass8x8[i];
        int32_t scale = cDequantScale8x8[qpMod6][posClass];
        int32_t val = static_cast<int32_t>(coeffs[i]) * scale * scalingList[i];

        if (qpDiv6 >= 6)
            val <<= (qpDiv6 - 6);
        else
            val = (val + (1 << (5 - qpDiv6))) >> (6 - qpDiv6);

        coeffs[i] = static_cast<int16_t>(val);
    }
}

// ── 8x8 Inverse Integer DCT — ITU-T H.264 §8.5.12 ────────────────────

/** Apply inverse 8x8 integer DCT and add to prediction, storing result.
 *
 *  Implements the 8x8 butterfly in ITU-T H.264 §8.5.12.2.
 *  The transform uses the even/odd decomposition with half-pel accuracy
 *  coefficients {8, 10, 12} and quarter-pel {3, 6, 9, 10}.
 *
 *  The >> 6 normalization is absorbed from the dequant scaling,
 *  matching the 4x4 convention used elsewhere in this codebase.
 *
 *  @param coeffs     Input: 64 dequantized coefficients in raster order
 *  @param pred       Input: 8x8 prediction block (stride = predStride)
 *  @param predStride Stride of prediction buffer
 *  @param out        Output: 8x8 reconstructed block (stride = outStride)
 *  @param outStride  Stride of output buffer
 */
/** 1-D 8-point inverse butterfly — ITU-T H.264 §8.5.12.2 Equations 8-325..8-333.
 *  Outputs are the unscaled sum/difference of even (e0..e3) and odd (o0..o3)
 *  half-transforms. Combine as `out[k] = e_k + o_{3-k}` for k=0..3 and
 *  `out[7-k] = e_k - o_{3-k}` for k=0..3, with the special pos 1/6 sign:
 *  `out[1] = e1 - o2`, `out[6] = e1 + o2` (matches libavc sign convention).
 *
 *  Used by both horizontal and vertical passes of inverseDct8x8AddPred.
 *  Single source of truth for the butterfly avoids drift between the two passes
 *  (h(7) bug had to be fixed in two places before this was DRY'd).
 */
struct Butterfly8 { int32_t e0, e1, e2, e3, o0, o1, o2, o3; };

inline Butterfly8 dct8Butterfly(int32_t s0, int32_t s1, int32_t s2, int32_t s3,
                                 int32_t s4, int32_t s5, int32_t s6, int32_t s7) noexcept
{
    // Even part — §8.5.12.2 Eq 8-325..8-328
    int32_t a0 = s0 + s4;
    int32_t a2 = s0 - s4;
    int32_t a4 = (s2 >> 1) - s6;
    int32_t a6 = s2 + (s6 >> 1);

    // Odd part — §8.5.12.2 Eq 8-332. h(7) uses (m(1)>>1), NOT (m(7)>>1).
    // Verified against libavc ih264_iquant_itrans_recon_8x8 (y7) and JM
    // inverse8x8() in transform.c (2026-04-17 audit).
    int32_t b0 = -s3 + s5 - s7 - (s7 >> 1);
    int32_t b1 =  s1 + s7 - s3 - (s3 >> 1);
    int32_t b2 = -s1 + s7 + s5 + (s5 >> 1);
    int32_t b3 =  s3 + s5 + s1 + (s1 >> 1);

    return {
        a0 + a6, a2 + a4, a2 - a4, a0 - a6,
        b0 + (b3 >> 2), b1 + (b2 >> 2), b2 - (b1 >> 2), b3 - (b0 >> 2)
    };
}

inline void inverseDct8x8AddPred(const int16_t* coeffs,
                                  const uint8_t* pred, uint32_t predStride,
                                  uint8_t* out, uint32_t outStride) noexcept
{
    int32_t tmp[64];

    // Horizontal 1-D 8-point inverse transform (for each row).
    for (uint32_t i = 0U; i < 8U; ++i)
    {
        const int16_t* row = coeffs + i * 8;
        Butterfly8 b = dct8Butterfly(row[0], row[1], row[2], row[3],
                                      row[4], row[5], row[6], row[7]);
        // Combine even + odd → output row. Position 1/6 use the o2 sign-swap
        // per libavc sign convention (tmp[1] = e1 - o2, tmp[6] = e1 + o2).
        tmp[i * 8 + 0] = b.e0 + b.o3;
        tmp[i * 8 + 1] = b.e1 - b.o2;
        tmp[i * 8 + 2] = b.e2 + b.o1;
        tmp[i * 8 + 3] = b.e3 + b.o0;
        tmp[i * 8 + 4] = b.e3 - b.o0;
        tmp[i * 8 + 5] = b.e2 - b.o1;
        tmp[i * 8 + 6] = b.e1 + b.o2;
        tmp[i * 8 + 7] = b.e0 - b.o3;
    }

    // Vertical 1-D 8-point inverse transform (for each column) + pred + clip.
    // Rounding bias for >>6 final shift (dequant already includes the other 6 bits).
    static constexpr int32_t cRoundBias8x8 = 32;
    for (uint32_t j = 0U; j < 8U; ++j)
    {
        Butterfly8 b = dct8Butterfly(tmp[0 * 8 + j], tmp[1 * 8 + j],
                                      tmp[2 * 8 + j], tmp[3 * 8 + j],
                                      tmp[4 * 8 + j], tmp[5 * 8 + j],
                                      tmp[6 * 8 + j], tmp[7 * 8 + j]);
        int32_t r[8] = {
            (b.e0 + b.o3 + cRoundBias8x8) >> 6,
            (b.e1 - b.o2 + cRoundBias8x8) >> 6,
            (b.e2 + b.o1 + cRoundBias8x8) >> 6,
            (b.e3 + b.o0 + cRoundBias8x8) >> 6,
            (b.e3 - b.o0 + cRoundBias8x8) >> 6,
            (b.e2 - b.o1 + cRoundBias8x8) >> 6,
            (b.e1 + b.o2 + cRoundBias8x8) >> 6,
            (b.e0 - b.o3 + cRoundBias8x8) >> 6,
        };
        for (uint32_t k = 0U; k < 8U; ++k)
            out[k * outStride + j] = static_cast<uint8_t>(clipU8(pred[k * predStride + j] + r[k]));
    }
}

/** DC-only fast path for 8x8: add constant DC value to prediction.
 *  Used when only the DC coefficient is non-zero in an 8x8 block.
 */
inline void inverseDcOnly8x8AddPred(int16_t dcCoeff,
                                     const uint8_t* pred, uint32_t predStride,
                                     uint8_t* out, uint32_t outStride) noexcept
{
    int32_t dcVal = (dcCoeff + 32) >> 6;

    for (uint32_t row = 0U; row < 8U; ++row)
    {
        for (uint32_t col = 0U; col < 8U; ++col)
        {
            out[row * outStride + col] = static_cast<uint8_t>(
                clipU8(pred[row * predStride + col] + dcVal));
        }
    }
}

} // namespace sub0h264

#endif // CROG_SUB0H264_TRANSFORM_HPP
