/** Sub0h264 — Intra prediction for I-frame reconstruction
 *
 *  Implements H.264 intra prediction modes for luma (4x4, 16x16)
 *  and chroma (8x8) blocks.
 *
 *  Reference: ITU-T H.264 §8.3
 *
 *  Spec-annotated review (2026-04-09):
 *    §8.3.1.2 I_4x4: all 9 modes verified (V,H,DC,DDL,DDR,VR,HD,VL,HU) [CHECKED §8.3.1.2]
 *    §8.3.1.2 DC: 4-branch availability (both/top/left/neither) [CHECKED FM-3]
 *    §8.3.1.2 DDL/VL: top-right unavailable → replicate top[3] [CHECKED §8.3.1.2.3/§8.3.1.2.8]
 *    §8.3.1.2 DDR: requires top+left+topLeft [CHECKED §8.3.1.2.5]
 *    §8.3.3 I_16x16: V/H/DC/Plane verified [CHECKED §8.3.3]
 *    §8.3.3.4 Plane: b=(5*H+32)>>6, c=(5*V+32)>>6, center (x-7,y-7) [CHECKED §8.3.3.4]
 *    §8.3.4 Chroma: DC/H/V/Plane verified [CHECKED §8.3.4]
 *    §8.3.4.1 Chroma DC: per-4x4-quadrant availability mapping [CHECKED §8.3.4.1]
 *    §8.3.4.4 Chroma Plane: b=(17*H+16)>>5, center (x-3,y-3) [CHECKED §8.3.4.4]
 *    §8.3.2 I_8x8: all 9 modes + §8.3.2.1 reference sample filtering [CHECKED §8.3.2]
 *    Filter helpers: filt11=(a+b+1)>>1, filt121=(a+2b+c+2)>>2 [CHECKED §8.3.1.2]
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_INTRA_PRED_HPP
#define CROG_SUB0H264_INTRA_PRED_HPP

#include "frame.hpp"
#include "transform.hpp"  // clipU8

#include <cstdint>
#include <cstring>

namespace sub0h264 {

/// Intra 4x4 prediction modes — ITU-T H.264 Table 8-2.
enum class Intra4x4Mode : uint8_t
{
    Vertical        = 0U,
    Horizontal      = 1U,
    Dc              = 2U,
    DiagDownLeft    = 3U,
    DiagDownRight   = 4U,
    VerticalRight   = 5U,
    HorizontalDown  = 6U,
    VerticalLeft    = 7U,
    HorizontalUp    = 8U,
};

/// Intra 16x16 prediction modes — ITU-T H.264 Table 8-3.
enum class Intra16x16Mode : uint8_t
{
    Vertical   = 0U,
    Horizontal = 1U,
    Dc         = 2U,
    Plane      = 3U,
};

/// Intra chroma prediction modes — ITU-T H.264 Table 8-4.
enum class IntraChromaMode : uint8_t
{
    Dc         = 0U,
    Horizontal = 1U,
    Vertical   = 2U,
    Plane      = 3U,
};

/// Default prediction value when no neighbors are available — ITU-T H.264 §8.3.1.
inline constexpr uint8_t cDefaultPredValue = 128U;

// ── Filter helpers — ITU-T H.264 §8.3.1.2 ──────────────────────────────

/// 2-tap average: (a + b + 1) >> 1 — used in §8.3.1.2 prediction equations.
inline constexpr uint8_t filt11(uint8_t a, uint8_t b) noexcept
{
    return static_cast<uint8_t>((a + b + 1U) >> 1U);
}

/// 3-tap weighted: (a + 2*b + c + 2) >> 2 — used in §8.3.1.2 prediction equations.
inline constexpr uint8_t filt121(uint8_t a, uint8_t b, uint8_t c) noexcept
{
    return static_cast<uint8_t>((a + 2U * b + c + 2U) >> 2U);
}

// ── Intra 4x4 prediction — ITU-T H.264 §8.3.1.2 ───────────────────────

/** Generate a 4x4 intra prediction block.
 *
 *  @param mode      Prediction mode [0-8]
 *  @param top       4 pixels above the block (nullptr if unavailable)
 *  @param topRight  4 pixels to the top-right (nullptr if unavailable)
 *  @param left      4 pixels to the left (nullptr if unavailable)
 *  @param topLeft   Top-left pixel (nullptr if unavailable)
 *  @param[out] pred Output 4x4 prediction block (stride=4)
 */
