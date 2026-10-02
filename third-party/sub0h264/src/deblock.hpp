/** Sub0h264 — H.264 in-loop deblocking filter
 *
 *  Implements the deblocking filter for removing blocking artifacts
 *  at macroblock and 4x4 block boundaries. Both luma and chroma.
 *
 *  Reference: ITU-T H.264 §8.7
 *
 *  Spec-annotated review (2026-04-09):
 *    §8.7.2.1 BS derivation: intra+MB=4, intra+internal=3, coeff=2,
 *             ref/MV diff=1, else=0. [CHECKED §8.7.2.1]
 *    §8.7.2.3 Weak filter (BS<4): delta formula, tc=tc0+ap+aq,
 *             optional p1/q1 when ap/aq<beta. [CHECKED §8.7.2.3]
 *    §8.7.2.4 Strong filter (BS=4): strong threshold |p0-q0|<(alpha>>2)+2,
 *             3-tap P/Q with fallback. [CHECKED §8.7.2.4]
 *    §8.7.2.2 QP averaging: (qpP+qpQ+1)>>1 at boundaries. [CHECKED §8.7.2.2]
 *    §8.7 Filter order: vertical edges first, then horizontal. [CHECKED §8.7]
 *    FM-22: BS precomputed per-MB before filter application. [CHECKED FM-22]
 *    FM-10: Chroma uses QPc from cChromaQpTable, not luma QP. [CHECKED FM-10]
 *    Tables: Alpha, Beta, tc0 verified monotonic + boundary. [CHECKED FM-14]
 *
 *  KNOWN ISSUE: P-frame chroma deblocking may produce different results
 *  from libavc at horizontal MB boundaries in row 2+. Likely downstream
 *  of the CABAC coefficient decode bug (Phase 5 mb_type fix may help).
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_DEBLOCK_HPP
#define CROG_SUB0H264_DEBLOCK_HPP

#include "frame.hpp"
#include "motion.hpp"    // MbMotionInfo
#include "tables.hpp"    // cChromaQpTable
#include "transform.hpp" // clipU8, clampQpIdx

#include <cstdint>
#include <cstdlib> // abs
#include <algorithm>

namespace sub0h264 {

// ── Deblocking tables — ITU-T H.264 Table 8-16, 8-17 ───────────────────

/// Alpha threshold table indexed by indexA [0-51].
inline constexpr uint8_t cAlphaTable[52] = {
     0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,
     4,   4,   5,   6,   7,   8,   9,  10,
    12,  13,  15,  17,  20,  22,  25,  28,
    32,  36,  40,  45,  50,  56,  63,  71,
    80,  90, 101, 113, 127, 144, 162, 182,
   203, 226, 255, 255,
};

/// Beta threshold table indexed by indexB [0-51].
inline constexpr uint8_t cBetaTable[52] = {
     0,   0,   0,   0,   0,   0,   0,   0,
     0,   0,   0,   0,   0,   0,   0,   0,
     2,   2,   2,   3,   3,   3,   3,   4,
     4,   4,   6,   6,   7,   7,   8,   8,
     9,   9,  10,  10,  11,  11,  12,  12,
    13,  13,  14,  14,  15,  15,  16,  16,
    17,  17,  18,  18,
};

/// tc0 clipping table indexed by [indexA][bS-1] (bS 1-3).
/// Column 0 is unused (bS=0 means no filtering).
inline constexpr uint8_t cTc0Table[52][4] = {
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0}, { 0, 0, 0, 0},
    { 0, 0, 0, 0}, { 0, 0, 0, 1}, { 0, 0, 0, 1}, { 0, 0, 0, 1},
    { 0, 0, 0, 1}, { 0, 0, 1, 1}, { 0, 0, 1, 1}, { 0, 1, 1, 1},
    { 0, 1, 1, 1}, { 0, 1, 1, 1}, { 0, 1, 1, 1}, { 0, 1, 1, 2},
    { 0, 1, 1, 2}, { 0, 1, 1, 2}, { 0, 1, 1, 2}, { 0, 1, 2, 3},
    { 0, 1, 2, 3}, { 0, 2, 2, 3}, { 0, 2, 2, 4}, { 0, 2, 3, 4},
    { 0, 2, 3, 4}, { 0, 3, 3, 5}, { 0, 3, 4, 6}, { 0, 3, 4, 6},
    { 0, 4, 5, 7}, { 0, 4, 5, 8}, { 0, 4, 6, 9}, { 0, 5, 7,10},
    { 0, 6, 8,11}, { 0, 6, 8,13}, { 0, 7,10,14}, { 0, 8,11,16},
    { 0, 9,12,18}, { 0,10,13,20}, { 0,11,15,23}, { 0,13,17,25},
};

// ── Boundary strength ───────────────────────────────────────────────────

/** Compute boundary strength for a vertical or horizontal edge.
 *
 *  Reference: ITU-T H.264 §8.7.2.1 Table 8-15.
 *
 *  @param isIntraP  True if the p-side MB is intra
 *  @param isIntraQ  True if the q-side MB is intra
 *  @param isMbEdge  True if this is a MB boundary (not internal 4x4 edge)
 *  @param hasCoeffP True if p-side 4x4 block has non-zero coefficients
 *  @param hasCoeffQ True if q-side 4x4 block has non-zero coefficients
 *  @param mvPx,mvPy Motion vector of p-side (quarter-pel)
 *  @param mvQx,mvQy Motion vector of q-side (quarter-pel)
 *  @param refP,refQ Reference indices of p-side and q-side
 *  @return Boundary strength [0-4]
 */
