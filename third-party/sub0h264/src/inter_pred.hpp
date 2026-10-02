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

/** Perform luma motion compensation for a rectangular block.
 *
 *  Handles all 16 fractional-pel positions (dx,dy ∈ {0,1,2,3}).
 *  dx,dy represent quarter-pel offsets.
 *
 *  @param ref       Reference frame
 *  @param refX      Integer-pel X position in reference
 *  @param refY      Integer-pel Y position in reference
 *  @param dx        Horizontal fractional offset (0-3, quarter-pel)
 *  @param dy        Vertical fractional offset (0-3, quarter-pel)
 *  @param width     Block width in pixels
 *  @param height    Block height in pixels
 *  @param[out] dst  Destination buffer
 *  @param dstStride Destination stride
 */
inline void lumaMotionComp(const Frame& ref,
                            int32_t refX, int32_t refY,
                            uint32_t dx, uint32_t dy,
                            uint32_t width, uint32_t height,
                            uint8_t* dst, uint32_t dstStride) noexcept
{
    // Clamp reference position to frame bounds with margin for filter taps.
    // Used by the integer / half-pel fast-path slow fallbacks (which read
    // directly from ref). The L3.1 prefetch block below introduces a
    // separate `getSample` lambda that reads from a stack scratch buffer.
    auto getSampleRef = [&](int32_t x, int32_t y) -> uint8_t {
        int32_t maxX = static_cast<int32_t>(ref.width()) - 1;
        int32_t maxY = static_cast<int32_t>(ref.height()) - 1;
        x = (x < 0) ? 0 : (x > maxX ? maxX : x);
        y = (y < 0) ? 0 : (y > maxY ? maxY : y);
        return ref.y(static_cast<uint32_t>(x), static_cast<uint32_t>(y));
    };

    if (dx == 0U && dy == 0U)
    {
        // Full-pel copy — fast path when block is fully within frame bounds.
        // Avoids per-pixel clamping (~6 branches per pixel eliminated).
        if (refX >= 0 && refY >= 0 &&
            refX + static_cast<int32_t>(width) <= static_cast<int32_t>(ref.width()) &&
            refY + static_cast<int32_t>(height) <= static_cast<int32_t>(ref.height()))
        {
            uint32_t ux = static_cast<uint32_t>(refX);
            uint32_t uy = static_cast<uint32_t>(refY);
            uint32_t refStride = ref.yStride();
            const uint8_t* refRow = ref.yRow(uy) + ux;
            for (uint32_t row = 0U; row < height; ++row)
            {
                std::memcpy(dst + row * dstStride, refRow, width);
                refRow += refStride;
            }
        }
        else
        {
            for (uint32_t row = 0U; row < height; ++row)
                for (uint32_t col = 0U; col < width; ++col)
                    dst[row * dstStride + col] = getSampleRef(refX + col, refY + row);
        }
        return;
    }

    if (dx == 0U && dy == 2U)
    {
        // Vertical half-pel — fast path when 6-tap filter window is in bounds
        if (refX >= 0 && refY >= 2 &&
            refX + static_cast<int32_t>(width) <= static_cast<int32_t>(ref.width()) &&
            refY + static_cast<int32_t>(height) + 3 <= static_cast<int32_t>(ref.height()))
        {
            // Cache 6 row pointers for the filter tap window.
            // On ESP32-P4 with PSRAM, each yRow() call touches a different
            // cache line. Pre-caching the pointers ensures the address
            // arithmetic happens once, not per-column.
            uint32_t stride = ref.yStride();
            uint32_t ux = static_cast<uint32_t>(refX);
            const uint8_t* baseRow = ref.yRow(static_cast<uint32_t>(refY) - 2U) + ux;
            for (uint32_t row = 0U; row < height; ++row)
            {
                const uint8_t* r0 = baseRow;
                const uint8_t* r1 = r0 + stride;
                const uint8_t* r2 = r1 + stride;
                const uint8_t* r3 = r2 + stride;
                const uint8_t* r4 = r3 + stride;
                const uint8_t* r5 = r4 + stride;
                for (uint32_t col = 0U; col < width; ++col)
                {
                    int32_t sum = r0[col] - 5*r1[col] + 20*r2[col]
                                + 20*r3[col] - 5*r4[col] + r5[col];
                    dst[row * dstStride + col] = static_cast<uint8_t>(clipU8((sum + 16) >> 5));
                }
                baseRow += stride;
            }
        }
        else
        {
            for (uint32_t row = 0U; row < height; ++row)
                for (uint32_t col = 0U; col < width; ++col)
                {
                    int32_t sum = 0;
                    for (int32_t k = -2; k <= 3; ++k)
                        sum += cLumaFilter6Tap[k + 2] * getSampleRef(refX + col, refY + row + k);
                    dst[row * dstStride + col] = static_cast<uint8_t>(clipU8((sum + 16) >> 5));
                }
        }
        return;
    }

    if (dx == 2U && dy == 0U)
    {
        // Horizontal half-pel — fast path when 6-tap filter window is in bounds
        if (refX >= 2 && refY >= 0 &&
            refX + static_cast<int32_t>(width) + 3 <= static_cast<int32_t>(ref.width()) &&
            refY + static_cast<int32_t>(height) <= static_cast<int32_t>(ref.height()))
        {
            for (uint32_t row = 0U; row < height; ++row)
            {
                const uint8_t* p = ref.yRow(static_cast<uint32_t>(refY + row))
                                 + static_cast<uint32_t>(refX) - 2U;
                for (uint32_t col = 0U; col < width; ++col)
                {
                    int32_t sum = p[0] - 5*p[1] + 20*p[2] + 20*p[3] - 5*p[4] + p[5];
                    dst[row * dstStride + col] = static_cast<uint8_t>(clipU8((sum + 16) >> 5));
                    ++p;
                }
            }
        }
        else
        {
            for (uint32_t row = 0U; row < height; ++row)
                for (uint32_t col = 0U; col < width; ++col)
                {
                    int32_t sum = 0;
                    for (int32_t k = -2; k <= 3; ++k)
                        sum += cLumaFilter6Tap[k + 2] * getSampleRef(refX + col + k, refY + row);
                    dst[row * dstStride + col] = static_cast<uint8_t>(clipU8((sum + 16) >> 5));
                }
        }
        return;
    }

    // ─── L3.1 reference prefetch ────────────────────────────────────────
    // All remaining positions (quarter-pel + 9 diagonal cases) call
    // getSample 6–36× per output pixel. On ESP32-P4 each call lands in
    // PSRAM. Copy the (width+5) × (height+5) reference window into a
    // stack scratch buffer once, then shadow getSample to read from L1-
    // resident memory for the remainder of this function.
    //
    // Layout: scratch[(y - refY + 2) * 21 + (x - refX + 2)] = ref(x, y)
    // clamped to frame bounds. The +2 offset reserves the 6-tap filter's
    // upper-left margin (taps -2 .. +3 each side).
    //
    // See docs/optimization/opportunities/L3.1_reference_prefetch.md.
    {  // open inner scope so the new getSample lambda shadows the outer one
    constexpr uint32_t kScratchStride = 21U;  // max (16 + 5) for 16x16 block
    alignas(16) uint8_t scratch[kScratchStride * kScratchStride];
    {
        const int32_t startX = refX - 2;
        const int32_t startY = refY - 2;
        const int32_t maxX = static_cast<int32_t>(ref.width()) - 1;
        const int32_t maxY = static_cast<int32_t>(ref.height()) - 1;
        const uint32_t w5 = width + 5U;
        const uint32_t h5 = height + 5U;

        if (startX >= 0 && startY >= 0 &&
            startX + static_cast<int32_t>(w5) <= static_cast<int32_t>(ref.width()) &&
            startY + static_cast<int32_t>(h5) <= static_cast<int32_t>(ref.height()))
        {
            // Common case: window fully in-bounds → row-wise memcpy.
            const uint8_t* srcRow = ref.yRow(static_cast<uint32_t>(startY))
                                  + static_cast<uint32_t>(startX);
            const uint32_t srcStride = ref.yStride();
            for (uint32_t row = 0U; row < h5; ++row)
            {
                std::memcpy(scratch + row * kScratchStride, srcRow, w5);
                srcRow += srcStride;
            }
        }
        else
        {
            // Edge case: clamp coordinates per pixel during the fill.
            for (uint32_t row = 0U; row < h5; ++row)
            {
                int32_t y = startY + static_cast<int32_t>(row);
                y = (y < 0) ? 0 : (y > maxY ? maxY : y);
                const uint8_t* srcRow = ref.yRow(static_cast<uint32_t>(y));
                for (uint32_t col = 0U; col < w5; ++col)
                {
                    int32_t x = startX + static_cast<int32_t>(col);
                    x = (x < 0) ? 0 : (x > maxX ? maxX : x);
                    scratch[row * kScratchStride + col] = srcRow[x];
                }
            }
        }
    }

    // Shadow the outer getSample lambda. From this point on, every
    // call (including those captured by hFilter / vFilterClip / j2D
    // below) reads from the L1-resident scratch buffer.
    auto getSample = [&](int32_t x, int32_t y) -> uint8_t {
        const int32_t sx = x - refX + 2;
        const int32_t sy = y - refY + 2;
        // Internal callers stay within the (-2, -2) .. (w+2, h+2) window
        // by construction (6-tap filter range). Defensive clamp guards
        // against unforeseen callers and accepts a 1-cycle hit.
        const int32_t sxC = (sx < 0) ? 0 :
            (sx >= static_cast<int32_t>(kScratchStride) ? static_cast<int32_t>(kScratchStride) - 1 : sx);
        const int32_t syC = (sy < 0) ? 0 :
            (sy >= static_cast<int32_t>(kScratchStride) ? static_cast<int32_t>(kScratchStride) - 1 : sy);
        return scratch[syC * kScratchStride + sxC];
    };

    // Quarter-pel horizontal (dx=1 or 3, dy=0):
    // a=(G+b+1)>>1 for dx=1, c=(H+b+1)>>1 for dx=3. §8.4.2.2.1
    if (dy == 0U)
    {
        for (uint32_t row = 0U; row < height; ++row)
            for (uint32_t col = 0U; col < width; ++col)
            {
                // b = horizontal half-pel between col and col+1
                int32_t sum = 0;
                for (int32_t k = -2; k <= 3; ++k)
                    sum += cLumaFilter6Tap[k + 2] * getSample(refX + col + k, refY + row);
                int32_t b = clipU8((sum + 16) >> 5);
                // Integer sample: G for dx=1, H (col+1) for dx=3
                int32_t full = getSample(refX + col + (dx == 3U ? 1 : 0), refY + row);
                dst[row * dstStride + col] = static_cast<uint8_t>((full + b + 1) >> 1);
            }
        return;
    }

    // Quarter-pel vertical (dx=0, dy=1 or 3):
    // d=(G+h+1)>>1 for dy=1, n=(J+h+1)>>1 for dy=3. §8.4.2.2.1
    if (dx == 0U)
    {
        for (uint32_t row = 0U; row < height; ++row)
            for (uint32_t col = 0U; col < width; ++col)
            {
                // h = vertical half-pel between row and row+1
                int32_t sum = 0;
                for (int32_t k = -2; k <= 3; ++k)
                    sum += cLumaFilter6Tap[k + 2] * getSample(refX + col, refY + row + k);
                int32_t h = clipU8((sum + 16) >> 5);
                // Integer sample: G for dy=1, J (row+1) for dy=3
                int32_t full = getSample(refX + col, refY + row + (dy == 3U ? 1 : 0));
                dst[row * dstStride + col] = static_cast<uint8_t>((full + h + 1) >> 1);
            }
        return;
    }

    // ── Diagonal fractional positions (dx!=0, dy!=0) ────────────────
    // ITU-T H.264 §8.4.2.2.1: requires computing intermediate half-pel
    // values b, h, j, m, s and then averaging per Table 8-12.

    // Helper: horizontal 6-tap producing UNCLIPPED intermediate (for j computation).
    auto hFilter = [&](int32_t x, int32_t y) -> int32_t {
        int32_t sum = 0;
        for (int32_t k = -2; k <= 3; ++k)
            sum += cLumaFilter6Tap[k + 2] * getSample(x + k, y);
        return sum; // NOT clipped — intermediate for 2D filter
    };

    // Helper: horizontal 6-tap, clipped (for b, s values).
    auto hFilterClip = [&](int32_t x, int32_t y) -> int32_t {
        return clipU8((hFilter(x, y) + 16) >> 5);
    };

    // Helper: vertical 6-tap, clipped (for h, m values).
    auto vFilterClip = [&](int32_t x, int32_t y) -> int32_t {
        int32_t sum = 0;
        for (int32_t k = -2; k <= 3; ++k)
            sum += cLumaFilter6Tap[k + 2] * getSample(x, y + k);
        return clipU8((sum + 16) >> 5);
    };

    // Helper: j = 2D 6-tap at (x, y). Vertical 6-tap on horizontal intermediates.
    // §8.4.2.2.1: j = clip((vFilter(hFilter_unclipped) + 512) >> 10)
    auto j2D = [&](int32_t x, int32_t y) -> int32_t {
        int32_t sum = 0;
        for (int32_t k = -2; k <= 3; ++k)
            sum += cLumaFilter6Tap[k + 2] * hFilter(x, y + k);
        return clipU8((sum + 512) >> 10);
    };

    // Compute per-pixel based on fractional position — Table 8-12:
    //   (1,1)=e: (b+h+1)>>1     (2,1)=f: (b+j+1)>>1     (3,1)=g: (b+m+1)>>1
    //   (1,2)=i: (h+j+1)>>1     (2,2)=j: 2D filter       (3,2)=k: (j+m+1)>>1
    //   (1,3)=p: (h+s+1)>>1     (2,3)=q: (j+s+1)>>1     (3,3)=r: (m+s+1)>>1
    // Where: b=hHalf@row, h=vHalf@col, j=2D@(col,row),
    //        m=vHalf@(col+1), s=hHalf@(row+1)
    for (uint32_t row = 0U; row < height; ++row)
    {
        for (uint32_t col = 0U; col < width; ++col)
        {
            int32_t x = refX + static_cast<int32_t>(col);
            int32_t y = refY + static_cast<int32_t>(row);
            int32_t val;

            if (dx == 2U && dy == 2U)
            {
                // j = 2D 6-tap — §8.4.2.2.1 Eq. 8-239/8-240
                val = j2D(x, y);
            }
            else if (dx == 1U && dy == 1U)
            {
                // e = (b + h + 1) >> 1
                val = (hFilterClip(x, y) + vFilterClip(x, y) + 1) >> 1;
            }
            else if (dx == 3U && dy == 1U)
            {
                // g = (b + m + 1) >> 1 — m is vertical half at col+1
                val = (hFilterClip(x, y) + vFilterClip(x + 1, y) + 1) >> 1;
            }
            else if (dx == 1U && dy == 3U)
            {
                // p = (h + s + 1) >> 1 — s is horizontal half at row+1
                val = (vFilterClip(x, y) + hFilterClip(x, y + 1) + 1) >> 1;
            }
            else if (dx == 3U && dy == 3U)
            {
                // r = (m + s + 1) >> 1
                val = (vFilterClip(x + 1, y) + hFilterClip(x, y + 1) + 1) >> 1;
            }
            else if (dx == 2U && dy == 1U)
            {
                // f = (b + j + 1) >> 1
                val = (hFilterClip(x, y) + j2D(x, y) + 1) >> 1;
            }
            else if (dx == 2U && dy == 3U)
            {
                // q = (j + s + 1) >> 1
                val = (j2D(x, y) + hFilterClip(x, y + 1) + 1) >> 1;
            }
            else if (dx == 1U && dy == 2U)
            {
                // i = (h + j + 1) >> 1
                val = (vFilterClip(x, y) + j2D(x, y) + 1) >> 1;
            }
            else // dx == 3U && dy == 2U
            {
                // k = (j + m + 1) >> 1
                val = (j2D(x, y) + vFilterClip(x + 1, y) + 1) >> 1;
            }

            dst[row * dstStride + col] = static_cast<uint8_t>(val);
        }
    }
    }  // close L3.1 prefetch+slow-path scope (shadowed getSample ends here)
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