inline void intraPred4x4(Intra4x4Mode mode,
                          const uint8_t* top, const uint8_t* topRight,
                          const uint8_t* left, const uint8_t* topLeft,
                          uint8_t* pred) noexcept
{
    switch (mode)
    {
    case Intra4x4Mode::Vertical:
        if (top)
        {
            for (uint32_t row = 0U; row < 4U; ++row)
                std::memcpy(pred + row * 4U, top, 4U);
        }
        break;

    case Intra4x4Mode::Horizontal:
        if (left)
        {
            for (uint32_t row = 0U; row < 4U; ++row)
                std::memset(pred + row * 4U, left[row], 4U);
        }
        break;

    case Intra4x4Mode::Dc:
    {
        uint32_t sum = 0U;
        uint32_t count = 0U;
        if (top)  { for (uint32_t i = 0U; i < 4U; ++i) sum += top[i];  count += 4U; }
        if (left) { for (uint32_t i = 0U; i < 4U; ++i) sum += left[i]; count += 4U; }

        uint8_t val = cDefaultPredValue;
        if (count == 8U) val = static_cast<uint8_t>((sum + 4U) >> 3U);
        else if (count == 4U) val = static_cast<uint8_t>((sum + 2U) >> 2U);

        std::memset(pred, val, 16U);
        break;
    }

    case Intra4x4Mode::DiagDownLeft:
        if (top)
        {
            // ITU-T H.264 §8.3.1.2.3: when top-right unavailable,
            // substitute all 4 samples with top[3] (last top pixel).
            uint8_t trBuf[4] = { top[3], top[3], top[3], top[3] };
            const uint8_t* tr = topRight ? topRight : trBuf;
            uint8_t t[8] = { top[0], top[1], top[2], top[3],
                             tr[0], tr[1], tr[2], tr[3] };
            for (uint32_t row = 0U; row < 4U; ++row)
                for (uint32_t col = 0U; col < 4U; ++col)
                {
                    uint32_t idx = row + col;
                    pred[row * 4U + col] = (idx == 6U)
                        ? filt121(t[6], t[7], t[7])
                        : filt121(t[idx], t[idx + 1U], t[idx + 2U]);
                }
        }
        break;

    case Intra4x4Mode::DiagDownRight:
        if (top && left && topLeft)
        {
            // ITU-T H.264 §8.3.1.2.5: DiagDownRight
            // pred[y][x] = filt121(p[d-1], p[d], p[d+1]) where d = x - y
            // p values indexed by diagonal offset d:
            //   d = -3: left[3]    d = -2: left[2]    d = -1: left[1]
            //   d =  0: left[0]/tl/top[0]  (center = filt121(left[0], tl, top[0]))
            //   d = +1: tl/top[0]/top[1]   d = +2: top[0]/top[1]/top[2]
            //   d = +3: top[1]/top[2]/top[3]
            // Build array: p[d] for d = -4..-1,0,+1..+3
            //   idx: 0=left[3], 1=left[2], 2=left[1], 3=left[0],
            //        4=topLeft, 5=top[0], 6=top[1], 7=top[2], 8=top[3]
            uint8_t tl = *topLeft;
            uint8_t p[9] = { left[3], left[2], left[1], left[0],
                             tl, top[0], top[1], top[2], top[3] };
            // pred[y][x] = filt121(p[d-1], p[d], p[d+1])
            // where d_idx = 4 + d = 4 + (x - y), range [1..7]
            for (uint32_t row = 0U; row < 4U; ++row)
                for (uint32_t col = 0U; col < 4U; ++col)
                {
                    int32_t d = static_cast<int32_t>(col) - static_cast<int32_t>(row);
                    uint32_t di = static_cast<uint32_t>(4 + d); // index into p[]
                    pred[row * 4U + col] = filt121(p[di - 1U], p[di], p[di + 1U]);
                }
        }
        break;

    case Intra4x4Mode::VerticalRight:
        if (top && left && topLeft)
        {
            uint8_t tl = *topLeft;
            // ITU-T H.264 §8.3.1.2.6
            pred[0*4+0] = filt11(tl, top[0]);
            pred[0*4+1] = filt11(top[0], top[1]);
            pred[0*4+2] = filt11(top[1], top[2]);
            pred[0*4+3] = filt11(top[2], top[3]);
            pred[1*4+0] = filt121(left[0], tl, top[0]);
            pred[1*4+1] = filt121(tl, top[0], top[1]);
            pred[1*4+2] = filt121(top[0], top[1], top[2]);
            pred[1*4+3] = filt121(top[1], top[2], top[3]);
            pred[2*4+0] = filt121(left[1], left[0], tl);
            pred[2*4+1] = pred[0*4+0];
            pred[2*4+2] = pred[0*4+1];
            pred[2*4+3] = pred[0*4+2];
            pred[3*4+0] = filt121(left[2], left[1], left[0]);
            pred[3*4+1] = pred[1*4+0];
            pred[3*4+2] = pred[1*4+1];
            pred[3*4+3] = pred[1*4+2];
        }
        break;

    case Intra4x4Mode::HorizontalDown:
        if (top && left && topLeft)
        {
            uint8_t tl = *topLeft;
            // ITU-T H.264 §8.3.1.2.7
            pred[0*4+0] = filt11(tl, left[0]);
            pred[0*4+1] = filt121(left[0], tl, top[0]);
            pred[0*4+2] = filt121(tl, top[0], top[1]);
            pred[0*4+3] = filt121(top[0], top[1], top[2]);
            pred[1*4+0] = filt11(left[0], left[1]);
            pred[1*4+1] = filt121(tl, left[0], left[1]);
            pred[1*4+2] = pred[0*4+0];
            pred[1*4+3] = pred[0*4+1];
            pred[2*4+0] = filt11(left[1], left[2]);
            pred[2*4+1] = filt121(left[0], left[1], left[2]);
            pred[2*4+2] = pred[1*4+0];
            pred[2*4+3] = pred[1*4+1];
            pred[3*4+0] = filt11(left[2], left[3]);
            pred[3*4+1] = filt121(left[1], left[2], left[3]);
            pred[3*4+2] = pred[2*4+0];
            pred[3*4+3] = pred[2*4+1];
        }
        break;

    case Intra4x4Mode::VerticalLeft:
        if (top)
        {
            // ITU-T H.264 §8.3.1.2.8: when top-right unavailable,
            // substitute all 4 samples with top[3].
            uint8_t trBuf[4] = { top[3], top[3], top[3], top[3] };
            const uint8_t* tr = topRight ? topRight : trBuf;
            uint8_t t[8] = { top[0], top[1], top[2], top[3], tr[0], tr[1], tr[2], tr[3] };
            // ITU-T H.264 §8.3.1.2.8
            pred[0*4+0] = filt11(t[0], t[1]);
            pred[0*4+1] = filt11(t[1], t[2]);
            pred[0*4+2] = filt11(t[2], t[3]);
            pred[0*4+3] = filt11(t[3], t[4]);
            pred[1*4+0] = filt121(t[0], t[1], t[2]);
            pred[1*4+1] = filt121(t[1], t[2], t[3]);
            pred[1*4+2] = filt121(t[2], t[3], t[4]);
            pred[1*4+3] = filt121(t[3], t[4], t[5]);
            pred[2*4+0] = filt11(t[1], t[2]);
            pred[2*4+1] = filt11(t[2], t[3]);
            pred[2*4+2] = filt11(t[3], t[4]);
            pred[2*4+3] = filt11(t[4], t[5]);
            pred[3*4+0] = filt121(t[1], t[2], t[3]);
            pred[3*4+1] = filt121(t[2], t[3], t[4]);
            pred[3*4+2] = filt121(t[3], t[4], t[5]);
            pred[3*4+3] = filt121(t[4], t[5], t[6]);
        }
        break;

    case Intra4x4Mode::HorizontalUp:
        if (left)
        {
            // ITU-T H.264 §8.3.1.2.9
            pred[0*4+0] = filt11(left[0], left[1]);
            pred[0*4+1] = filt121(left[0], left[1], left[2]);
            pred[0*4+2] = filt11(left[1], left[2]);
            pred[0*4+3] = filt121(left[1], left[2], left[3]);
            pred[1*4+0] = pred[0*4+2];
            pred[1*4+1] = pred[0*4+3];
            pred[1*4+2] = filt11(left[2], left[3]);
            pred[1*4+3] = filt121(left[2], left[3], left[3]);
            pred[2*4+0] = pred[1*4+2];
            pred[2*4+1] = pred[1*4+3];
            pred[2*4+2] = left[3];
            pred[2*4+3] = left[3];
            pred[3*4+0] = left[3];
            pred[3*4+1] = left[3];
            pred[3*4+2] = left[3];
            pred[3*4+3] = left[3];
        }
        break;

    default:
        std::memset(pred, cDefaultPredValue, 16U);
        break;
    }
}

