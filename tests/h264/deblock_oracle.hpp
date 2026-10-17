/* Original macroblock traversal from 406b52e; SPDX-License-Identifier: MIT */
#pragma once
namespace sub0h264 {
inline void referenceDeblockMb(Frame& frame, uint32_t mbX, uint32_t mbY,
                       bool isIntra, int32_t alphaOffset, int32_t betaOffset,
                       const uint8_t* nnzLuma,
                       const MbMotionInfo* mbMotion,
                       const int32_t* mbQps,
                       const uint8_t* mbTransform8x8,
                       int32_t chromaQpIndexOffset,
                       uint16_t widthInMbs, uint16_t heightInMbs,
                       bool skipChroma = false) noexcept
{
    uint32_t mbIdx = mbY * widthInMbs + mbX;
    (void)heightInMbs;
    bool qUses8x8 = (mbTransform8x8[mbIdx] != 0U);

    // This MB's luma and chroma QP — used for internal edges.
    int32_t qp = mbQps[mbIdx];

    // Internal-edge thresholds (same MB on both sides).
    int32_t indexA = clampQpIdx(qp + alphaOffset);
    int32_t indexB = clampQpIdx(qp + betaOffset);
    int32_t alpha = cAlphaTable[indexA];
    int32_t beta = cBetaTable[indexB];

    // Keep the original joint early return: dropping cAlpha could change
    // which luma boundary edges are visited when neighboring QPs differ.
    int32_t chromaQp = cChromaQpTable[clampQpIdx(qp + chromaQpIndexOffset)];
    int32_t cAlpha = cAlphaTable[clampQpIdx(chromaQp + alphaOffset)];

    if (alpha == 0 && cAlpha == 0)
    {
        // Left and top MB edges use the QP averaged with the neighbour
        // (§8.7.2.2), so a low-QP MB beside a higher-QP one can still need
        // those edges filtered.
        bool boundaryFiltered = false;
        for (uint32_t side = 0U; side < 2U && !boundaryFiltered; ++side)
        {
            if (side == 0U ? mbX == 0U : mbY == 0U) continue;
            uint32_t neighborIdx = side == 0U ? mbIdx - 1U : mbIdx - widthInMbs;
            int32_t lumaAvg = (qp + mbQps[neighborIdx] + 1) >> 1;
            int32_t chromaAvg = (chromaQp +
                cChromaQpTable[clampQpIdx(mbQps[neighborIdx] + chromaQpIndexOffset)] + 1) >> 1;
            boundaryFiltered = cAlphaTable[clampQpIdx(lumaAvg + alphaOffset)] != 0 ||
                               cAlphaTable[clampQpIdx(chromaAvg + alphaOffset)] != 0;
        }
        if (!boundaryFiltered)
            return; // No filtering needed at this QP
    }

    uint8_t verticalBs[4][4] = {}, horizontalBs[4][4] = {};
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
        if (edge == 0U)
        {
            uint32_t leftMbIdx = mbY * widthInMbs + mbX - 1U;
            int32_t qpAvg = (qp + mbQps[leftMbIdx] + 1) >> 1;
            edgeIndexA = clampQpIdx(qpAvg + alphaOffset);
            edgeAlpha = cAlphaTable[edgeIndexA];
            edgeBeta  = cBetaTable[clampQpIdx(qpAvg + betaOffset)];
        }

        // Precompute BS for 4 block rows on this edge (one per 4x4 block pair).
        // BS only changes at 4-row boundaries, so we compute 4 values and reuse.
        auto& edgeBs = verticalBs[edge];
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
        if (edge == 0U)
        {
            uint32_t topMbIdx = (mbY - 1U) * widthInMbs + mbX;
            int32_t qpAvg = (qp + mbQps[topMbIdx] + 1) >> 1;
            edgeIndexA = clampQpIdx(qpAvg + alphaOffset);
            edgeAlpha = cAlphaTable[edgeIndexA];
            edgeBeta  = cBetaTable[clampQpIdx(qpAvg + betaOffset)];
        }

        // Precompute BS for 4 block columns on this horizontal edge.
        auto& hEdgeBs = horizontalBs[edge];
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
    }
    // Chroma filtering is independent of Y, so choose once for the whole MB.
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
    if (!skipChroma)
        deblockChromaMb(frame, mbX, mbY, alphaOffset, betaOffset,
                        mbQps, chromaQpIndexOffset, widthInMbs,
                        verticalBs, horizontalBs);
#else
    (void)skipChroma;
#endif

}

}