inline uint8_t computeBs(bool isIntraP, bool isIntraQ, bool isMbEdge,
                          bool hasCoeffP, bool hasCoeffQ,
                          int16_t mvPx, int16_t mvPy, int16_t mvQx, int16_t mvQy,
                          int8_t refP, int8_t refQ) noexcept
{
    // §8.7.2.1: BS=4 for intra MB at MB boundary
    if (isMbEdge && (isIntraP || isIntraQ))
        return 4U;

    // §8.7.2.1: BS=3 for intra MB at internal 4x4 boundary
    if (isIntraP || isIntraQ)
        return 3U;

    // §8.7.2.1: BS=2 if either block has non-zero transform coefficients
    if (hasCoeffP || hasCoeffQ)
        return 2U;

    // §8.7.2.1: BS=1 if different reference or |MV_diff| >= 4 quarter-pel
    if (refP != refQ)
        return 1U;
    if (std::abs(mvPx - mvQx) >= 4 || std::abs(mvPy - mvQy) >= 4)
        return 1U;

    return 0U;
}

// ── Filter kernels ──────────────────────────────────────────────────────

/** Apply weak luma filter (BS=1-3) to one pixel crossing.
 *
 *  Modifies p0, q0 (and optionally p1, q1 for luma).
 *  Reference: ITU-T H.264 §8.7.2.3 (filtering for BS < 4).
 */
inline void filterLumaWeak(uint8_t& p0, uint8_t& p1, const uint8_t& p2,
                            uint8_t& q0, uint8_t& q1, const uint8_t& q2,
                            int32_t alpha, int32_t beta, int32_t tc0) noexcept
{
    int32_t ip0 = p0, ip1 = p1, ip2 = p2;
    int32_t iq0 = q0, iq1 = q1, iq2 = q2;

    // Decision: filter?
    if (std::abs(ip0 - iq0) >= alpha) return;
    if (std::abs(iq1 - iq0) >= beta) return;
    if (std::abs(ip1 - ip0) >= beta) return;

    int32_t ap = std::abs(ip2 - ip0);
    int32_t aq = std::abs(iq2 - iq0);

    int32_t tc = tc0 + (ap < beta ? 1 : 0) + (aq < beta ? 1 : 0);

    // Delta for p0/q0
    int32_t delta = ((((iq0 - ip0) << 2) + (ip1 - iq1) + 4) >> 3);
    delta = std::max(-tc, std::min(tc, delta));

    p0 = static_cast<uint8_t>(clipU8(ip0 + delta));
    q0 = static_cast<uint8_t>(clipU8(iq0 - delta));

    // Optional p1/q1 filtering (luma only)
    if (ap < beta)
    {
        int32_t dp1 = ((ip2 + ((ip0 + iq0 + 1) >> 1) - (ip1 << 1)) >> 1);
        dp1 = std::max(-tc0, std::min(tc0, dp1));
        p1 = static_cast<uint8_t>(clipU8(ip1 + dp1));
    }
    if (aq < beta)
    {
        int32_t dq1 = ((iq2 + ((ip0 + iq0 + 1) >> 1) - (iq1 << 1)) >> 1);
        dq1 = std::max(-tc0, std::min(tc0, dq1));
        q1 = static_cast<uint8_t>(clipU8(iq1 + dq1));
    }
}