// ── Intra 16x16 prediction — ITU-T H.264 §8.3.3 ───────────────────────

/** Generate a 16x16 intra prediction block.
 *
 *  @param mode       Prediction mode [0-3]
 *  @param frame      Frame containing reconstructed neighbors
 *  @param mbX, mbY   Macroblock position
 *  @param[out] pred  Output 16x16 prediction block (stride=16)
 */
inline void intraPred16x16(Intra16x16Mode mode,
                            const Frame& frame, uint32_t mbX, uint32_t mbY,
                            uint8_t* pred) noexcept
{
    uint32_t pixX = mbX * cMbSize;
    uint32_t pixY = mbY * cMbSize;
    bool hasTop  = (mbY > 0U);
    bool hasLeft = (mbX > 0U);

    switch (mode)
    {
    case Intra16x16Mode::Vertical:
        if (hasTop)
        {
            const uint8_t* topRow = frame.yRow(pixY - 1U) + pixX;
            for (uint32_t row = 0U; row < 16U; ++row)
                std::memcpy(pred + row * 16U, topRow, 16U);
        }
        break;

    case Intra16x16Mode::Horizontal:
        if (hasLeft)
        {
            for (uint32_t row = 0U; row < 16U; ++row)
            {
                uint8_t leftPix = frame.y(pixX - 1U, pixY + row);
                std::memset(pred + row * 16U, leftPix, 16U);
            }
        }
        break;

    case Intra16x16Mode::Dc:
    {
        uint32_t sum = 0U;
        uint32_t count = 0U;
        if (hasTop)
        {
            const uint8_t* topRow = frame.yRow(pixY - 1U) + pixX;
            for (uint32_t i = 0U; i < 16U; ++i) sum += topRow[i];
            count += 16U;
        }
        if (hasLeft)
        {
            for (uint32_t i = 0U; i < 16U; ++i) sum += frame.y(pixX - 1U, pixY + i);
            count += 16U;
        }

        uint8_t val = cDefaultPredValue;
        if (count == 32U) val = static_cast<uint8_t>((sum + 16U) >> 5U);
        else if (count == 16U) val = static_cast<uint8_t>((sum + 8U) >> 4U);

        std::memset(pred, val, 256U);
        break;
    }

    case Intra16x16Mode::Plane:
        if (hasTop && hasLeft)
        {
            const uint8_t* topRow = frame.yRow(pixY - 1U) + pixX;

            int32_t h = 0, v = 0;
            for (int32_t i = 0; i < 8; ++i)
            {
                h += (i + 1) * (topRow[8 + i] - topRow[6 - i]);
                v += (i + 1) * (static_cast<int32_t>(frame.y(pixX - 1U, pixY + 8U + i))
                              - static_cast<int32_t>(frame.y(pixX - 1U, pixY + 6U - i)));
            }

            int32_t a = 16 * (static_cast<int32_t>(frame.y(pixX - 1U, pixY + 15U)) + topRow[15]);
            int32_t b = (5 * h + 32) >> 6;
            int32_t c = (5 * v + 32) >> 6;

            for (uint32_t row = 0U; row < 16U; ++row)
            {
                for (uint32_t col = 0U; col < 16U; ++col)
                {
                    int32_t val = (a + b * (static_cast<int32_t>(col) - 7)
                                     + c * (static_cast<int32_t>(row) - 7) + 16) >> 5;
                    pred[row * 16U + col] = static_cast<uint8_t>(clipU8(val));
                }
            }
        }
        break;
    }
}

