/** Sub0h264 — Inter prediction (motion compensation)
 *
 *  Implements H.264 luma and chroma interpolation filters for
 *  fractional-pel motion compensation in P-frames.
 *
 *  Reference: ITU-T H.264 §8.4.2
 *
 *  Spec-annotated review (2026-04-09):
 *    §8.4.2.2.1 Luma MC: 6-tap {1,-5,20,20,-5,1}, (sum+16)>>5 [CHECKED §8.4.2.2.1]
 *    §8.4.2.2.1 All 16 quarter-pel positions per Table 8-12 [CHECKED Table 8-12]
 *    §8.4.2.2.1 j (2D): vertical 6-tap on horizontal intermediates, (sum+512)>>10 [CHECKED §8.4.2.2.1]
 *    §8.4.2.2.2 Chroma MC: bilinear weights (8-dx)(8-dy), (sum+32)>>6 [CHECKED §8.4.2.2.2]
 *    §8.4.2.3.1 Weighted pred: ((w*val+round)>>logWD)+offset, Clip1 [CHECKED §8.4.2.3.1]
 *    FM-21: Edge clamping via getSample (clamp to [0, width-1]) [CHECKED FM-21]
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_INTER_PRED_HPP
#define CROG_SUB0H264_INTER_PRED_HPP

#include "frame.hpp"
#include "motion.hpp"
#include "transform.hpp" // clipU8
#include "luma_mve.hpp"

#include <cstdint>
#include <algorithm>

namespace sub0h264 {

/// 6-tap FIR filter coefficients for luma half-pel — ITU-T H.264 §8.4.2.2.1.
/// Filter: {1, -5, 20, 20, -5, 1} / 32
inline constexpr int32_t cLumaFilter6Tap[6] = { 1, -5, 20, 20, -5, 1 };

/** Apply 6-tap luma horizontal filter for half-pel position.
 *  Output = (src[-2] - 5*src[-1] + 20*src[0] + 20*src[1] - 5*src[2] + src[3] + 16) >> 5
 */
inline int32_t lumaHFilter(const uint8_t* src) noexcept
{
    return src[-2] - 5 * src[-1] + 20 * src[0] + 20 * src[1] - 5 * src[2] + src[3];
}

/** Apply 6-tap luma vertical filter for half-pel position. */
inline int32_t lumaVFilter(const uint8_t* src, uint32_t stride) noexcept
{
    return src[-2 * static_cast<int32_t>(stride)]
         - 5 * src[-1 * static_cast<int32_t>(stride)]
         + 20 * src[0]
         + 20 * src[1 * stride]
         - 5 * src[2 * stride]
         + src[3 * stride];
}

/** Six-tap numerator; half-pel intermediates must not be clipped for 2-D. */
inline int32_t lumaMotionCompFir(int32_t a, int32_t b, int32_t c,
                                  int32_t d, int32_t e, int32_t f) noexcept
{
    return a + f - 5 * (b + e) + 20 * (c + d);
}