/** Apply strong luma filter (BS=4) to one pixel crossing.
 *  Reference: ITU-T H.264 §8.7.2.4 (filtering for BS = 4).
 */
inline void filterLumaStrong(uint8_t& p0, uint8_t& p1, uint8_t& p2,
                              uint8_t& q0, uint8_t& q1, uint8_t& q2,
                              const uint8_t& p3, const uint8_t& q3,
                              int32_t alpha, int32_t beta) noexcept
{
    int32_t ip0 = p0, ip1 = p1, ip2 = p2, ip3 = p3;
    int32_t iq0 = q0, iq1 = q1, iq2 = q2, iq3 = q3;

    if (std::abs(ip0 - iq0) >= alpha) return;
    if (std::abs(iq1 - iq0) >= beta) return;
    if (std::abs(ip1 - ip0) >= beta) return;

    // §8.7.2.4: strong filter condition — |p0 - q0| < (alpha >> 2) + 2
    bool strongThresh = (std::abs(ip0 - iq0) < ((alpha >> 2) + 2));
    int32_t ap = std::abs(ip2 - ip0);
    int32_t aq = std::abs(iq2 - iq0);

    if (strongThresh)
    {
        // P-side
        if (ap < beta)
        {
            p0 = static_cast<uint8_t>((ip2 + 2 * ip1 + 2 * ip0 + 2 * iq0 + iq1 + 4) >> 3);
            p1 = static_cast<uint8_t>((ip2 + ip1 + ip0 + iq0 + 2) >> 2);
            p2 = static_cast<uint8_t>((2 * ip3 + 3 * ip2 + ip1 + ip0 + iq0 + 4) >> 3);
        }
        else
        {
            p0 = static_cast<uint8_t>((2 * ip1 + ip0 + iq1 + 2) >> 2);
        }

        // Q-side
        if (aq < beta)
        {
            q0 = static_cast<uint8_t>((ip1 + 2 * ip0 + 2 * iq0 + 2 * iq1 + iq2 + 4) >> 3);
            q1 = static_cast<uint8_t>((ip0 + iq0 + iq1 + iq2 + 2) >> 2);
            q2 = static_cast<uint8_t>((2 * iq3 + 3 * iq2 + iq1 + iq0 + ip0 + 4) >> 3);
        }
        else
        {
            q0 = static_cast<uint8_t>((2 * iq1 + iq0 + ip1 + 2) >> 2);
        }
    }
    else
    {
        p0 = static_cast<uint8_t>((2 * ip1 + ip0 + iq1 + 2) >> 2);
        q0 = static_cast<uint8_t>((2 * iq1 + iq0 + ip1 + 2) >> 2);
    }
}

/** Apply chroma filter (BS=1-3, weak) to one pixel crossing.
 *  Reference: ITU-T H.264 §8.7.2.3 (chroma variant).
 */
inline void filterChromaWeak(uint8_t& p0, const uint8_t& p1,
                              uint8_t& q0, const uint8_t& q1,
                              int32_t alpha, int32_t beta, int32_t tc0) noexcept
{
    int32_t ip0 = p0, ip1 = p1, iq0 = q0, iq1 = q1;

    if (std::abs(ip0 - iq0) >= alpha) return;
    if (std::abs(iq1 - iq0) >= beta) return;
    if (std::abs(ip1 - ip0) >= beta) return;

    int32_t tc = tc0 + 1; // Chroma always +1

    int32_t delta = ((((iq0 - ip0) << 2) + (ip1 - iq1) + 4) >> 3);
    delta = std::max(-tc, std::min(tc, delta));

    p0 = static_cast<uint8_t>(clipU8(ip0 + delta));
    q0 = static_cast<uint8_t>(clipU8(iq0 - delta));
}

/** Apply chroma filter (BS=4, strong) to one pixel crossing.
 *  Reference: ITU-T H.264 §8.7.2.4 (chroma variant).
 */