// ── Intra chroma prediction (8x8) — ITU-T H.264 §8.3.4 ────────────────

/** Generate an 8x8 intra chroma prediction block.
 *
 *  @param mode       Prediction mode [0-3]
 *  @param frame      Frame containing reconstructed chroma neighbors
 *  @param mbX, mbY   Macroblock position
 *  @param isU        True for U (Cb) plane, false for V (Cr)
 *  @param[out] pred  Output 8x8 prediction block (stride=8)
 */
inline void intraPredChroma8x8(IntraChromaMode mode,
                                const Frame& frame, uint32_t mbX, uint32_t mbY,
                                bool isU, uint8_t* pred) noexcept
{
    uint32_t pixX = mbX * cChromaBlockSize;
    uint32_t pixY = mbY * cChromaBlockSize;
    bool hasTop  = (mbY > 0U);
    bool hasLeft = (mbX > 0U);

    auto getPixel = [&](uint32_t x, uint32_t y) -> uint8_t {
        return isU ? frame.u(x, y) : frame.v(x, y);
    };
    auto getRow = [&](uint32_t y) -> const uint8_t* {
        return isU ? frame.uRow(y) : frame.vRow(y);
    };

    switch (mode)
    {
    case IntraChromaMode::Dc:
    {
        // ITU-T H.264 §8.3.4.1: chroma DC prediction for 4:2:0.
        // Each 4x4 quadrant uses DIFFERENT neighbor combinations:
        //   (0,0) top-left:     top[0..3] + left[0..3] → avg of 8
        //   (1,0) top-right:    top[4..7] only          → avg of 4
        //   (0,1) bottom-left:  left[4..7] only         → avg of 4
        //   (1,1) bottom-right: top[4..7] + left[4..7]  → avg of 8
        // When top or left is unavailable, adapt accordingly.
        // Reference: libavc ih264d_process_intra_mb.c, chroma DC prediction.
        for (uint32_t qy = 0U; qy < 2U; ++qy)
        {
            for (uint32_t qx = 0U; qx < 2U; ++qx)
            {
                // Determine which neighbors this quadrant uses
                bool useTop  = hasTop  && (qx == 0U || qy == 1U || !hasLeft);
                bool useLeft = hasLeft && (qy == 0U || qx == 1U || !hasTop);

                // Simplified: always use top for the same-row quadrant,
                // always use left for the same-column quadrant.
                // Per §8.3.4.1: block (qx, qy) uses:
                //   top  if qy==0 (top row) or both available for (1,1)
                //   left if qx==0 (left col) or both available for (1,1)
                // Actually the spec rule is simpler:
                //   block (0,0): both if available
                //   block (1,0): top only (left is "not the same column")
                //   block (0,1): left only (top is "not the same row")
                //   block (1,1): both if available

                useTop  = hasTop  && (qx == qy || qy == 0U);  // (0,0), (1,0), (1,1)
                useLeft = hasLeft && (qx == qy || qx == 0U);  // (0,0), (0,1), (1,1)

                // Fallback: if neither applies, use whatever is available
                if (!useTop && !useLeft)
                {
                    useTop = hasTop;
                    useLeft = hasLeft;
                }

                uint32_t sum = 0U;
                uint32_t count = 0U;

                if (useTop)
                {
                    const uint8_t* topRow = getRow(pixY - 1U);
                    for (uint32_t i = 0U; i < 4U; ++i)
                        sum += topRow[pixX + qx * 4U + i];
                    count += 4U;
                }
                if (useLeft)
                {
                    for (uint32_t i = 0U; i < 4U; ++i)
                        sum += getPixel(pixX - 1U, pixY + qy * 4U + i);
                    count += 4U;
                }

                uint8_t val = cDefaultPredValue;
                if (count == 8U) val = static_cast<uint8_t>((sum + 4U) >> 3U);
                else if (count == 4U) val = static_cast<uint8_t>((sum + 2U) >> 2U);

                for (uint32_t row = 0U; row < 4U; ++row)
                    std::memset(pred + (qy * 4U + row) * 8U + qx * 4U, val, 4U);
            }
        }
        break;
    }

    case IntraChromaMode::Horizontal:
        if (hasLeft)
        {
            for (uint32_t row = 0U; row < 8U; ++row)
            {
                uint8_t leftPix = getPixel(pixX - 1U, pixY + row);
                std::memset(pred + row * 8U, leftPix, 8U);
            }
        }
        break;

    case IntraChromaMode::Vertical:
        if (hasTop)
        {
            const uint8_t* topRow = getRow(pixY - 1U) + pixX;
            for (uint32_t row = 0U; row < 8U; ++row)
                std::memcpy(pred + row * 8U, topRow, 8U);
        }
        break;

    case IntraChromaMode::Plane:
        if (hasTop && hasLeft)
        {
            const uint8_t* topRow = getRow(pixY - 1U) + pixX;
            int32_t h = 0, v = 0;
            for (int32_t i = 0; i < 4; ++i)
            {
                h += (i + 1) * (topRow[4 + i] - topRow[2 - i]);
                v += (i + 1) * (static_cast<int32_t>(getPixel(pixX - 1U, pixY + 4U + i))
                              - static_cast<int32_t>(getPixel(pixX - 1U, pixY + 2U - i)));
            }

            int32_t a = 16 * (static_cast<int32_t>(getPixel(pixX - 1U, pixY + 7U)) + topRow[7]);
            int32_t b = (17 * h + 16) >> 5;
            int32_t c = (17 * v + 16) >> 5;

            for (uint32_t row = 0U; row < 8U; ++row)
            {
                for (uint32_t col = 0U; col < 8U; ++col)
                {
                    int32_t val = (a + b * (static_cast<int32_t>(col) - 3)
                                     + c * (static_cast<int32_t>(row) - 3) + 16) >> 5;
                    pred[row * 8U + col] = static_cast<uint8_t>(clipU8(val));
                }
            }
        }
        break;
    }
}

