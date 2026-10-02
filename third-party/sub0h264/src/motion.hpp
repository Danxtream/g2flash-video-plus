/** Sub0h264 — Motion vector prediction and decoding
 *
 *  Computes motion vector predictors from spatial neighbors and
 *  reconstructs final MVs by adding decoded MVDs.
 *
 *  Reference: ITU-T H.264 §8.4.1
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_MOTION_HPP
#define CROG_SUB0H264_MOTION_HPP

#include <cstdint>
#include <algorithm>

namespace sub0h264 {

/// Motion vector (quarter-pel precision) — ITU-T H.264 §8.4.1.3.
struct MotionVector
{
    int16_t x = 0;   ///< Horizontal component (quarter-pel units)
    int16_t y = 0;   ///< Vertical component (quarter-pel units)
};

/// Per-MB motion information stored for neighbor context.
struct MbMotionInfo
{
    MotionVector mv;           ///< Motion vector (L0, after adding MVD to predictor)
    MotionVector mvd;          ///< Decoded MVD — stored for CABAC §9.3.3.1.1.7 context
    int8_t refIdx = -1;        ///< Reference frame index (-1 = intra/unavailable)
    bool available = false;    ///< True if this MB/partition is available
};

/** Compute the median of three signed values.
 *  Used for MV predictor computation — ITU-T H.264 §8.4.1.3.1.
 */
inline int16_t median3(int16_t a, int16_t b, int16_t c) noexcept
{
    int16_t minAB = std::min(a, b);
    int16_t maxAB = std::max(a, b);
    return std::max(minAB, std::min(maxAB, c));
}

/** Compute the MV predictor from three spatial neighbors.
 *
 *  §8.4.1.3.1 Step 1: When BOTH B and C are unavailable and A is available,
 *    substitute B=A, C=A. No substitution otherwise — unavailable neighbors
 *    use default mv=(0,0), refIdx=-1.
 *  §8.4.1.3.1 Step 2: If exactly one of A/B/C has matching refIdx, use that
 *    MV directly. Otherwise median of all three.
 *  [CHECKED §8.4.1.3.1]
 *
 *  [UNCHECKED §8.4.1.3.1] Directional shortcuts for 16x8/8x16 partitions
 *  not implemented here — handled at call site.
 *
 *  @param left     Left neighbor motion info (partition A)
 *  @param top      Top neighbor motion info (partition B)
 *  @param topRight Top-right neighbor motion info (partition C, or top-left D)
 *  @param refIdx   Reference index of the current partition
 *  @return Predicted motion vector
 */
inline MotionVector computeMvPredictor(const MbMotionInfo& left,
                                        const MbMotionInfo& top,
                                        const MbMotionInfo& topRight,
                                        int8_t refIdx) noexcept
{
    // §8.4.1.3.1 Step 1: substitute ONLY when both B and C are unavailable.
    // When only one is unavailable, it keeps default mv=(0,0), refIdx=-1.
    // [CHECKED §8.4.1.3.1 Eqs 8-207..8-210]
    MbMotionInfo effA = left;
    MbMotionInfo effB = top;
    MbMotionInfo effC = topRight;

    if (!effB.available && !effC.available && effA.available)
    {
        effB = effA;
        effC = effA;
    }

    // §8.4.1.3.1 Step 2: count matching refIdx among effective neighbors.
    uint32_t matchCount = 0U;
    int32_t lastMatchIdx = -1;
    if (effA.available && effA.refIdx == refIdx) { ++matchCount; lastMatchIdx = 0; }
    if (effB.available && effB.refIdx == refIdx) { ++matchCount; lastMatchIdx = 1; }
    if (effC.available && effC.refIdx == refIdx) { ++matchCount; lastMatchIdx = 2; }

    const MbMotionInfo* effNeighbors[3] = { &effA, &effB, &effC };

    // §8.4.1.3.1: If exactly one has matching refIdx → use that MV directly.
    if (matchCount == 1U)
        return effNeighbors[lastMatchIdx]->mv;

    // Otherwise: median of all three effective MVs.
    // Unavailable neighbors contribute mv=(0,0) per §8.4.1.3.2.
    int16_t mvA_x = effA.available ? effA.mv.x : 0;
    int16_t mvA_y = effA.available ? effA.mv.y : 0;
    int16_t mvB_x = effB.available ? effB.mv.x : 0;
    int16_t mvB_y = effB.available ? effB.mv.y : 0;
    int16_t mvC_x = effC.available ? effC.mv.x : 0;
    int16_t mvC_y = effC.available ? effC.mv.y : 0;

    return { median3(mvA_x, mvB_x, mvC_x), median3(mvA_y, mvB_y, mvC_y) };
}

} // namespace sub0h264

#endif // CROG_SUB0H264_MOTION_HPP