inline void filterChromaStrong(uint8_t& p0, const uint8_t& p1,
                                uint8_t& q0, const uint8_t& q1,
                                int32_t alpha, int32_t beta) noexcept
{
    int32_t ip0 = p0, ip1 = p1, iq0 = q0, iq1 = q1;

    if (std::abs(ip0 - iq0) >= alpha) return;
    if (std::abs(iq1 - iq0) >= beta) return;
    if (std::abs(ip1 - ip0) >= beta) return;

    p0 = static_cast<uint8_t>((2 * ip1 + ip0 + iq1 + 2) >> 2);
    q0 = static_cast<uint8_t>((2 * iq1 + iq0 + ip1 + 2) >> 2);
}

// ── MB-level deblocking ─────────────────────────────────────────────────

/** Deblock one macroblock (luma + chroma).
 *
 *  Filters all 4 vertical and 4 horizontal edges.
 *  Internal edges use this MB's QP. Boundary edges use
 *  (qpP + qpQ + 1) >> 1 — ITU-T H.264 §8.7.2.2.
 *
 *  When the q-side or p-side MB uses the 8x8 transform (§7.3.5
 *  transform_size_8x8_flag = 1), the corresponding internal 4x4 edges
 *  (vertical edges 1 and 3, horizontal edges 1 and 3) are skipped per
 *  §8.7.2 — those edges do not exist as transform-block boundaries and
 *  filtering them would smear genuine 4-pixel detail. Chroma is unaffected
 *  (chroma always uses 4x4 transforms; chroma deblock already only filters
 *  the 8-pel boundary at edge=0 and edge=2).
 *
 *  @param frame              Frame to filter in-place
 *  @param mbX,mbY            Macroblock position
 *  @param isIntra            True if this MB is intra-coded
 *  @param alphaOffset        Slice alpha_c0_offset_div2 * 2
 *  @param betaOffset         Slice beta_offset_div2 * 2
 *  @param nnzLuma            NNZ array for luma [totalMbs * 16]
 *  @param mbMotion           MV info per MB
 *  @param mbQps              Per-MB luma QP [totalMbs] — §7.4.5
 *  @param mbTransform8x8     Per-MB transform_size_8x8_flag [totalMbs] — §7.3.5
 *  @param chromaQpIndexOffset PPS chroma_qp_index_offset
 *  @param widthInMbs         Frame width in MBs
 *  @param heightInMbs        Frame height in MBs
 */