// ── Intra 8x8 prediction — ITU-T H.264 §8.3.2 ─────────────────────────

/** Generate an 8x8 intra prediction block.
 *
 *  Uses the same 9 modes as 4x4 (§8.3.2.2) but on 8x8 blocks.
 *  §8.3.2.1 defines reference sample filtering before prediction.
 *
 *  @param mode      Prediction mode [0-8] (same enum as Intra4x4Mode)
 *  @param frame     Current frame (for reading neighbor pixels)
 *  @param absX      Absolute X position of block's top-left pixel
 *  @param absY      Absolute Y position of block's top-left pixel
 *  @param[out] pred Output 8x8 prediction block (stride=8)
 */
inline void intraPred8x8Luma(Intra4x4Mode mode, const Frame& frame,
                              uint32_t absX, uint32_t absY,
                              uint8_t* pred) noexcept
{
    // §8.3.2.1: Gather reference samples p[-1,-1], p[0..-1..7,-1], p[-1,0..7]
    // and optionally p[8..15,-1] for top-right extension.
    bool hasTop  = (absY > 0U);
    bool hasLeft = (absX > 0U);
    bool hasTL   = hasTop && hasLeft;

    // §6.4.10.4 / Table 6-3: Top-right availability for I_8x8 blocks in raster
    // scan order within an MB depends on block position (blockX, blockY) in
    // {0, 8} × {0, 8}:
    //   block 0 (TL, 0,0): top-right from MB(X, Y-1) or MB(X+1, Y-1) — decoded
    //   block 1 (TR, 8,0): top-right from MB(X+1, Y-1) — decoded
    //   block 2 (BL, 0,8): top-right is block 1 of current MB — decoded
    //   block 3 (BR, 8,8): top-right would be MB(X+1, Y), NOT YET DECODED
    // Per spec, block 3 top-right is unavailable; replicate top[7] (p[7,-1]).
    const uint32_t blockX = absX & 0x08U;
    const uint32_t blockY = absY & 0x08U;
    const bool topRightAvailable = !(blockX != 0U && blockY != 0U);

    uint8_t top[16] = {};   // p[0..15, -1] (8 top + 8 top-right)
    uint8_t left[8] = {};   // p[-1, 0..7]
    uint8_t tl = cDefaultPredValue; // p[-1, -1]

    if (hasTop)
    {
        const uint8_t* row = frame.yRow(absY - 1U);
        for (uint32_t c = 0U; c < 8U; ++c)
            top[c] = row[absX + c];
        // Top-right: p[8..15, -1]. Unavailable for block 3 (BR of MB) since
        // MB to the right hasn't been decoded yet. Also replicate at right
        // frame edge.
        for (uint32_t c = 8U; c < 16U; ++c)
            top[c] = (topRightAvailable && absX + c < frame.width())
                        ? row[absX + c] : top[7];
    }
    if (hasLeft)
        for (uint32_t r = 0U; r < 8U; ++r)
            left[r] = frame.y(absX - 1U, absY + r);
    if (hasTL)
        tl = frame.y(absX - 1U, absY - 1U);

    // §8.3.2.1: Reference sample filtering (low-pass for smoother prediction).
    // p'[-1,-1] = (p[-1,0] + 2*p[-1,-1] + p[0,-1] + 2) >> 2
    // p'[x,-1]  = (p[x-1,-1] + 2*p[x,-1] + p[x+1,-1] + 2) >> 2  for x=0..6
    // p'[7,-1]  = (p[6,-1] + 2*p[7,-1] + p[8,-1] + 2) >> 2
    // Similar for left column.
    uint8_t ft[16] = {}, fl[8] = {}, ftl;
    (void)ftl; // Used by diagonal modes (DDR, VR, HD) — currently DC fallback

    if (hasTop && hasLeft)
        ftl = filt121(left[0], tl, top[0]);
    else if (hasTop)
        ftl = top[0];
    else if (hasLeft)
        ftl = left[0];
    else
        ftl = cDefaultPredValue;

    if (hasTop)
    {
        ft[0] = hasTL ? filt121(tl, top[0], top[1]) : filt121(top[0], top[0], top[1]);
        for (uint32_t c = 1U; c < 15U; ++c)
            ft[c] = filt121(top[c - 1U], top[c], top[c + 1U]);
        ft[15] = filt121(top[14], top[15], top[15]);
    }

    if (hasLeft)
    {
        fl[0] = hasTL ? filt121(tl, left[0], left[1]) : filt121(left[0], left[0], left[1]);
        for (uint32_t r = 1U; r < 7U; ++r)
            fl[r] = filt121(left[r - 1U], left[r], left[r + 1U]);
        fl[7] = filt121(left[6], left[7], left[7]);
    }

    switch (mode)
    {
    case Intra4x4Mode::Vertical: // §8.3.2.2.2
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
                pred[r * 8U + c] = hasTop ? ft[c] : cDefaultPredValue;
        break;

    case Intra4x4Mode::Horizontal: // §8.3.2.2.3
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
                pred[r * 8U + c] = hasLeft ? fl[r] : cDefaultPredValue;
        break;

    case Intra4x4Mode::Dc: // §8.3.2.2.4
    {
        uint32_t sum = 0U, count = 0U;
        if (hasTop)  { for (uint32_t c = 0U; c < 8U; ++c) { sum += ft[c]; ++count; } }
        if (hasLeft) { for (uint32_t r = 0U; r < 8U; ++r) { sum += fl[r]; ++count; } }
        uint8_t dc = (count > 0U)
            ? static_cast<uint8_t>((sum + count / 2U) / count) : cDefaultPredValue;
        std::memset(pred, dc, 64U);
        break;
    }

    case Intra4x4Mode::DiagDownLeft: // §8.3.2.2.5
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
            {
                uint32_t idx = r + c;
                pred[r * 8U + c] = (idx == 14U)
                    ? filt121(ft[14], ft[15], ft[15])
                    : filt121(ft[idx], ft[idx + 1U], ft[idx + 2U]);
            }
        break;

    case Intra4x4Mode::DiagDownRight: // §8.3.2.2.6
        // pred[r][c] where d = c - r:
        //   d > 0: filt121(ft[d-2], ft[d-1], ft[d])  — use ftl for ft[-1]
        //   d == 0: filt121(fl[0], ftl, ft[0])
        //   d < 0: filt121(fl[-d-2], fl[-d-1], fl[-d]) — use ftl for fl[-1]...fl[0]
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
            {
                int32_t d = static_cast<int32_t>(c) - static_cast<int32_t>(r);
                if (d > 0)
                {
                    uint8_t a = (d >= 2) ? ft[d - 2] : (d == 1 ? ftl : fl[0]);
                    pred[r * 8U + c] = filt121(a, ft[d - 1], ft[d]);
                }
                else if (d == 0)
                    pred[r * 8U + c] = filt121(fl[0], ftl, ft[0]);
                else // d < 0, i.e. r > c
                {
                    uint32_t md = static_cast<uint32_t>(-d);
                    uint8_t a = (md >= 2U) ? fl[md - 2U] : ftl;
                    uint8_t b = fl[md - 1U];
                    pred[r * 8U + c] = filt121(a, b, md < 8U ? fl[md] : fl[7]);
                }
            }
        break;

    case Intra4x4Mode::VerticalRight: // §8.3.2.2.7
    {
        // extTop[i] = p[i-1, -1]: extTop[0]=ftl, extTop[1]=ft[0], ..., extTop[8]=ft[7]
        uint8_t extTop[9] = {ftl, ft[0], ft[1], ft[2], ft[3], ft[4], ft[5], ft[6], ft[7]};
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
            {
                int32_t zVR = 2 * static_cast<int32_t>(c) - static_cast<int32_t>(r);
                uint32_t k = c - (r >> 1U); // index into extTop for centre sample
                if (zVR >= 0 && (zVR & 1) == 0)
                    // Eq 8-115: pred = (p[x-(y>>1)-1,-1] + p[x-(y>>1),-1] + 1) >> 1
                    pred[r * 8U + c] = filt11(extTop[k], extTop[k + 1U]);
                else if (zVR >= 0)
                    // Eq 8-117: pred = (p[x-(y>>1)-2,-1] + 2*p[x-(y>>1)-1,-1] + p[x-(y>>1),-1] + 2) >> 2
                    pred[r * 8U + c] = filt121(extTop[k - 1U], extTop[k], extTop[k + 1U]);
                else if (zVR == -1)
                    // Eq 8-116: pred = (p[-1,0] + 2*p[-1,-1] + p[0,-1] + 2) >> 2.
                    // Constant value for ALL zVR==-1 positions: (0,1), (1,3), (2,5), (3,7).
                    pred[r * 8U + c] = filt121(fl[0], ftl, ft[0]);
                else
                {
                    // Eq 8-118 (zVR ∈ {-2,-3,-4,-5,-6,-7}):
                    //   pred = (p[-1, i] + 2*p[-1, i-1] + p[-1, i-2] + 2) >> 2
                    //   where i = y - 2x - 1, so samples are taken from the
                    //   left column. Verified against JM intra8x8_pred.c L999-1014
                    //   PredArray[0..2] / [11..13] mapping.
                    int32_t i = static_cast<int32_t>(r) - 2 * static_cast<int32_t>(c) - 1;
                    uint8_t a  = (i >= 0) ? fl[i] : ftl;
                    uint8_t b  = (i >= 1) ? fl[i - 1] : ftl;
                    uint8_t cv = (i >= 2) ? fl[i - 2] : ftl;
                    pred[r * 8U + c] = filt121(a, b, cv);
                }
            }
        break;
    }

    case Intra4x4Mode::HorizontalDown: // §8.3.2.2.8
    {
        // Mirror of VerticalRight with r/c swapped.
        uint8_t extLeft[9] = {ftl, fl[0], fl[1], fl[2], fl[3], fl[4], fl[5], fl[6], fl[7]};
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
            {
                int32_t zHD = 2 * static_cast<int32_t>(r) - static_cast<int32_t>(c);
                uint32_t k = r - (c >> 1U);
                if (zHD >= 0 && (zHD & 1) == 0)
                    pred[r * 8U + c] = filt11(extLeft[k], extLeft[k + 1U]);
                else if (zHD >= 0)
                    pred[r * 8U + c] = filt121(extLeft[k - 1U], extLeft[k], extLeft[k + 1U]);
                else if (zHD == -1)
                    // Mirror of VR Eq 8-116: constant for all zHD==-1 positions
                    // (1,0), (3,1), (5,2), (7,3).
                    pred[r * 8U + c] = filt121(ft[0], ftl, fl[0]);
                else
                {
                    // zHD ∈ {-2..-7}: mirror of VR negative branch — use top
                    // row indexed by i = x - 2y - 1.
                    int32_t i = static_cast<int32_t>(c) - 2 * static_cast<int32_t>(r) - 1;
                    uint8_t a  = (i >= 0) ? ft[i] : ftl;
                    uint8_t b  = (i >= 1) ? ft[i - 1] : ftl;
                    uint8_t cv = (i >= 2) ? ft[i - 2] : ftl;
                    pred[r * 8U + c] = filt121(a, b, cv);
                }
            }
        break;
    }

    case Intra4x4Mode::VerticalLeft: // §8.3.2.2.9
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
            {
                uint32_t idx = c + (r >> 1);
                if ((r & 1U) == 0U)
                    pred[r * 8U + c] = filt11(ft[idx], ft[idx + 1U]);
                else
                    pred[r * 8U + c] = filt121(ft[idx], ft[idx + 1U], ft[idx + 2U]);
            }
        break;

    case Intra4x4Mode::HorizontalUp: // §8.3.2.2.10
        // Spec convention: zHU = x + 2*y where x=col, y=row.
        // - zHU even, < 13: filt11(p[-1, y+(x>>1)], p[-1, y+(x>>1)+1])
        // - zHU odd,  < 13: filt121(p[-1, y+(x>>1)], p[-1, y+(x>>1)+1], p[-1, y+(x>>1)+2])
        // - zHU == 13:      filt121(p[-1, 6], p[-1, 7], p[-1, 7])
        // - zHU >  13:      p[-1, 7]
        // Verified vs JM intra8x8_hor_up_pred PredArray[0..21] mapping
        // (intra8x8_pred.c L1306-1336).
        for (uint32_t r = 0U; r < 8U; ++r)
            for (uint32_t c = 0U; c < 8U; ++c)
            {
                uint32_t zHU = c + 2U * r;
                uint32_t zi = r + (c >> 1U); // = (zHU >> 1) with x>>1 semantics
                if (zHU < 13U && (zHU & 1U) == 0U)
                    pred[r * 8U + c] = filt11(fl[zi], fl[zi + 1U]);
                else if (zHU < 13U) // odd
                    pred[r * 8U + c] = filt121(fl[zi], fl[zi + 1U], fl[zi + 2U]);
                else if (zHU == 13U)
                    pred[r * 8U + c] = filt121(fl[6], fl[7], fl[7]);
                else
                    pred[r * 8U + c] = fl[7];
            }
        break;
    }
}

} // namespace sub0h264

#endif // CROG_SUB0H264_INTRA_PRED_HPP