/** Interpolate a 4/8/16-wide luma partition with edge replication. */
inline void lumaMotionComp(const Frame& ref,
                            int32_t refX, int32_t refY,
                            uint32_t dx, uint32_t dy,
                            uint32_t width, uint32_t height,
                            uint8_t* dst, uint32_t dstStride) noexcept
{
    const int32_t frameWidth = ref.width(), frameHeight = ref.height();
    if (dx == 0U && dy == 0U && refX >= 0 && refY >= 0 &&
        refX + static_cast<int32_t>(width) <= frameWidth &&
        refY + static_cast<int32_t>(height) <= frameHeight)
    {
        for (uint32_t row = 0U; row < height; ++row)
            std::memcpy(dst + row * dstStride, ref.yRow(refY + row) + refX, width);
        return;
    }

    constexpr uint32_t scratchStride = 21U; // Largest partition plus six-tap halo.
    uint8_t scratch[scratchStride * scratchStride];
    const uint8_t* src;
    uint32_t stride;
    if (refX >= 2 && refY >= 2 &&
        refX + static_cast<int32_t>(width) + 3 <= frameWidth &&
        refY + static_cast<int32_t>(height) + 3 <= frameHeight)
    {
        src = ref.yRow(refY) + refX;
        stride = ref.yStride();
    }
    else
    {
        for (uint32_t row = 0U; row < height + 5U; ++row)
        {
            const int32_t y = std::clamp(refY + static_cast<int32_t>(row) - 2, 0, frameHeight - 1);
            const uint8_t* source = ref.yRow(y);
            for (uint32_t col = 0U; col < width + 5U; ++col)
            {
                const int32_t x = std::clamp(refX + static_cast<int32_t>(col) - 2, 0, frameWidth - 1);
                scratch[row * scratchStride + col] = source[x];
            }
        }
        src = scratch + 2U * scratchStride + 2U;
        stride = scratchStride;
    }
    if (dx == 0U && dy == 0U)
    {
        for (uint32_t row = 0U; row < height; ++row)
            std::memcpy(dst + row * dstStride, src + row * stride, width);
        return;
    }

#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE & 1)
    lumaMotionCompMve(src, stride, dx, dy, width, height, dst, dstStride);
#else
    const int32_t step = static_cast<int32_t>(stride);
    if (dx == 0U || dy == 0U || ((dx & 1U) && (dy & 1U)))
    {
        // Axial fractions and odd/odd diagonals need no centre half-pel.
        for (uint32_t row = 0U; row < height; ++row)
        {
            const uint8_t* p = src + row * stride;
            for (uint32_t col = 0U; col < width; ++col)
            {
                int32_t value;
                if (dy == 0U)
                {
                    value = clipU8((lumaHFilter(p + col) + 16) >> 5);
                    if (dx != 2U) value = (value + p[col + (dx >> 1U)] + 1) >> 1;
                }
                else if (dx == 0U)
                {
                    value = clipU8((lumaVFilter(p + col, stride) + 16) >> 5);
                    if (dy != 2U) value = (value + p[(dy >> 1U) * stride + col] + 1) >> 1;
                }
                else
                {
                    int32_t h = clipU8((lumaHFilter(p + (dy >> 1U) * stride + col) + 16) >> 5);
                    int32_t v = clipU8((lumaVFilter(p + col + (dx >> 1U), stride) + 16) >> 5);
                    value = (h + v + 1) >> 1;
                }
                dst[row * dstStride + col] = static_cast<uint8_t>(value);
            }
        }
        return;
    }

    // Reuse each horizontal numerator across six output rows. Its range is
    // [-2550, 10710], so int16_t is exact; the vertical sum needs int32_t.
    int16_t horizontal[scratchStride][16];
    for (uint32_t row = 0U; row < height + 5U; ++row)
    {
        const uint8_t* p = src + (static_cast<int32_t>(row) - 2) * step;
        for (uint32_t col = 0U; col < width; ++col)
            horizontal[row][col] = static_cast<int16_t>(lumaHFilter(p + col));
    }
    for (uint32_t row = 0U; row < height; ++row)
    {
        for (uint32_t col = 0U; col < width; ++col)
        {
            int32_t sum = lumaMotionCompFir(horizontal[row][col], horizontal[row + 1U][col],
                horizontal[row + 2U][col], horizontal[row + 3U][col],
                horizontal[row + 4U][col], horizontal[row + 5U][col]);
            int32_t value = clipU8((sum + 512) >> 10);
            if (dy != 2U)
            {
                int32_t h = clipU8((horizontal[row + 2U + (dy >> 1U)][col] + 16) >> 5);
                value = (value + h + 1) >> 1;
            }
            else if (dx != 2U)
            {
                int32_t v = clipU8((lumaVFilter(src + row * stride + col + (dx >> 1U), stride) + 16) >> 5);
                value = (value + v + 1) >> 1;
            }
            dst[row * dstStride + col] = static_cast<uint8_t>(value);
        }
    }
#endif
}