inline void deblockMb(Frame& frame, uint32_t mbX, uint32_t mbY,
                       bool isIntra, int32_t alphaOffset, int32_t betaOffset,
                       const uint8_t* nnzLuma,
                       const MbMotionInfo* mbMotion,
                       const int32_t* mbQps,
                       const uint8_t* mbTransform8x8,
                       int32_t chromaQpIndexOffset,
                       uint16_t widthInMbs, uint16_t heightInMbs) noexcept
{
    uint32_t mbIdx = mbY * widthInMbs + mbX;
    (void)heightInMbs;
    bool qUses8x8 = (mbTransform8x8[mbIdx] != 0U);

    // This MB's luma and chroma QP — used for internal edges.
    int32_t qp = mbQps[mbIdx];
    int32_t chromaQpIdx = clampQpIdx(qp + chromaQpIndexOffset);
    int32_t chromaQp = cChromaQpTable[chromaQpIdx];

    // Internal-edge thresholds (same MB on both sides).
    int32_t indexA = clampQpIdx(qp + alphaOffset);
    int32_t indexB = clampQpIdx(qp + betaOffset);
    int32_t alpha = cAlphaTable[indexA];
    int32_t beta = cBetaTable[indexB];

    int32_t cIndexA = clampQpIdx(chromaQp + alphaOffset);
    int32_t cIndexB = clampQpIdx(chromaQp + betaOffset);
    int32_t cAlpha = cAlphaTable[cIndexA];
    int32_t cBeta = cBetaTable[cIndexB];

    if (alpha == 0 && cAlpha == 0)
        return; // No filtering needed at this QP

    uint32_t pixX = mbX * 16U;
    uint32_t pixY = mbY * 16U;

    // ── Vertical edges (left boundary + 3 internal) ─────────────────

    for (uint32_t edge = 0U; edge < 4U; ++edge)
    {
        uint32_t edgeX = pixX + edge * 4U;
        if (edge == 0U && mbX == 0U) continue; // No left neighbor

        // §8.7.2: when this MB uses 8x8 transform, internal 4x4 edges at
        // x=4 (edge==1) and x=12 (edge==3) are not transform boundaries.
        if (qUses8x8 && (edge == 1U || edge == 3U)) continue;

        // For boundary edges, average QP of both MBs — §8.7.2.2.
        int32_t edgeAlpha = alpha, edgeBeta = beta, edgeIndexA = indexA;
        int32_t edgeCAlpha = cAlpha, edgeCBeta = cBeta, edgeCIndexA = cIndexA;
        if (edge == 0U)
        {
            uint32_t leftMbIdx = mbY * widthInMbs + mbX - 1U;
            int32_t qpAvg = (qp + mbQps[leftMbIdx] + 1) >> 1;
            edgeIndexA = clampQpIdx(qpAvg + alphaOffset);
            edgeAlpha = cAlphaTable[edgeIndexA];
            edgeBeta  = cBetaTable[clampQpIdx(qpAvg + betaOffset)];

            int32_t leftChromaQp = cChromaQpTable[clampQpIdx(mbQps[leftMbIdx] + chromaQpIndexOffset)];
            int32_t cQpAvg = (chromaQp + leftChromaQp + 1) >> 1;
            edgeCIndexA = clampQpIdx(cQpAvg + alphaOffset);
            edgeCAlpha  = cAlphaTable[edgeCIndexA];
            edgeCBeta   = cBetaTable[clampQpIdx(cQpAvg + betaOffset)];
        }

        // Precompute BS for 4 block rows on this edge (one per 4x4 block pair).
        // BS only changes at 4-row boundaries, so we compute 4 values and reuse.
        uint8_t edgeBs[4];
        {
            uint32_t mbIdxP = (edge == 0U) ? (mbY * widthInMbs + mbX - 1U) : mbIdx;
            for (uint32_t blkRow = 0U; blkRow < 4U; ++blkRow)
            {
                uint32_t blkQ = edge + blkRow * 4U;
                uint32_t blkP = (edge == 0U) ? 3U + blkRow * 4U : blkQ - 1U;
                bool isIntraP = (edge == 0U)
                    ? (mbMotion[mbIdxP * 16U + blkP].refIdx == -1) : isIntra;
                edgeBs[blkRow] = computeBs(isIntraP, isIntra, edge == 0U,
                    nnzLuma[mbIdxP * 16U + blkP] > 0U,
                    nnzLuma[mbIdx * 16U + blkQ] > 0U,
                    mbMotion[mbIdxP * 16U + blkP].mv.x,
                    mbMotion[mbIdxP * 16U + blkP].mv.y,
                    mbMotion[mbIdx * 16U + blkQ].mv.x,
                    mbMotion[mbIdx * 16U + blkQ].mv.y,
                    mbMotion[mbIdxP * 16U + blkP].refIdx,
                    mbMotion[mbIdx * 16U + blkQ].refIdx);
            }
        }

        for (uint32_t row = 0U; row < 16U; ++row)
        {
            uint8_t bs = edgeBs[row >> 2U];
            if (bs == 0U) continue;

            uint8_t* yPtr = frame.yRow(pixY + row) + edgeX;

            if (bs == 4U)
            {
                filterLumaStrong(yPtr[-1], yPtr[-2], yPtr[-3],
                                  yPtr[0], yPtr[1], yPtr[2],
                                  edgeX >= 4U ? yPtr[-4] : yPtr[-1],
                                  edgeX + 3U < frame.width() ? yPtr[3] : yPtr[0],
                                  edgeAlpha, edgeBeta);
            }
            else
            {
                int32_t tc0 = cTc0Table[edgeIndexA][bs];
                uint8_t p2 = (edgeX >= 3U) ? yPtr[-3] : yPtr[-1];
                uint8_t q2 = (edgeX + 2U < frame.width()) ? yPtr[2] : yPtr[0];
                filterLumaWeak(yPtr[-1], yPtr[-2], p2,
                               yPtr[0], yPtr[1], q2,
                               edgeAlpha, edgeBeta, tc0);
            }
        }

        // Chroma vertical edges — ITU-T H.264 §8.7.2.
        // Chroma edges at MB boundary (edge=0) and 8-pixel internal (edge=2).
        // BS is derived from the corresponding luma edge per §8.7.2.1: each
        // chroma row cRow maps to luma rows 2*cRow and 2*cRow+1, both in luma
        // block-row (cRow>>1). The luma BS was already computed per blkRow in
        // edgeBs[] above, so we reuse it directly.
        if (edge == 0U || edge == 2U)
        {
            uint32_t cEdgeX = pixX / 2U + (edge / 2U) * 4U;
            if (edge == 0U && mbX == 0U) continue;

            for (uint32_t cRow = 0U; cRow < 8U; ++cRow)
            {
                uint32_t cY = pixY / 2U + cRow;
                uint8_t maxBs = edgeBs[cRow >> 1U];
                if (maxBs == 0U) continue;

                for (int plane = 0; plane < 2; ++plane)
                {
                    uint8_t* ptr = (plane == 0)
                        ? frame.uRow(cY) + cEdgeX
                        : frame.vRow(cY) + cEdgeX;

                    if (maxBs == 4U)
                        filterChromaStrong(ptr[-1], (cEdgeX > 0U ? ptr[-2] : ptr[-1]),
                                           ptr[0], ptr[1], edgeCAlpha, edgeCBeta);
                    else
                        filterChromaWeak(ptr[-1], (cEdgeX > 0U ? ptr[-2] : ptr[-1]),
                                         ptr[0], ptr[1], edgeCAlpha, edgeCBeta,
                                         cTc0Table[edgeCIndexA][maxBs]);
                }
            }
        }
    }

    // ── Horizontal edges (top boundary + 3 internal) ────────────────

    for (uint32_t edge = 0U; edge < 4U; ++edge)
    {
        uint32_t edgeY = pixY + edge * 4U;
        if (edge == 0U && mbY == 0U) continue;

        // §8.7.2: when this MB uses 8x8 transform, internal 4x4 edges at
        // y=4 (edge==1) and y=12 (edge==3) are not transform boundaries.
        if (qUses8x8 && (edge == 1U || edge == 3U)) continue;

        // For boundary edges, average QP of both MBs — §8.7.2.2.
        int32_t edgeAlpha = alpha, edgeBeta = beta, edgeIndexA = indexA;
        int32_t edgeCAlpha = cAlpha, edgeCBeta = cBeta, edgeCIndexA = cIndexA;
        if (edge == 0U)
        {
            uint32_t topMbIdx = (mbY - 1U) * widthInMbs + mbX;
            int32_t qpAvg = (qp + mbQps[topMbIdx] + 1) >> 1;
            edgeIndexA = clampQpIdx(qpAvg + alphaOffset);
            edgeAlpha = cAlphaTable[edgeIndexA];
            edgeBeta  = cBetaTable[clampQpIdx(qpAvg + betaOffset)];

            int32_t topChromaQp = cChromaQpTable[clampQpIdx(mbQps[topMbIdx] + chromaQpIndexOffset)];
            int32_t cQpAvg = (chromaQp + topChromaQp + 1) >> 1;
            edgeCIndexA = clampQpIdx(cQpAvg + alphaOffset);
            edgeCAlpha  = cAlphaTable[edgeCIndexA];
            edgeCBeta   = cBetaTable[clampQpIdx(cQpAvg + betaOffset)];
        }

        // Precompute BS for 4 block columns on this horizontal edge.
        uint8_t hEdgeBs[4];
        {
            uint32_t mbIdxP = (edge == 0U) ? ((mbY - 1U) * widthInMbs + mbX) : mbIdx;
            for (uint32_t blkCol = 0U; blkCol < 4U; ++blkCol)
            {
                uint32_t blkQ = blkCol + edge * 4U;
                uint32_t blkP = (edge == 0U) ? blkCol + 12U : blkQ - 4U;
                bool isIntraP = (edge == 0U)
                    ? (mbMotion[mbIdxP * 16U + blkP].refIdx == -1) : isIntra;
                hEdgeBs[blkCol] = computeBs(isIntraP, isIntra, edge == 0U,
                    nnzLuma[mbIdxP * 16U + blkP] > 0U,
                    nnzLuma[mbIdx * 16U + blkQ] > 0U,
                    mbMotion[mbIdxP * 16U + blkP].mv.x,
                    mbMotion[mbIdxP * 16U + blkP].mv.y,
                    mbMotion[mbIdx * 16U + blkQ].mv.x,
                    mbMotion[mbIdx * 16U + blkQ].mv.y,
                    mbMotion[mbIdxP * 16U + blkP].refIdx,
                    mbMotion[mbIdx * 16U + blkQ].refIdx);
            }
        }

        for (uint32_t col = 0U; col < 16U; ++col)
        {
            uint8_t bs = hEdgeBs[col >> 2U];
            if (bs == 0U) continue;

            uint8_t* yQ = frame.yRow(edgeY) + pixX + col;
            uint8_t* yP = frame.yRow(edgeY - 1U) + pixX + col;

            if (bs == 4U)
            {
                uint8_t p3 = (edgeY >= 4U) ? *(frame.yRow(edgeY - 4U) + pixX + col) : *yP;
                uint8_t q3 = (edgeY + 3U < frame.height()) ? *(frame.yRow(edgeY + 3U) + pixX + col) : *yQ;
                uint8_t& p2 = *(frame.yRow(edgeY - 3U) + pixX + col);
                uint8_t& p1 = *(frame.yRow(edgeY - 2U) + pixX + col);
                uint8_t& q1 = *(frame.yRow(edgeY + 1U) + pixX + col);
                uint8_t& q2 = *(frame.yRow(edgeY + 2U) + pixX + col);

                filterLumaStrong(*yP, p1, p2, *yQ, q1, q2, p3, q3, edgeAlpha, edgeBeta);
            }
            else
            {
                int32_t tc0 = cTc0Table[edgeIndexA][bs];
                uint8_t p2 = (edgeY >= 3U) ? *(frame.yRow(edgeY - 3U) + pixX + col) : *yP;
                uint8_t q2 = (edgeY + 2U < frame.height()) ? *(frame.yRow(edgeY + 2U) + pixX + col) : *yQ;
                uint8_t& p1 = *(frame.yRow(edgeY - 2U) + pixX + col);
                uint8_t& q1 = *(frame.yRow(edgeY + 1U) + pixX + col);

                filterLumaWeak(*yP, p1, p2, *yQ, q1, q2, edgeAlpha, edgeBeta, tc0);
            }
        }

        // Chroma horizontal edges — ITU-T H.264 §8.7.2.
        // BS from corresponding luma edge per §8.7.2.1: chroma column cCol
        // maps to luma columns 2*cCol and 2*cCol+1, both in luma block-col
        // (cCol>>1). Reuse hEdgeBs[] computed above.
        if (edge == 0U || edge == 2U)
        {
            uint32_t cEdgeY = pixY / 2U + (edge / 2U) * 4U;
            if (edge == 0U && mbY == 0U) continue;

            for (uint32_t cCol = 0U; cCol < 8U; ++cCol)
            {
                uint32_t cX = pixX / 2U + cCol;
                uint8_t maxBs = hEdgeBs[cCol >> 1U];
                if (maxBs == 0U) continue;

                for (int plane = 0; plane < 2; ++plane)
                {
                    auto getRow = [&](uint32_t r) -> uint8_t* {
                        return plane == 0 ? frame.uRow(r) : frame.vRow(r);
                    };

                    uint8_t* qPtr = getRow(cEdgeY) + cX;
                    uint8_t* pPtr = getRow(cEdgeY - 1U) + cX;
                    uint8_t p1val = (cEdgeY >= 2U) ? *(getRow(cEdgeY - 2U) + cX) : *pPtr;
                    uint8_t q1val = (cEdgeY + 1U < frame.height() / 2U) ? *(getRow(cEdgeY + 1U) + cX) : *qPtr;

                    if (maxBs == 4U)
                        filterChromaStrong(*pPtr, p1val, *qPtr, q1val, edgeCAlpha, edgeCBeta);
                    else
                        filterChromaWeak(*pPtr, p1val, *qPtr, q1val,
                                         edgeCAlpha, edgeCBeta, cTc0Table[edgeCIndexA][maxBs]);
                }
            }
        }
    }
}

} // namespace sub0h264

#endif // CROG_SUB0H264_DEBLOCK_HPP