/** Perform chroma bilinear motion compensation.
 *
 *  @param ref       Reference frame
 *  @param refX      Integer-pel X position in chroma plane
 *  @param refY      Integer-pel Y position in chroma plane
 *  @param dx        Horizontal fraction (0-7, eighth-pel)
 *  @param dy        Vertical fraction (0-7, eighth-pel)
 *  @param width     Block width in chroma pixels
 *  @param height    Block height in chroma pixels
 *  @param isU       True for U plane, false for V
 *  @param[out] dst  Destination buffer
 *  @param dstStride Destination stride
 *
 *  Reference: ITU-T H.264 §8.4.2.2.2
 */
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
inline void chromaMotionComp(const Frame& ref,
                              int32_t refX, int32_t refY,
                              uint32_t dx, uint32_t dy,
                              uint32_t width, uint32_t height,
                              bool isU, uint8_t* dst, uint32_t dstStride) noexcept
{
    uint32_t chromaW = ref.width() / 2U;
    uint32_t chromaH = ref.height() / 2U;

    auto getSample = [&](int32_t x, int32_t y) -> uint8_t {
        int32_t maxCx = static_cast<int32_t>(chromaW) - 1;
        int32_t maxCy = static_cast<int32_t>(chromaH) - 1;
        x = (x < 0) ? 0 : (x > maxCx ? maxCx : x);
        y = (y < 0) ? 0 : (y > maxCy ? maxCy : y);
        return isU ? ref.u(static_cast<uint32_t>(x), static_cast<uint32_t>(y))
                   : ref.v(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    };

    /// Chroma filter weights — ITU-T H.264 §8.4.2.2.2 Eq. 8-258.
    uint32_t w00 = (8U - dx) * (8U - dy);
    uint32_t w10 = dx * (8U - dy);
    uint32_t w01 = (8U - dx) * dy;
    uint32_t w11 = dx * dy;

    // Fast path: block + 1-pixel margin fully within chroma plane bounds.
    // Eliminates 4×6 branches per pixel (getSample clamping).
    if (refX >= 0 && refY >= 0 &&
        refX + static_cast<int32_t>(width) + 1 <= static_cast<int32_t>(chromaW) &&
        refY + static_cast<int32_t>(height) + 1 <= static_cast<int32_t>(chromaH))
    {
        uint32_t uvStride = ref.uvStride();
        for (uint32_t row = 0U; row < height; ++row)
        {
            const uint8_t* r0 = isU
                ? (ref.uRow(static_cast<uint32_t>(refY + row)) + static_cast<uint32_t>(refX))
                : (ref.vRow(static_cast<uint32_t>(refY + row)) + static_cast<uint32_t>(refX));
            const uint8_t* r1 = r0 + uvStride;
            for (uint32_t col = 0U; col < width; ++col)
            {
                uint32_t val = (w00 * r0[col] + w10 * r0[col + 1U]
                              + w01 * r1[col] + w11 * r1[col + 1U] + 32U) >> 6U;
                dst[row * dstStride + col] = static_cast<uint8_t>(val);
            }
        }
    }
    else
    {
        for (uint32_t row = 0U; row < height; ++row)
        {
            for (uint32_t col = 0U; col < width; ++col)
            {
                uint32_t a = getSample(refX + col, refY + row);
                uint32_t b = getSample(refX + col + 1, refY + row);
                uint32_t c = getSample(refX + col, refY + row + 1);
                uint32_t d = getSample(refX + col + 1, refY + row + 1);

                uint32_t val = (w00 * a + w10 * b + w01 * c + w11 * d + 32U) >> 6U;
                dst[row * dstStride + col] = static_cast<uint8_t>(val);
            }
        }
    }
}
#endif

/** Apply explicit weighted prediction to a prediction block — §8.4.2.3.1.
 *
 *  predWeighted[x][y] = Clip1Y( ((w * pred[x][y] + 2^(logWD-1)) >> logWD) + offset )
 *
 *  When the weight flag is false, the prediction is used as-is (identity).
 *
 *  @param pred       Prediction samples (modified in place)
 *  @param stride     Stride of the prediction buffer
 *  @param width      Block width in samples
 *  @param height     Block height in samples
 *  @param logWD      log2_weight_denom (luma or chroma)
 *  @param weight     Weight value for this ref_idx
 *  @param offset     Offset value for this ref_idx
 *  @param weightFlag True if explicit weight was signaled
 */
inline void applyWeightedPred(uint8_t* pred, uint32_t stride,
                               uint32_t width, uint32_t height,
                               uint32_t logWD, int16_t weight,
                               int16_t offset, bool weightFlag) noexcept
{
    if (!weightFlag)
        return; // Default weight — identity, no change needed

    // §8.4.2.3.1: round = 2^(logWD - 1) for logWD >= 1, else 0
    int32_t round = (logWD >= 1U) ? (1 << (logWD - 1U)) : 0;

    for (uint32_t row = 0U; row < height; ++row)
    {
        for (uint32_t col = 0U; col < width; ++col)
        {
            int32_t val = pred[row * stride + col];
            if (logWD >= 1U)
                val = ((weight * val + round) >> logWD) + offset;
            else
                val = weight * val + offset;
            // Clip to [0, 255]
            if (val < 0) val = 0;
            if (val > 255) val = 255;
            pred[row * stride + col] = static_cast<uint8_t>(val);
        }
    }
}

} // namespace sub0h264

#endif // CROG_SUB0H264_INTER_PRED_HPP
