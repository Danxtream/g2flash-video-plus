/** Sub0h264 — H.264 decoder pipeline
 *
 *  Wires together NAL parsing, parameter sets, slice headers,
 *  CAVLC entropy decoding, intra prediction, and inverse transform
 *  into a complete decode pipeline.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_DECODER_HPP
#define CROG_SUB0H264_DECODER_HPP

#include "annexb.hpp"
#include "bitstream.hpp"
#include "cabac.hpp"
#include "cabac_mb_parser.hpp"
#include "cabac_neighbor.hpp"
#include "cabac_parse.hpp"
#include "cavlc.hpp"
#include "deblock.hpp"

// #define SUB0H264_TRACE_I4X4_BLOCKS
// #define SUB0H264_EXP_FORCE_MODE1_SCAN5
// #define SUB0H264_P0_I8X8_DIAG 1  // Enable for I_8x8/I_4x4/I_16x16 MB diag
#include "decode_timing.hpp"
#include "decode_trace.hpp"
#include "dpb.hpp"
#include "frame.hpp"
#include "inter_pred.hpp"
#include "intra_pred.hpp"
#include "motion.hpp"
#include "nal.hpp"
#include "param_sets.hpp"
#include "pps.hpp"
#include "scaling_list.hpp"
#include "slice.hpp"
#include "sps.hpp"
#include "tables.hpp"
#include "transform.hpp"
#include "sub0h264/sub0h264_types.hpp"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sub0h264 {

/// Decode status for a single frame.
enum class DecodeStatus : int32_t
{
    FrameDecoded =  1,   ///< A complete frame was decoded
    NeedMoreData =  0,   ///< NAL consumed, no frame yet (SPS/PPS/partial)
    Error        = -1,   ///< Decode error (corrupted bitstream or unsupported feature)
};

/** Graceful handling for unsupported spec features.
 *
 *  In debug builds, triggers an immediate assert so developers see the
 *  unsupported-feature condition during testing. In release builds, the
 *  assert compiles out and execution continues to the caller-provided
 *  return statement (e.g. `return DecodeStatus::Error` or `return false`).
 *
 *  Usage:
 *      SUB0H264_UNSUPPORTED("B-slices not implemented — §7.3.4");
 *      return DecodeStatus::Error;
 *
 *  This pattern preserves the existing assert-on-debug discipline while
 *  ensuring release builds degrade gracefully on conforming-but-unsupported
 *  bitstreams rather than hard-crashing.
 */
#define SUB0H264_UNSUPPORTED(msg) assert(false && (msg))

/** Simplified H.264 decoder.
 *
 *  Currently supports I-frame (IDR) decoding only.
 *  Baseline profile, CAVLC entropy coding, max 640x480.
 */
class H264Decoder
{
public:
    H264Decoder() = default;

    /** Feed a complete Annex-B byte stream and decode all frames.
     *
     *  @param data   Annex-B byte stream (may contain multiple NALs/frames)
     *  @param size   Size in bytes
     *  @return Number of frames decoded, or -1 on error
     */
    int32_t decodeStream(const uint8_t* data, uint32_t size) noexcept
    {
        std::vector<NalBounds> bounds;
        findNalUnits(data, size, bounds);

        int32_t framesDecoded = 0;

        for (const auto& b : bounds)
        {
            NalUnit nal;
            if (!parseNalUnit(data + b.offset, b.size, nal))
                continue;

            DecodeStatus status = processNal(nal);
            if (status == DecodeStatus::FrameDecoded)
                ++framesDecoded;
        }

        return framesDecoded;
    }

    /** Process a single parsed NAL unit.
     *  @return DecodeStatus indicating result
     */
    DecodeStatus processNal(const NalUnit& nal) noexcept
    {
        BitReader br(nal.rbspData.data(), static_cast<uint32_t>(nal.rbspData.size()));

        switch (nal.type)
        {
        case NalType::Sps:
        {
            Sps sps;
            if (parseSps(br, sps) != Result::Ok)
                return DecodeStatus::Error;
            paramSets_.storeSps(sps);
            return DecodeStatus::NeedMoreData;
        }

        case NalType::Pps:
        {
            Pps pps;
            if (parsePps(br, paramSets_.spsArray(), pps) != Result::Ok)
                return DecodeStatus::Error;
            paramSets_.storePps(pps);
            return DecodeStatus::NeedMoreData;
        }

        case NalType::SliceIdr:
        case NalType::SliceNonIdr:
            return decodeSlice(br, nal);

        default:
            return DecodeStatus::NeedMoreData; // Ignore SEI, AUD, etc.
        }
    }

    /** @return Most recently decoded frame (nullptr if none). */
    const Frame* currentFrame() const noexcept
    {
        const Frame* dpbFrame = dpb_.currentFrame();
        if (dpbFrame && dpbFrame->isAllocated())
            return dpbFrame;
        return currentFrame_.isAllocated() ? &currentFrame_ : nullptr;
    }

    /** @return Mutable reference to parameter sets. */
    ParamSets& paramSets() noexcept { return paramSets_; }

    /** @return Number of frames decoded so far. */
    uint32_t frameCount() const noexcept { return frameCount_; }

    /** Set trace filter for debugging. Printf requires SUB0H264_TRACE=1 build.
     *  Callback tracing is always available. */
    void setTrace(const DecodeTrace& t) noexcept { trace_ = t; }

    /** Access trace for setting callbacks from tests. */
    DecodeTrace& trace() noexcept { return trace_; }
    CabacEngine& cabacEngine() noexcept { return cabacEngine_; }
    CabacMbParser& cabacParser() noexcept { return cabacParser_; }
    CabacContextSet& cabacContexts() noexcept { return cabacCtx_; }
    CabacNeighborCtx& cabacNeighbors() noexcept { return cabacNeighbor_; }

    /** Enable per-section profiling. Pass nullptr to disable. */
    void setProfile(SectionProfile* profile) noexcept { profile_ = profile; }

    /** Read MV at a specific 4x4 block position (absolute 4x4 grid coords).
     *  Public for test inspection of internal MV state. */
    MbMotionInfo motionAt4x4(int32_t blk4x, int32_t blk4y) const noexcept
    {
        return getMotionAt4x4(blk4x, blk4y);
    }

private:
    DecodeTrace trace_;
    SectionProfile* profile_ = nullptr;
    ParamSets paramSets_;
    Frame currentFrame_;
    Frame* activeFrame_ = nullptr; ///< Frame being decoded into (DPB target or currentFrame_)
    Dpb dpb_;
    uint32_t frameCount_ = 0U;
    bool dpbInitialized_ = false;

    // Per-frame MB context: non-zero coefficient counts for CAVLC context
    std::vector<uint8_t> nnzLuma_;    // [mbIdx * 16 + blkIdx]
    std::vector<uint8_t> nnzCb_;      // [mbIdx * 4 + blkIdx]
    std::vector<uint8_t> nnzCr_;      // [mbIdx * 4 + blkIdx]

    // Per-MB luma QP after mb_qp_delta accumulation — ITU-T H.264 §7.4.5.
    // Used by deblocking filter: boundary edges require QP averaging (§8.7.2.2).
    std::vector<int32_t> mbQps_;      // [mbIdx]

    // Per-MB transform_size_8x8_flag — ITU-T H.264 §7.3.5.
    // Used by deblocking (§8.7.2): MBs using 8x8 transform skip filtering of
    // internal 4x4 edges (vertical edges 1,3 and horizontal edges 1,3).
    std::vector<uint8_t> mbTransform8x8_; // [mbIdx], 0 or 1

    // Per-frame MV context for inter prediction
    std::vector<MbMotionInfo> mbMotion_;  // [mbIdx]

    /// Per-slice resolved scaling lists in raster order (§7.4.2.1.1).
    /// Refreshed each slice; lists default to flat-16 when no SPS/PPS matrix.
    ResolvedScalingLists scalingLists_{};

    // Per-MB intra 4x4 prediction modes for MPM derivation across MBs.
    // [mbIdx * 16 + blkIdx] — only valid for I_4x4 MBs.
    // For I_16x16 MBs, all entries set to DC(2) per spec §8.3.1.1.
    std::vector<uint8_t> mbIntra4x4Modes_;  // [mbIdx * 16 + blkIdx]

    // CABAC state
    uint32_t cabacSliceCount_ = 0U; ///< Counts CABAC slice inits for SliceStart event indexing
    CabacEngine cabacEngine_;
    CabacContextSet cabacCtx_;
    CabacNeighborCtx cabacNeighbor_; ///< Per-MB CABAC neighbor state (skip, I4x4, CBP, chroma mode)
    CabacMbParser cabacParser_;      ///< Bound syntax element parser for isolated testing
    /// §9.3.3.1.1.5: prevMbQpDelta — tracks whether the previous MB had non-zero
    /// mb_qp_delta for bin[0] context selection. Reset at slice start.
    bool prevMbHadNonZeroQpDelta_ = false;

    uint16_t widthInMbs_ = 0U;
    uint16_t heightInMbs_ = 0U;
    uint16_t prevRefFrameNum_ = 0U; ///< §8.2.5.2: PrevRefFrameNum for gap detection

    /** Get nC (CAVLC context) for a luma 4x4 block.
     *  nC = (leftNnz + topNnz + 1) >> 1
     */
    /** Get nC context for CAVLC coeff_token decode.
     *  nC = (nA + nB + 1) >> 1 when both available, else nA or nB alone.
     *  Reference: ITU-T H.264 §9.2.1 Table 9-5 context derivation.
     */
    int32_t getLumaNc(uint32_t mbX, uint32_t mbY, uint32_t blkIdx) const noexcept
    {
        // Block position within MB (raster-order 4x4 grid):
        //  0  1  2  3
        //  4  5  6  7
        //  8  9 10 11
        // 12 13 14 15
        uint32_t blkX = blkIdx & 3U;
        uint32_t blkY = blkIdx >> 2U;

        int32_t leftNnz = 0;
        int32_t topNnz = 0;
        bool hasLeft = false, hasTop = false;

        // Left neighbor
        if (blkX > 0U)
        {
            // Within same MB
            uint32_t leftBlk = blkIdx - 1U;
            leftNnz = nnzLuma_[(mbY * widthInMbs_ + mbX) * 16U + leftBlk];
            hasLeft = true;
        }
        else if (mbX > 0U)
        {
            // From left MB, rightmost column
            uint32_t leftBlk = blkY * 4U + 3U;
            leftNnz = nnzLuma_[(mbY * widthInMbs_ + mbX - 1U) * 16U + leftBlk];
            hasLeft = true;
        }

        // Top neighbor
        if (blkY > 0U)
        {
            // Within same MB
            uint32_t topBlk = blkIdx - 4U;
            topNnz = nnzLuma_[(mbY * widthInMbs_ + mbX) * 16U + topBlk];
            hasTop = true;
        }
        else if (mbY > 0U)
        {
            // From top MB, bottom row
            uint32_t topBlk = 12U + blkX;
            topNnz = nnzLuma_[((mbY - 1U) * widthInMbs_ + mbX) * 16U + topBlk];
            hasTop = true;
        }

        if (hasLeft && hasTop) return (leftNnz + topNnz + 1) >> 1;
        if (hasLeft) return leftNnz;
        if (hasTop) return topNnz;
        return 0;
    }

    /** Get nC (CAVLC context) for a chroma 4x4 block.
     *
     *  Chroma block layout per MB (Cb or Cr, 2x2 grid):
     *    0  1
     *    2  3
     *
     *  @param mbX,mbY    Macroblock position
     *  @param blkIdx     Chroma block index [0-3]
     *  @param isCb       True for Cb plane, false for Cr
     *  @return nC context value
     *
     *  Reference: ITU-T H.264 §9.2.1
     */
    int32_t getChromaNc(uint32_t mbX, uint32_t mbY, uint32_t blkIdx, bool isCb) const noexcept
    {
        const auto& nnz = isCb ? nnzCb_ : nnzCr_;
        uint32_t blkX = blkIdx & 1U;
        uint32_t blkY = blkIdx >> 1U;

        int32_t leftNnz = 0, topNnz = 0;
        bool hasLeft = false, hasTop = false;

        // Left neighbor
        if (blkX > 0U)
        {
            leftNnz = nnz[(mbY * widthInMbs_ + mbX) * 4U + blkIdx - 1U];
            hasLeft = true;
        }
        else if (mbX > 0U)
        {
            /// Right column of left MB: blkY * 2 + 1
            uint32_t leftBlk = blkY * 2U + 1U;
            leftNnz = nnz[(mbY * widthInMbs_ + mbX - 1U) * 4U + leftBlk];
            hasLeft = true;
        }

        // Top neighbor
        if (blkY > 0U)
        {
            topNnz = nnz[(mbY * widthInMbs_ + mbX) * 4U + blkIdx - 2U];
            hasTop = true;
        }
        else if (mbY > 0U)
        {
            /// Bottom row of top MB: 2 + blkX
            uint32_t topBlk = 2U + blkX;
            topNnz = nnz[((mbY - 1U) * widthInMbs_ + mbX) * 4U + topBlk];
            hasTop = true;
        }

        if (hasLeft && hasTop) return (leftNnz + topNnz + 1) >> 1;
        if (hasLeft) return leftNnz;
        if (hasTop) return topNnz;
        return 0;
    }

    /** Derive chroma QP from luma QP and PPS offset.
     *  §8.5.12.1 / §7.4.2.2: QPc = cChromaQpTable[Clip3(0,51, QP + offset)].
     *  NOTE (FM-10): Always use this helper — never apply luma QP directly to chroma.
     *  [CHECKED §7.4.2.2] — clampQpIdx implements Clip3(0,51,·) and cChromaQpTable
     *  matches spec Table 8-15.
     */
    static int32_t computeChromaQp(int32_t qp, int32_t offset) noexcept
    {
        return cChromaQpTable[clampQpIdx(qp + offset)];
    }

    /** Decode transform_size_8x8_flag via CABAC — §9.3.3.1.1.10.
     *  ctxInc = condTermFlagA + condTermFlagB from neighbor transform flags.
     *  Stores the decoded flag in cabacNeighbor_ for future context derivation.
     *  Used by both I_NxN and P-inter decode paths. */
    bool decodeCabacTransform8x8Flag(uint32_t mbX, uint32_t mbY,
                                      uint32_t mbIdx) noexcept
    {
        uint32_t t8x8A = (mbX > 0U) ? (cabacNeighbor_[mbIdx - 1U].transform8x8() ? 1U : 0U) : 0U;
        uint32_t t8x8B = (mbY > 0U) ? (cabacNeighbor_[mbIdx - widthInMbs_].transform8x8() ? 1U : 0U) : 0U;
        bool flag = cabacEngine_.decodeBin(cabacCtx_[cCtxTransform8x8 + t8x8A + t8x8B]) != 0U;
        cabacNeighbor_[mbIdx].setTransform8x8(flag);
        return flag;
    }

    /** Dequantize four chroma 2x2 DC coefficients after inverse Hadamard.
     *  §8.5.11.2: uses LevelScale4x4(chromaQP%6, 0, 0) which includes
     *  `weightScale[0][0]` of the active chroma scaling list when non-flat.
     *  `weightScale00` passes that factor; flat lists use 16 (the identity).
     *  [CHECKED FM-10]
     */
    static void dequantChromaDcValues(int16_t dc[4], int32_t chromaQp,
                                       int32_t weightScale00 = 16) noexcept
    {
        int32_t cqpDiv6 = chromaQp / 6;
        int32_t cqpMod6 = chromaQp % 6;
        int32_t normAdjust = cDequantScale[cqpMod6][0]; // normAdjust4x4(chromaQP%6, 0, 0)
        // Spec §8.5.11.2 Eq 8-326 for chroma DC after 2x2 Hadamard:
        //   dcC' = (dcC * LevelScale4x4(cQP%6,0,0)) << (cQP/6 - 5)        if cQP/6 >= 5
        //   dcC' = (dcC * LevelScale4x4(cQP%6,0,0) + 2^(4-cQP/6)) >> (5-cQP/6) otherwise
        // For flat (weightScale00=16) the libavc path keeps the `/16`
        // factor folded into the -1 shift exponent. Non-flat multiplies
        // weightScale00 explicitly and uses the spec's -5 exponent form.
        const bool flat = (weightScale00 == 16);
        for (uint32_t i = 0U; i < 4U; ++i)
        {
            if (dc[i] == 0) continue;
            int32_t val;
            if (flat)
            {
                val = dc[i] * normAdjust;
                val = cqpDiv6 >= 1
                    ? (val << (cqpDiv6 - 1))
                    : ((val + 1) >> 1);
            }
            else
            {
                val = dc[i] * normAdjust * weightScale00;
                val = cqpDiv6 >= 5
                    ? (val << (cqpDiv6 - 5))
                    : ((val + (1 << (4 - cqpDiv6))) >> (5 - cqpDiv6));
            }
            dc[i] = static_cast<int16_t>(val);
#ifdef SUB0H264_DEQUANT_NORM
            dc[i] = static_cast<int16_t>((dc[i] + 32) >> 6);
#endif
        }
    }

    /** Chroma AC coded_block_flag context increment — §9.3.3.1.1.9 (ctxBlockCat=4).
     *
     *  Chroma 4x4 blocks in 2x2 grid: 0=TL, 1=TR, 2=BL, 3=BR.
     *  Left/top neighbor derivation follows §6.4.11.5 for neighboring 4x4 chroma blocks.
     *  transBlockN available if neighbor avail, not skip/I_PCM, cbpChroma==2.
     *  condTermFlagN: unavail+intra→1, unavail+inter→0, avail+transBlock avail→actual CBF.
     *
     *  @param isCb  True for Cb component, false for Cr
     *  @param isCurrentIntra  True if current MB is intra
     */
    uint32_t chromaAcCbfCtxInc(uint32_t mbX, uint32_t mbY,
                                uint32_t blkIdx, bool isCb,
                                bool isCurrentIntra) const noexcept
    {
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;
        const auto& nnz = isCb ? nnzCb_ : nnzCr_;
        uint32_t defaultVal = isCurrentIntra ? 1U : 0U; // unavailable default

        // Left neighbor (A)
        uint32_t leftNnz = defaultVal;
        if (blkIdx & 1U) // block 1 or 3: left is block 0 or 2 in same MB
            leftNnz = nnz[mbIdx * 4U + blkIdx - 1U];
        else if (mbX > 0U) // block 0 or 2: left is block 1 or 3 of left MB
        {
            uint32_t leftMbIdx = mbIdx - 1U;
            if (cabacNeighbor_[leftMbIdx].chromaCbp() == 2U)
                leftNnz = nnz[leftMbIdx * 4U + blkIdx + 1U];
            else
                leftNnz = 0U; // cbpChroma < 2 → transBlock not available → 0
        }

        // Top neighbor (B)
        uint32_t topNnz = defaultVal;
        if (blkIdx >= 2U) // block 2 or 3: top is block 0 or 1 in same MB
            topNnz = nnz[mbIdx * 4U + blkIdx - 2U];
        else if (mbY > 0U) // block 0 or 1: top is block 2 or 3 of top MB
        {
            uint32_t topMbIdx = mbIdx - widthInMbs_;
            if (cabacNeighbor_[topMbIdx].chromaCbp() == 2U)
                topNnz = nnz[topMbIdx * 4U + blkIdx + 2U];
            else
                topNnz = 0U; // cbpChroma < 2 → transBlock not available → 0
        }

        return (leftNnz != 0U ? 1U : 0U) + (topNnz != 0U ? 2U : 0U);
    }

    /** Dequantize 16 Intra_16x16 luma DC coefficients after inverse 4x4 Hadamard.
     *  §8.5.12.1 Eq. 8-324..8-326: Scaling of Intra16x16 luma DC coefficients.
     *    if qp/6 >= 2: dc[i] = dc[i] * LevelScale(qp%6,0,0) << (qp/6 - 2)
     *    else:         dc[i] = (dc[i] * LevelScale(qp%6,0,0) + 2^(1-qp/6)) >> (2 - qp/6)
     *  NOTE: Uses luma QP (not chroma QP).
     */
    static void dequantLumaDcValues(int16_t dc[16], int32_t qp,
                                     int32_t weightScale00 = 16) noexcept
    {
        int32_t qpDiv6 = qp / 6;
        int32_t qpMod6 = qp % 6;
        int32_t normAdjust = cDequantScale[qpMod6][0]; // normAdjust4x4(qP%6, 0, 0)
        // Spec §8.5.10 Eq 8-325 for I_16x16 DC after Hadamard:
        //   dcY' = (dcY * LevelScale4x4(qP%6,0,0)) << (qP/6 - 6)           if qP/6 >= 6
        //   dcY' = (dcY * LevelScale4x4(qP%6,0,0) + 2^(5-qP/6)) >> (6-qP/6) if qP/6 < 6
        // LevelScale4x4(m,0,0) = normAdjust * weightScale00.
        //
        // For weightScale00 == 16 (flat) the historical libavc path folds
        // the factor of 16 into a pre-subtracted shift exponent, producing
        // the `(qpDiv6 - 2)` / `(2 - qpDiv6)` form. That fast path has no
        // precision loss. For non-flat, we fall back to the spec equation
        // directly — multiplying by weightScale00 and shifting by -6 — so
        // the `/16` rounding of the earlier approach is avoided.
        const bool flat = (weightScale00 == 16);
        for (uint32_t i = 0U; i < 16U; ++i)
        {
            if (dc[i] == 0) continue;
            int32_t val;
            if (flat)
            {
                val = dc[i] * normAdjust;
                val = qpDiv6 >= 2
                    ? (val << (qpDiv6 - 2))
                    : ((val + (1 << (1 - qpDiv6))) >> (2 - qpDiv6));
            }
            else
            {
                val = dc[i] * normAdjust * weightScale00;
                val = qpDiv6 >= 6
                    ? (val << (qpDiv6 - 6))
                    : ((val + (1 << (5 - qpDiv6))) >> (6 - qpDiv6));
            }
            dc[i] = static_cast<int16_t>(val);
#ifdef SUB0H264_DEQUANT_NORM
            dc[i] = static_cast<int16_t>((dc[i] + 32) >> 6);
#endif
        }
    }

    /** Sample reference pixels for one I_4x4 4x4 block from activeFrame_.
     *  §8.3.1.1: Reference sample availability.
     *  Uses stride arithmetic for left column.
     *  Always uses activeFrame_ (FM-16: avoids currentFrame_ vs activeFrame_ mismatch).
     *  [UNCHECKED §8.3.1.1]
     *
     *  @param absX/absY  Absolute pixel position of block top-left
     *  @param blkIdx     Scan-order block index within MB (0..15)
     *  @param topBuf     Caller-allocated uint8_t[8] scratch for top + top-right samples
     *  @param leftBuf    Caller-allocated uint8_t[4] scratch for left column
     *  @param topLeftVal Caller-allocated scalar for top-left sample
     *  @param top/topRight/left/topLeft  Output pointers (nullptr = unavailable)
     */
    void sampleIntra4x4Neighbors(uint32_t absX, uint32_t absY, uint32_t blkIdx,
                                  uint8_t topBuf[8], uint8_t leftBuf[4],
                                  uint8_t& topLeftVal,
                                  const uint8_t*& top, const uint8_t*& topRight,
                                  const uint8_t*& left, const uint8_t*& topLeft) const noexcept
    {
        top = nullptr; topRight = nullptr; left = nullptr; topLeft = nullptr;
        topLeftVal = cDefaultPredValue;

        if (absY > 0U)
        {
            const uint8_t* row = activeFrame_->yRow(absY - 1U);
            for (uint32_t i = 0U; i < 4U; ++i) topBuf[i] = row[absX + i];
            top = topBuf;
            // Top-right availability — §6.4.11. [CHECKED §6.4.11]
            // cTopRightUnavailScan derived at compile time from §6.4.3 scan
            // order (see tables.hpp). Covers: same-MB scan ordering + right-
            // edge. Caller checks absX+4 for frame boundary.
            bool topRightAvail = (absX + 4U < activeFrame_->width())
                                 && !cTopRightUnavailScan[blkIdx];
            if (topRightAvail)
            {
                for (uint32_t i = 0U; i < 4U; ++i) topBuf[4U + i] = row[absX + 4U + i];
                topRight = topBuf + 4U;
            }
        }
        if (absX > 0U)
        {
            // Stride arithmetic avoids per-row y(x,y) multiply.
            const uint8_t* leftCol = activeFrame_->yRow(absY) + (absX - 1U);
            uint32_t stride = activeFrame_->yStride();
            for (uint32_t i = 0U; i < 4U; ++i) leftBuf[i] = leftCol[i * stride];
            left = leftBuf;
        }
        if (absX > 0U && absY > 0U)
        {
            topLeftVal = activeFrame_->y(absX - 1U, absY - 1U);
            topLeft = &topLeftVal;
        }
    }

    /** Decode a slice (IDR or non-IDR). */
    DecodeStatus decodeSlice(BitReader& br, const NalUnit& nal) noexcept
    {
        bool isIdr = (nal.type == NalType::SliceIdr);

        // Peek at PPS ID to find the right parameter sets
        BitReader peekBr = br;
        peekBr.readUev(); // first_mb_in_slice
        peekBr.readUev(); // slice_type
        uint8_t ppsId = static_cast<uint8_t>(peekBr.readUev());

        const Pps* pps = paramSets_.getPps(ppsId);
        if (!pps) return DecodeStatus::Error;
        const Sps* sps = paramSets_.getSps(pps->spsId_);
        if (!sps) return DecodeStatus::Error;

        // §7.4.2.1.1 / §8.5.12.1 — resolve scaling lists for this slice.
        // Cached per-slice; dequant call sites take the scaled path only when
        // the relevant list is non-flat (most streams remain on the flat-16
        // fast path). See scaling_list.hpp for fall-back rules.
        scalingLists_ = resolveScalingLists(*sps, *pps);

        // Parse slice header
        SliceHeader sh;
        if (parseSliceHeader(br, *sps, *pps, isIdr, nal.refIdc, sh) != Result::Ok)
            return DecodeStatus::Error;

        // Initialize DPB and allocate context on first SPS use
        widthInMbs_ = sps->widthInMbs_;
        heightInMbs_ = sps->heightInMbs_;
        uint32_t totalMbs = static_cast<uint32_t>(widthInMbs_) * heightInMbs_;

        //TODO: We should reason about the best way to manage DPB initialization and resizing. Currently we check if dpbInitialized_ is false, and if so we call dpb_.init() with the width, height, and numRefFrames from the SPS. This means that the DPB will be initialized on the first frame that uses a valid SPS. However, we should also consider what happens if we encounter a new SPS with different dimensions or reference frame requirements later in the stream. In that case, we might need to reinitialize or resize the DPB to accommodate the new parameters. We should ensure that our DPB implementation can handle such changes gracefully, either by allowing dynamic resizing or by enforcing that all frames use the same SPS parameters. Additionally, we should consider how this interacts with frame buffers that are currently in use as references in the DPB when reinitializing or resizing.
        if (!dpbInitialized_)
        {
            dpb_.init(sps->width(), sps->height(), sps->numRefFrames_);
            dpbInitialized_ = true;
        }

        // Get a frame buffer from the DPB to decode into
        Frame* decodeTarget = dpb_.getDecodeTarget();
        if (!decodeTarget)
            return DecodeStatus::Error;

        //TODO: We should reason about the best way to manage the active frame buffer for decoding. Currently we call dpb_.getDecodeTarget() to get a frame to decode into, and we also have a separate currentFrame_ that we use for backwards compatibility. We need to ensure that we are correctly managing the lifecycle of these frames, especially when it comes to reference counting and ensuring that we don't overwrite frames that are still needed as references. We might want to consider having a clear ownership model where the DPB manages all frame buffers and we only have pointers or references to those frames in the decoder, rather than having a separate currentFrame_ that is allocated independently. This would help avoid confusion and potential bugs related to frame management.
        // Also keep a reference in currentFrame_ for backwards compatibility
        if (!currentFrame_.isAllocated() ||
            currentFrame_.width() != sps->width() ||
            currentFrame_.height() != sps->height())
        {
            currentFrame_.allocate(sps->width(), sps->height());
        }

        // Resize context arrays
        // TODO: we could optimize this by only resizing on resolution change
        // TODO: We should use a custom allocator or some more specific batch allocating approach so we can ensure memory locality of table allocations?
        // TODO: We could just allocate a static maximal-bounded size -embedded friendly.
        nnzLuma_.resize(totalMbs * 16U, 0U);
        nnzCb_.resize(totalMbs * 4U, 0U);
        nnzCr_.resize(totalMbs * 4U, 0U);
        mbQps_.resize(totalMbs, 0);
        mbTransform8x8_.resize(totalMbs, 0U);
        mbMotion_.resize(totalMbs * 16U); // 16 MVs per MB (per-4x4-block)
        mbIntra4x4Modes_.resize(totalMbs * 16U, 2U); // Default DC(2)

        // Clear context for new frame
        if (isIdr)
        {
            dpb_.flush();
            prevRefFrameNum_ = sh.frameNum_; // §8.2.5.2: reset at IDR
            // Re-get decode target after flush
            decodeTarget = dpb_.getDecodeTarget();
        }

        // TODO; We should reason over if these all need to be zeroed - document if they do or don't etc.
        //TODO: We should consider using a more efficient way to clear these large context arrays, such as std::fill or memset, instead of a loop. For example, we could use std::fill(nnzLuma_.begin(), nnzLuma_.end(), 0U) to set all values to zero in one call, which is likely optimized and more efficient than a manual loop. This would also improve readability by clearly indicating that we are initializing the entire array to zero.
        std::fill(nnzLuma_.begin(), nnzLuma_.end(), static_cast<uint8_t>(0U));
        std::fill(nnzCb_.begin(), nnzCb_.end(), static_cast<uint8_t>(0U));
        std::fill(nnzCr_.begin(), nnzCr_.end(), static_cast<uint8_t>(0U));
        std::fill(mbQps_.begin(), mbQps_.end(), 0);
        std::fill(mbTransform8x8_.begin(), mbTransform8x8_.end(), static_cast<uint8_t>(0U));
        std::fill(mbMotion_.begin(), mbMotion_.end(), MbMotionInfo{});
        std::fill(mbIntra4x4Modes_.begin(), mbIntra4x4Modes_.end(), static_cast<uint8_t>(2U));

#if SUB0H264_TRACE
        std::printf("[DBG] Slice header parsed: type=%u firstMb=%lu frameNum=%u qpDelta=%d bitOffset=%lu cabacIdc=%u numRefL0=%u weightedPred=%u\n",
            static_cast<unsigned>(sh.sliceType_), (unsigned long)sh.firstMbInSlice_,
            sh.frameNum_, sh.sliceQpDelta_, (unsigned long)br.bitOffset(),
            sh.cabacInitIdc_, sh.numRefIdxActiveL0_, pps->weightedPredFlag_ ? 1U : 0U);
#endif

        // Compute effective QP
        int32_t sliceQp = pps->picInitQp_ + sh.sliceQpDelta_;

        // Emit SliceStart trace event (always — cheap callback check).
        // The callback decides whether to enable entropy tracing for this slice.
        trace_.onSliceStart(cabacSliceCount_,
            static_cast<uint32_t>(sh.sliceType_),
            pps->isCabac(), sliceQp);

        // Reference frame is obtained AFTER buildRefListL0() for P-slices
        // (moved below to avoid using stale L0 list from previous frame)
        const Frame* refFrame = nullptr;

        // Initialize CABAC engine if High profile
        const bool useCabac = pps->isCabac();
        if (useCabac)
        {
            // Align to byte boundary after slice header
            //TODO: We should reason about the best way to handle bitstream alignment after the slice header when using CABAC. Currently we call br.alignToByte() before initializing the CABAC engine, which is necessary because the slice header may not end on a byte boundary and CABAC decoding operates on bytes. However, we should ensure that this alignment is correctly handled in all cases, especially if there are any optional fields in the slice header that could affect the bit offset. We might also want to consider encapsulating this alignment logic into a helper function that is called whenever we switch from parsing syntax elements to initializing the CABAC engine, to ensure consistency and reduce the chance of errors.
            br.alignToByte();

            // Initialize CABAC contexts — §9.3.1.1
            uint32_t sliceTypeIdx = (sh.sliceType_ == SliceType::I) ? 2U :
                                    (sh.sliceType_ == SliceType::P) ? 0U : 1U;
            cabacCtx_.init(sliceTypeIdx, sh.cabacInitIdc_, sliceQp);

            // Initialize arithmetic engine — §9.3.1.2
            cabacEngine_.init(br);
            // Emit CabacInit trace event and bind trace to the engine.
            // The callback decides whether to act on this slice's bins.
#if SUB0H264_TRACE
            trace_.onCabacInit(cabacSliceCount_, cabacEngine_.range(),
                               cabacEngine_.offset());
            cabacEngine_.setTrace(&trace_, cabacCtx_.data());
#endif
            ++cabacSliceCount_;

            // Initialize per-MB CABAC neighbor context
            cabacNeighbor_.init(widthInMbs_, heightInMbs_);

            // Bind the syntax element parser to engine + contexts + neighbors
            cabacParser_.bind(cabacEngine_, cabacCtx_, cabacNeighbor_);
        }

        // Set active frame: decode directly into DPB target when possible.
        // Both I-slices and P-slices decode into the DPB target, eliminating
        // the ~460KB frame copy. For intra-in-P, neighbourhood pixels are
        // already in decodeTarget from previous inter MBs.
        if (decodeTarget && decodeTarget->isAllocated())
            activeFrame_ = decodeTarget;
        else
            activeFrame_ = &currentFrame_;

        // Decode macroblocks
        if (sh.sliceType_ == SliceType::I)
        {
            // QPY accumulates across MBs — ITU-T H.264 §7.4.5.
            int32_t mbQp = sliceQp;
            // §9.3.3.1.1.5: prevMbQpDelta resets to 0 at slice start. [CHECKED §9.3.3.1.1.5]
            prevMbHadNonZeroQpDelta_ = false;
            for (uint32_t mbAddr = sh.firstMbInSlice_; mbAddr < totalMbs; ++mbAddr)
            {
                uint32_t mbX = mbAddr % widthInMbs_;
                uint32_t mbY = mbAddr / widthInMbs_;

                if (useCabac)
                {
                    // Note: do NOT gate on br.isExhausted() — CABAC's 16-bit
                    // lookahead means the engine still holds valid bins after
                    // the underlying BitReader appears exhausted. Termination
                    // is via end_of_slice_flag (§7.3.4) below.
#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                    std::fprintf(stderr, "MB_START(%u,%u): R=%u O=%u bp=%u qp=%d\n",
                        mbX, mbY, cabacEngine_.range(), cabacEngine_.offset(),
                        static_cast<uint32_t>(cabacEngine_.bitPosition()), mbQp);
#endif
                    trace_.onMbStart(mbX, mbY, 200U,
                                     static_cast<uint32_t>(cabacEngine_.bitPosition()));
                    int64_t cT0 = profile_ ? sub0h264TimerUs() : 0;
                    if (!decodeCabacIntraMb(br, *sps, *pps, sh, mbQp, mbX, mbY))
                        break;
                    if (profile_) profile_->intraPredUs += sub0h264TimerUs() - cT0;

                    // §7.3.4: decode end_of_slice_flag after each MB
                    if (mbAddr < totalMbs - 1U)
                    {
                        if (cabacEngine_.decodeTerminate() == 1U)
                            break; // end of slice
                    }
                }
                else
                {
                    int64_t intraT0 = profile_ ? sub0h264TimerUs() : 0;
                    if (!decodeIntraMb(br, *sps, *pps, sh, mbQp, mbX, mbY))
                        break;
                    if (profile_) profile_->intraPredUs += sub0h264TimerUs() - intraT0;
                }
                // Trace per-MB bit offset for alignment debugging (type 201)
                trace_.onMbStart(mbX, mbY, 201U, static_cast<uint32_t>(br.bitOffset()));
                // Store accumulated QP for deblocking pass — ITU-T H.264 §8.7.2.2.
                mbQps_[mbAddr] = mbQp;
            }
        }
        else if (sh.sliceType_ == SliceType::P)
        {
            // Build L0 reference list per §8.2.4.2.1, apply reordering per §8.2.4.3.
            uint32_t maxFrameNum = 1U << sps->bitsInFrameNum_;
            dpb_.buildRefListL0(sh.frameNum_, maxFrameNum,
                                sh.numReorderCmdsL0_,
                                sh.refPicListModificationL0_ ? sh.reorderCmdsL0_ : nullptr);

            // Get reference frame AFTER building L0 list (avoids stale reference)
#if SUB0H264_TRACE
            // (L0 list built — per-partition ref lookup via getRef() in doPartitionMC)
#endif
            refFrame = dpb_.getReference(0U);
            if (!refFrame)
                return DecodeStatus::Error;

            // QPY accumulates across MBs — ITU-T H.264 §7.4.5.
            int32_t mbQp = sliceQp;
            prevMbHadNonZeroQpDelta_ = false; // §9.3.3.1.1.5: reset at slice start

            if (useCabac)
            {
                // CABAC P-slice: per-MB skip flag instead of skip run
                for (uint32_t mbAddr = sh.firstMbInSlice_; mbAddr < totalMbs; ++mbAddr)
                {
                    uint32_t mbX = mbAddr % widthInMbs_;
                    uint32_t mbY = mbAddr / widthInMbs_;

                    // CABAC has 16-bit lookahead beyond the underlying BitReader's
                    // current bitOffset, so isExhausted() returns true while the
                    // engine still holds valid bins. Don't gate the loop on it;
                    // the end_of_slice_flag (decoded after each MB) is the proper
                    // termination per §7.3.4. Removing this fixed P-frame decode
                    // truncating ~3 MBs early on Tapo C110 (last MB(36,22) → MB(39,22)).

                    // Emit CABAC MB-start trace (a=200, b=bitPosition).
                    // Allows tests to verify the CABAC bit-position at the start of each MB.
                    trace_.onMbStart(mbX, mbY, 200U,
                                     static_cast<uint32_t>(cabacEngine_.bitPosition()));

                    // Decode mb_skip_flag — §9.3.3.1.1.1
                    bool leftSkip, topSkip;
                    cabacNeighbor_.skipCtx(mbX, mbY, leftSkip, topSkip);
                    uint32_t skipFlag = cabacDecodeMbSkipP(cabacEngine_, cabacCtx_.data(),
                                                           leftSkip, topSkip);

                    if (skipFlag)
                    {
                        cabacNeighbor_[mbAddr].setSkip(true);
                        cabacNeighbor_[mbAddr].setI4x4(false);
                        cabacNeighbor_[mbAddr].setTransform8x8(false);
                        // Skip MBs have no residual → CBP = 0, no DC CBF.
                        // Must set explicitly so neighbor CBP context derivation
                        // (§9.3.3.1.1.4) sees the skip MB as "not coded".
                        cabacNeighbor_[mbAddr].cbp = 0U;
                        cabacNeighbor_[mbAddr].dcCbf = 0U;
                        // §9.3.3.1.1.5 / JM cabac.c L604: skip MBs reset
                        // prevHadDelta to false (mb_qp_delta is absent).
                        prevMbHadNonZeroQpDelta_ = false;
                        int64_t skipT0 = profile_ ? sub0h264TimerUs() : 0;
                        decodePSkipMb(*decodeTarget, *refFrame, mbX, mbY, sh);
                        if (profile_) profile_->interPredUs += sub0h264TimerUs() - skipT0;
                        // Skip MBs inherit QP — no mb_qp_delta per §7.4.5.
                    }
                    else
                    {
                        cabacNeighbor_[mbAddr].setSkip(false);

                        // §9.3.3.1.2: condTermFlagN = 0 for unavailable OR I_NxN
                        bool leftCondZero, topCondZero;
                        cabacNeighbor_.mbTypeCtxI(mbX, mbY, leftCondZero, topCondZero);
                        uint32_t mbTypeRaw = cabacDecodeMbTypeP(cabacEngine_, cabacCtx_.data(),
                                                                 leftCondZero, topCondZero);

                        if (mbTypeRaw >= 5U)
                        {
                            // Intra in CABAC P-slice: decode using CABAC intra path
                            uint32_t intraMbType = mbTypeRaw - 5U;
                            cabacNeighbor_[mbAddr].setI4x4(intraMbType == 0U);
                            uint32_t mbIdx = mbAddr;
                            setMbMotion(mbIdx, {0, 0}, -1);
                            bool ok;
                            if (intraMbType == 0U)
                                ok = decodeCabacI4x4Mb(*sps, *pps, mbQp, mbX, mbY);
                            else if (intraMbType == 25U)
                            {
                                // I_PCM (§9.3.3.1.1): raw 256 Y + 64 Cb + 64 Cr
                                // bytes follow the mb_type, byte-aligned. NOT
                                // YET IMPLEMENTED — decode produces undefined
                                // pixels for I_PCM macroblocks. Stream decode
                                // cannot continue correctly because subsequent
                                // bits are misaligned.
                                SUB0H264_UNSUPPORTED(
                                    "CABAC I_PCM (§9.3.3.1.1) not implemented — "
                                    "subsequent bitstream is misaligned; decoder "
                                    "cannot continue safely.");
                                ok = false;
                            }
                            else
                                ok = decodeCabacI16x16Mb(*sps, *pps, intraMbType, mbQp, mbX, mbY);
                            if (!ok)
                                break;
                        }
                        else
                        {
                            cabacNeighbor_[mbAddr].setI4x4(false);
                            int64_t ciT0 = profile_ ? sub0h264TimerUs() : 0;
                            decodeCabacPInterMb(br, *sps, *pps, sh, mbQp,
                                                mbTypeRaw, *decodeTarget, mbX, mbY);
                            if (profile_) profile_->interPredUs += sub0h264TimerUs() - ciT0;
                        }
                    }

                    // §7.3.4: decode end_of_slice_flag after each MB
                    if (mbAddr < totalMbs - 1U)
                    {
                        if (cabacEngine_.decodeTerminate() == 1U)
                            break;
                    }

                    // Store accumulated QP for deblocking pass — §8.7.2.2.
                    mbQps_[mbAddr] = mbQp;
                }
            }
            else
            {
                // CAVLC P-slice: skip run based
                uint32_t mbSkipRun = 0U;
                bool needSkipRun = true;

                //TODO: We should reason about how to optimize the skip_run handling in CAVLC P-slice decoding. Currently we read skip_run at the start of the loop when needSkipRun is true, and then decrement mbSkipRun for each skipped MB until it reaches zero. This means we check the needSkipRun condition on every MB, which adds some overhead. We could consider restructuring this to read skip_run once and then have an inner loop that processes all the skipped MBs before reading the next skip_run, which would reduce the number of condition checks and might be more efficient. Alternatively, we could encapsulate the skip_run logic into a helper function that manages the state and returns whether the current MB is skipped or coded, which would simplify the main decoding loop and potentially improve readability.
                for (uint32_t mbAddr = sh.firstMbInSlice_; mbAddr < totalMbs; ++mbAddr)
                {
                    uint32_t mbX = mbAddr % widthInMbs_;
                    uint32_t mbY = mbAddr / widthInMbs_;

                    //TODO: Optimize - occurs always first iteraton, we can just read this once before the loop and then only read again when needed.
                    //TODO: We should reason about the best way to handle the skip_run state in the CAVLC P-slice decoding. Currently we have a boolean flag needSkipRun that indicates when we need to read a new skip_run value from the bitstream, and then we decrement mbSkipRun for each skipped MB until it reaches zero. This works, but it might be cleaner to structure this as an explicit loop that reads skip_run and then processes that many skipped MBs before reading the next coded MB. This would more closely follow the structure of the bitstream and might be easier to understand and maintain. We could also consider encapsulating this logic into a helper function that manages the skip_run state and returns whether the current MB is skipped or coded, which would simplify the main decoding loop.
                    if (needSkipRun)
                    {
                        mbSkipRun = br.readUev();
                        trace_.onMbStart(mbX, mbY, 98U, mbSkipRun); // 98 = skip_run value
                        needSkipRun = false;
                    }

                    if (mbSkipRun > 0U)
                    {
                        trace_.onMbStart(mbX, mbY, 99U, static_cast<uint32_t>(br.bitOffset()));
                        int64_t mcT0 = profile_ ? sub0h264TimerUs() : 0;
                        decodePSkipMb(*decodeTarget, *refFrame, mbX, mbY, sh);
                        if (profile_) profile_->interPredUs += sub0h264TimerUs() - mcT0;
                        --mbSkipRun;
                        // §7.3.4: after skip_run exhausted, next MB is coded
                        // (mb_type follows, NOT another skip_run).
                    }
                    else
                    {
                        trace_.onMbStart(mbX, mbY, 100U, static_cast<uint32_t>(br.bitOffset()));
                        uint32_t mbTypeRaw = br.readUev();
                        needSkipRun = true; // After coded MB, read next skip_run

                        trace_.onMbStart(mbX, mbY, mbTypeRaw, static_cast<uint32_t>(br.bitOffset()));

                        //TODO: We should reason about how to unify the intra-in-P handling between CAVLC and CABAC. Currently we have separate code paths for each, which leads to some duplication and potential for divergence. We could consider refactoring to have a common function for decoding intra MBs in P-slices that is called from both CAVLC and CABAC paths, with the only difference being how we read the mb_type (raw value from CAVLC vs decoded value from CABAC). This would help ensure consistency in intra MB handling across both entropy coding methods and reduce code duplication.
                        if (mbTypeRaw >= 5U)
                        {
                            mbTypeRaw -= 5U;
                            if (!decodeIntraMbInPSlice(br, *sps, *pps, sh, mbQp,
                                                        mbTypeRaw, *decodeTarget, mbX, mbY))
                                break;
                        }
                        else
                        {
                            int64_t interT0 = profile_ ? sub0h264TimerUs() : 0;
                            decodePInterMb(br, *sps, *pps, sh, mbQp,
                                           mbTypeRaw, *decodeTarget, mbX, mbY,
                                           sh.numRefIdxActiveL0_);
                            if (profile_) profile_->interPredUs += sub0h264TimerUs() - interT0;
                        }
                    }
                    // Store accumulated QP for deblocking pass — §8.7.2.2.
                    mbQps_[mbAddr] = mbQp;
                }
            }
        }
        else
        {
            // B-slices, SI/SP slices not implemented
            SUB0H264_UNSUPPORTED(
                "Only I-slices and P-slices are implemented. "
                "B-slices (§7.3.4), SI-slices, SP-slices are not supported.");
            return DecodeStatus::Error;
        }


        // Deblocking filter pass (entire frame, after all MBs decoded)
        // Per-MB QP is used; boundary edges use (qpP + qpQ + 1) >> 1 per §8.7.2.2.
        // §7.3.3: disable_deblocking_filter_idc == 2 means filter within slice
        // but NOT across slice boundaries. We don't implement cross-slice skip
        // (single-slice streams only). Gracefully error if idc==2 is encountered.
        if (sh.disableDeblockingFilter_ == 2U)
        {
            SUB0H264_UNSUPPORTED(
                "disable_deblocking_filter_idc==2 (no cross-slice filtering) "
                "not implemented — would require per-MB slice membership "
                "tracking §8.7");
            return DecodeStatus::Error;
        }
#ifndef SUB0H264_SKIP_DEBLOCK
        if (pps->deblockingFilterControlPresent_ == 0U ||
            sh.disableDeblockingFilter_ != 1U)
#else
        if (false)
#endif
        {
            int64_t dbT0 = profile_ ? sub0h264TimerUs() : 0;
            int32_t alphaOff = sh.sliceAlphaC0Offset_;
            int32_t betaOff = sh.sliceBetaOffset_;

            // activeFrame_ always points to the frame where all MBs were decoded.
            Frame& dbFrame = *activeFrame_;

            for (uint32_t my = 0U; my < heightInMbs_; ++my)
            {
                for (uint32_t mx = 0U; mx < widthInMbs_; ++mx)
                {
                    //TODO: We should reason about how to efficiently determine intra vs inter MBs for deblocking. Currently we check the reference index of the first 4x4 block's MV, which is a common heuristic since intra MBs have no references. However, this relies on the assumption that all blocks in an intra MB are marked with refIdx = -1, which should be true but we should verify this is always the case in our decoding logic. Alternatively, we could maintain a separate per-MB flag during decoding to explicitly track intra vs inter MBs, which might be more robust and slightly faster to check during deblocking.
                    //TODO: We should also consider the implications of this intra/inter check for edge cases, such as MBs with mixed intra/inter blocks (if such a thing is possible), or how this interacts with the I_NxN types in P-slices. We should verify that our decoding logic correctly sets the refIdx for all blocks in an intra MB to -1, and that we don't have any cases where an inter MB might have a block with refIdx = -1 that could cause a false positive for intra detection.
                    //TODO: We could also consider if there are any performance implications of this check, especially if we have a large number of MBs to deblock. If this becomes a bottleneck, we might want to optimize by maintaining an explicit intra/inter flag per MB during decoding, as mentioned above.
                    bool mbIsIntra = (mbMotion_[(my * widthInMbs_ + mx) * 16U].refIdx == -1);
                    deblockMb(dbFrame, mx, my,
                              mbIsIntra, alphaOff, betaOff,
                              nnzLuma_.data(), mbMotion_.data(),
                              mbQps_.data(), mbTransform8x8_.data(),
                              pps->chromaQpIndexOffset_,
                              widthInMbs_, heightInMbs_);
                }
            }
            if (profile_) profile_->deblockUs += sub0h264TimerUs() - dbT0;
        }

        //TODO: Is this the right place for this? It needs to be after deblocking, but should it be here or at the end of the function?
        //TODO: We should reason about the memory management strategy here - currently we have the DPB manage frame buffers and we copy to currentFrame_ for output. With the activeFrame_ refactor, we decode directly into the DPB target when possible, eliminating the copy. We should consider if we can eliminate currentFrame_ entirely and just have currentFrame() return a pointer to the DPB frame when available.
        //TODO: We could also consider a more explicit memory management strategy where we have a pool of frame buffers and we manage allocation and reuse more directly, rather than having the DPB implicitly manage frame buffers. This could allow for better performance and memory locality, especially on embedded platforms.
        //TODO: We should also consider the implications of multi-threaded decoding on memory management and synchronization, especially if we want to decode multiple frames in parallel or use separate threads for decoding and output.
        // Frame sync: only needed if activeFrame_ differs from decodeTarget.
        // With the activeFrame_ refactor, I-slices decode directly into DPB
        // when possible, eliminating the ~460KB copy.
        int64_t syncT0 = profile_ ? sub0h264TimerUs() : 0;
        if (activeFrame_ != decodeTarget && decodeTarget->isAllocated())
        {
            // Fallback: activeFrame_ was currentFrame_ → copy to DPB
            std::memcpy(decodeTarget->yData(), activeFrame_->yData(),
                        activeFrame_->yStride() * activeFrame_->height());
            std::memcpy(decodeTarget->uData(), activeFrame_->uData(),
                        activeFrame_->uvStride() * (activeFrame_->height() / 2U));
            std::memcpy(decodeTarget->vData(), activeFrame_->vData(),
                        activeFrame_->uvStride() * (activeFrame_->height() / 2U));
        }

        if (profile_) profile_->overheadUs += sub0h264TimerUs() - syncT0;

        //TODO: We should reason about when frames are marked as reference and when they are output. Currently we mark as reference here after decoding, but before output. This means that the current frame is available as a reference for the next frame before it is output. This is necessary for correct reference management, but we should consider if there are any implications for output latency or memory management, especially if we want to support low-latency streaming or real-time applications.
        // Mark as reference for future P-frames — §8.2.5
        if (nal.refIdc != 0U)
        {
            dpb_.markAsReference(sh.frameNum_);
            prevRefFrameNum_ = sh.frameNum_;

            // Apply MMCO commands for non-IDR adaptive marking — §8.2.5.4
            if (!sh.isIdr_ && sh.decRefPicMarking_.adaptiveRefPicMarking_ &&
                sh.decRefPicMarking_.numMmcoCommands_ > 0U)
            {
                uint32_t maxFrameNum = 1U << sps->bitsInFrameNum_;
                dpb_.applyMmco(sh.frameNum_, maxFrameNum,
                               sh.decRefPicMarking_.numMmcoCommands_,
                               sh.decRefPicMarking_.mmcoCommands_);
            }
        }

        ++frameCount_;
        if (profile_) ++profile_->frameCount;
        return DecodeStatus::FrameDecoded;
    }

    /** Decode one intra macroblock.
     *  @param currentQp  [in/out] Accumulated QP — updated by mb_qp_delta.
     *                    ITU-T H.264 §7.4.5: QPY is accumulated across MBs.
     */
    bool decodeIntraMb(BitReader& br, const Sps& sps, const Pps& pps,
                       const SliceHeader& sh, int32_t& currentQp,
                       uint32_t mbX, uint32_t mbY) noexcept
    {
        //TODO: is sh needed here? We need to pass it in for intraPred16x16 and intraPred4x4, but is there any other reason we need it in this function? If not, we could consider only passing the necessary parameters to intraPred16x16 and intraPred4x4 and not the entire sh, which would make those functions more modular and easier to test. For example, we could extract the relevant parameters from the sh (such as slice type, QP, etc.) and pass those directly to the prediction functions, rather than passing the entire sh object.
        (void)sh;

        //TODO: Is hasBits ewrroneously guarding the wrong thing here? We currently check hasBits(1) before reading mb_type, but mb_type is a ue(v) which can be more than 1 bit. We should verify that we are correctly checking for sufficient bits in the bitstream before attempting to read mb_type, and that we are accounting for the variable-length nature of the ue(v) coding. It might be more appropriate to check for a minimum number of bits that would be required to read a valid mb_type, or to handle the case where we attempt to read mb_type and catch any errors or exceptions that indicate insufficient bits.
        if (!br.hasBits(1U))
            return false;

        // Read mb_type
        uint32_t mbTypeRaw = br.readUev();

        // Check for end-of-slice (bitstream exhausted)
        if (br.isExhausted())
            return false;

#if SUB0H264_TRACE
        if (mbY == 0U && mbX < 12U)
            std::printf("[DBG] MB(%lu,0) mbType=%lu bitOff=%lu (start=%lu)\n",
                (unsigned long)mbX, (unsigned long)mbTypeRaw,
                (unsigned long)br.bitOffset(),
                (unsigned long)(br.bitOffset() - (mbTypeRaw < 4U ? 5U : 1U)));
        if (mbY == 0U && (mbX == 7U || mbX == 8U))
        {
            std::printf("[DBG]   MB(%lu,0) DETAIL: after mbType bitOff=%lu\n",
                (unsigned long)mbX, (unsigned long)br.bitOffset());
        }
#endif

        if (mbTypeRaw == 25U)
        {
            // CAVLC I_PCM (§7.3.5 / §7.3.5.3): raw 256 Y + 64 Cb + 64 Cr
            // bytes follow mb_type after pcm_alignment_zero_bit padding to
            // byte boundary. NOT IMPLEMENTED — subsequent bits misaligned
            // and decoder cannot continue safely.
            SUB0H264_UNSUPPORTED(
                "CAVLC I_PCM (§7.3.5) not implemented — subsequent "
                "bitstream is misaligned; decoder cannot continue safely.");
            return false;
        }

        if (isI16x16(static_cast<uint8_t>(mbTypeRaw)))
        {
            // I_16x16 macroblock
            return decodeI16x16Mb(br, sps, pps, mbTypeRaw, currentQp, mbX, mbY);
        }
        else
        {
            // I_4x4 macroblock (mbType == 0)
            return decodeI4x4Mb(br, sps, pps, currentQp, mbX, mbY);
        }
    }

    /** Dequantize a 4x4 block using the slice's resolved scaling list for
     *  the given plane/intra-vs-inter selector. Falls through to the flat-16
     *  fast path when the active list is flat (no per-coefficient multiply).
     *  See §8.5.12.1 / §7.4.2.1.1.
     */
    void dequantize4x4(int16_t* coeffs, int32_t qp, Block4x4Plane plane) noexcept
    {
        if (isFlat4x4(scalingLists_, plane))
            inverseQuantize4x4(coeffs, qp);
        else
            inverseQuantize4x4Scaled(coeffs, qp, select4x4(scalingLists_, plane));
    }

    /** Dequantize an 8x8 luma block (Y intra or Y inter). */
    void dequantize8x8(int16_t* coeffs, int32_t qp, bool isInter) noexcept
    {
        const uint32_t idx = isInter ? 1U : 0U;
        if (scalingLists_.isFlat8x8[idx])
            inverseQuantize8x8(coeffs, qp);
        else
            inverseQuantize8x8Scaled(coeffs, qp, scalingLists_.list8x8[idx]);
    }

    /** Decode an I_16x16 macroblock. */
    bool decodeI16x16Mb(BitReader& br, const Sps& sps, const Pps& pps,
                         uint32_t mbTypeRaw, int32_t& qp,
                         uint32_t mbX, uint32_t mbY) noexcept
    {
        //TODO: Is sps needed here? We need to pass it in for intraPred16x16, but is there any other reason we need it in this function? If not, we could consider only passing the necessary parameters to intraPred16x16 and not the entire sps, which would make the function more modular and easier to test. For example, we could extract the relevant parameters from the sps (such as width, height, etc.) and pass those directly to intraPred16x16, rather than passing the entire sps object.
        (void)sps;
        uint8_t predMode = i16x16PredMode(static_cast<uint8_t>(mbTypeRaw));
        uint8_t cbpLuma  = i16x16CbpLuma(static_cast<uint8_t>(mbTypeRaw));
        uint8_t cbpChroma = i16x16CbpChroma(static_cast<uint8_t>(mbTypeRaw));

        // Zero NNZ for this MB — blocks not coded retain 0 for correct
        // neighbor context in adjacent MBs' CAVLC decode (§9.2.1).
        {
            uint32_t mbIdx = mbY * widthInMbs_ + mbX;
            std::fill_n(&nnzLuma_[mbIdx * 16U], 16U, static_cast<uint8_t>(0U));
            std::fill_n(&nnzCb_[mbIdx * 4U], 4U, static_cast<uint8_t>(0U));
            std::fill_n(&nnzCr_[mbIdx * 4U], 4U, static_cast<uint8_t>(0U));
        }

        // §7.3.5 macroblock_layer() sequencing for I_16x16: [CHECKED §7.3.5]
        //   Step 1 — mb_type: decoded in caller (encodes predMode, cbpLuma, cbpChroma).
        //   Step 2 — mb_pred: intra_chroma_pred_mode ue(v) below [CHECKED §7.3.5.1]
        //             NOTE (FM-3): coded_block_pattern is ABSENT for I_16x16 —
        //             it is embedded in mb_type via i16x16CbpLuma/i16x16CbpChroma.
        //   Step 3 — mb_qp_delta: ALWAYS present for I_16x16 (below). [CHECKED §7.3.5]
        //   Step 4 — residual: DC + AC luma, then DC + AC chroma. [CHECKED §7.3.5.3]
        // §7.3.5.1 mb_pred for Intra_16x16: intra_chroma_pred_mode ue(v).
#if SUB0H264_TRACE
        if (mbY == 0U && (mbX == 0U || mbX == 7U || mbX == 8U))
            std::printf("[DBG]   MB(%lu) Before chroma_pred: bitOff=%lu\n",
                (unsigned long)mbX, (unsigned long)br.bitOffset());
#endif
        uint32_t chromaPredMode = br.readUev();

#if SUB0H264_TRACE
        if (mbY == 0U && (mbX == 0U || mbX == 7U || mbX == 8U))
            std::printf("[DBG]   MB(%lu) After chroma_pred=%lu: bitOff=%lu\n",
                (unsigned long)mbX, (unsigned long)chromaPredMode, (unsigned long)br.bitOffset());
#endif

        // §7.3.5: mb_qp_delta always present for Intra_16x16. [CHECKED §7.3.5]
        int32_t qpDelta = br.readSev();
#if SUB0H264_TRACE
        if (mbY == 0U && (mbX == 0U || mbX == 7U || mbX == 8U))
            std::printf("[DBG]   MB(%lu) After qpDelta=%d: bitOff=%lu\n",
                (unsigned long)mbX, qpDelta, (unsigned long)br.bitOffset());
#endif
        qp += qpDelta;
        //TODO: We should verify that this modular wrapping logic for QP is correct and follows the H.264 specification. The current implementation uses a common technique for modular arithmetic that handles negative values correctly, but we should ensure that it behaves as expected for all possible QP deltas and initial QP values. We might want to write some unit tests for this function to verify that it correctly wraps QP values into the 0-51 range for a variety of inputs.
        qp = ((qp % 52) + 52) % 52; // Proper modular wrapping for any delta

        // 1. Generate 16x16 luma prediction
        uint8_t lumaPred[256];
        intraPred16x16(static_cast<Intra16x16Mode>(predMode),
                       *activeFrame_, mbX, mbY, lumaPred);

#if SUB0H264_TRACE
        if (mbY == 0U && (mbX == 7U || mbX == 8U))
        {
            int32_t preNc = getLumaNc(mbX, mbY, 0U);
            uint32_t peek1 = br.peekBits(1U);
            uint32_t peek8 = br.peekBits(8U);
            std::printf("[DBG]   MB(%lu) DC block: bitOff=%lu nC=%d peek1=%u peek8=0x%02x\n",
                (unsigned long)mbX, (unsigned long)br.bitOffset(), preNc, peek1, peek8);
        }
#endif

        // 2. Decode luma DC block (4x4 Hadamard)
        int32_t dcNc = getLumaNc(mbX, mbY, 0U);
        ResidualBlock4x4 dcBlock;
        decodeResidualBlock4x4(br, dcNc, cMaxCoeff4x4, 0U, dcBlock);

        int16_t dcCoeffs[16];
        for (uint32_t i = 0U; i < 16U; ++i)
            dcCoeffs[i] = dcBlock.coeffs[i];

#if SUB0H264_TRACE
        // Temporary debug: trace DC decode for first MB
        if (mbX == 0U && mbY == 0U)
        {
            std::printf("[DBG] MB(0,0) I16x16: mbType=%lu pred=%u cbpL=%u cbpC=%u qp=%d bitOff=%lu\n",
                (unsigned long)mbTypeRaw, predMode, cbpLuma, cbpChroma, qp,
                (unsigned long)br.bitOffset());
            std::printf("[DBG]   DC block totalCoeff=%u nC=%d: ", dcBlock.totalCoeff, dcNc);
            for (uint32_t k = 0U; k < 16U; ++k) std::printf("%d ", dcBlock.coeffs[k]);
            std::printf("\n[DBG]   pred[0]=%u bitOff_after_dc=%lu\n", lumaPred[0],
                (unsigned long)br.bitOffset());
            // Peek at next few bits
            uint32_t peek = br.peekBits(16U);
            std::printf("[DBG]   next 16 bits: 0x%04x\n", peek);
        }
#endif

        // Inverse Hadamard
        inverseHadamard4x4(dcCoeffs);

        // §8.5.12.1: Intra16x16 luma DC dequant after Hadamard. Scaling list
        // weightScale[0][0] applied via the dequant helper's second arg.
        dequantLumaDcValues(dcCoeffs, qp, scalingLists_.list4x4[0][0]);

#if SUB0H264_TRACE
        if (mbX == 0U && mbY == 0U)
        {
            std::printf("[DBG]   After Hadamard+dequant DC: ");
            for (uint32_t k = 0U; k < 16U; ++k) std::printf("%d ", dcCoeffs[k]);
            std::printf("\n");
        }
#endif

        // 3. Decode and reconstruct each 4x4 luma sub-block
        uint8_t* mbLuma = activeFrame_->yMb(mbX, mbY);
        uint32_t yStride = activeFrame_->yStride();

        for (uint32_t blkIdx = 0U; blkIdx < 16U; ++blkIdx)
        {
            // H.264 §6.4.3: blocks iterate in spec scan order, not raster.
            uint32_t blkX = cLuma4x4BlkX[blkIdx];
            uint32_t blkY = cLuma4x4BlkY[blkIdx];
            uint32_t rasterIdx = cLuma4x4ToRaster[blkIdx];

            int16_t coeffs[16] = {};
            // Hadamard output is in raster order (4x4 grid row-major)
            coeffs[0] = dcCoeffs[rasterIdx]; // DC from Hadamard

            if (cbpLuma)
            {
                // Decode AC coefficients (start from index 1)
                int32_t nc = getLumaNc(mbX, mbY, rasterIdx);
                ResidualBlock4x4 acBlock;
                decodeResidualBlock4x4(br, nc, 15U, 1U, acBlock);

                // Merge AC into coeffs (skip DC at position 0)
                for (uint32_t i = 1U; i < 16U; ++i)
                    coeffs[i] = acBlock.coeffs[i];

                // Store NNZ for context (raster order)
                uint32_t mbIdx = mbY * widthInMbs_ + mbX;
                nnzLuma_[mbIdx * 16U + rasterIdx] = acBlock.totalCoeff;
            }

            // Inverse quantize AC coefficients only — DC was already dequantized
            // in the Hadamard path above. Dequanting position 0 again would
            // double-scale the DC coefficient (ITU-T H.264 §8.5.12.1).
            if (cbpLuma)
            {
                // Save DC, dequant all, restore DC (avoids conditional per-coeff)
                int16_t savedDc = coeffs[0];
                dequantize4x4(coeffs, qp, Block4x4Plane::LumaIntra);
                coeffs[0] = savedDc;
            }

            // Inverse DCT + add prediction + clip
            uint8_t* predPtr = lumaPred + blkY * 16U + blkX;
            uint8_t* outPtr = mbLuma + blkY * yStride + blkX;
            inverseDct4x4AddPred(coeffs, predPtr, 16U, outPtr, yStride);
        }

        // 4. Decode chroma (simplified — DC prediction + optional residual)
        decodeChromaMb(br, pps, chromaPredMode, cbpChroma, qp, mbX, mbY);

        return true;
    }

    /** Decode an I_4x4 macroblock. */
    /** Get intra 4x4 prediction mode for a neighboring block.
     *  Returns DC(2) if neighbor is unavailable or not intra-4x4.
     *  For cross-MB lookups, uses mbIntra4x4Modes_ which stores
     *  all previously decoded MB intra modes.
     *
     *  Block layout within MB (raster scan):
     *    0  1  2  3
     *    4  5  6  7
     *    8  9  10 11
     *    12 13 14 15
     *
     *  Reference: ITU-T H.264 §8.3.1.1
     */
    uint8_t getNeighborIntra4x4Mode(uint32_t mbX, uint32_t mbY,
                                     uint32_t blkIdx, bool isLeft,
                                     const uint8_t* curMbModes) const noexcept
    {
        /// Default intra 4x4 prediction mode when neighbor unavailable — ITU-T H.264 §8.3.1.1.
        static constexpr uint8_t cDefaultIntra4x4Mode = 2U; // DC

        uint32_t blkX = blkIdx & 3U;
        uint32_t blkY = blkIdx >> 2U;

        if (isLeft)
        {
            if (blkX > 0U)
                return curMbModes[blkIdx - 1U]; // Within same MB
            if (mbX == 0U)
                return cDefaultIntra4x4Mode; // No left MB
            // Left MB's rightmost column: same row, column 3
            uint32_t leftMbIdx = mbY * widthInMbs_ + (mbX - 1U);
            uint32_t leftBlk = blkY * 4U + 3U;
            return mbIntra4x4Modes_[leftMbIdx * 16U + leftBlk];
        }
        else // top
        {
            if (blkY > 0U)
                return curMbModes[blkIdx - 4U]; // Within same MB
            if (mbY == 0U)
                return cDefaultIntra4x4Mode; // No top MB
            // Top MB's bottom row: same column, row 3
            uint32_t topMbIdx = (mbY - 1U) * widthInMbs_ + mbX;
            uint32_t topBlk = 12U + blkX;
            return mbIntra4x4Modes_[topMbIdx * 16U + topBlk];
        }
    }

    bool decodeI4x4Mb(BitReader& br, const Sps& sps, const Pps& pps,
                       int32_t& qp, uint32_t mbX, uint32_t mbY) noexcept
    {
        (void)sps;
        // §7.3.5 macroblock_layer() sequencing for I_4x4/I_8x8: [CHECKED §7.3.5]
        //   Step 1 — mb_type: decoded in caller (mbTypeRaw==0 for I_NxN).
        //   Step 2 — transform_size_8x8_flag u(1): MISSING HERE — not decoded for CAVLC
        //             I_NxN when transform_8x8_mode_flag=1 in PPS. [UNCHECKED §7.3.5]
        //             (FM-3: missing syntax element. Benign only when pps.transform8x8Mode_==0.)
        //   Step 3 — mb_pred: intra4x4_pred_mode per block (below). [CHECKED §7.3.5.1]
        //   Step 4 — intra_chroma_pred_mode ue(v) (below). [CHECKED §7.3.5.1]
        //   Step 5 — coded_block_pattern me(v) (below, present for I_4x4). [CHECKED §7.3.5]
        //   Step 6 — mb_qp_delta se(v): only if cbp > 0. [CHECKED §7.3.5]
        //   Step 7 — residual(0,15). [CHECKED §7.3.5.3]
        // §7.3.5.1 mb_pred for I_NxN: prev_intra4x4_pred_mode_flag u(1) [+rem u(3)] per block.

        // §7.3.5 FM-3: transform_size_8x8_flag u(1) not decoded for CAVLC I_NxN.
        // If pps.transform8x8Mode_!=0, the bitstream contains a flag we don't read —
        // all subsequent bits shift by 1 and the entire MB decode is corrupted.
        if (pps.transform8x8Mode_ != 0U)
        {
            SUB0H264_UNSUPPORTED(
                "CAVLC I_NxN: §7.3.5 requires transform_size_8x8_flag u(1) "
                "before mb_pred when pps.transform8x8Mode_=1; not "
                "implemented (FM-3)");
            return false;
        }

        uint8_t mpmPerBlock[16] = {}; // saved for trace
        uint8_t predModes[16] = {};

#if SUB0H264_TRACE
        uint32_t predModeStartBit = 0U;
        if (mbX == 10U && mbY == 0U)
        {
            predModeStartBit = br.bitOffset();
            std::printf("[DBG] MB(10,0) pred modes start at bitOff=%lu\n",
                (unsigned long)predModeStartBit);
        }
#endif

        for (uint32_t i = 0U; i < 16U; ++i)
        {
            // Prediction modes are coded in spec scan order (§6.4.3).
            // Store in raster order for consistent neighbor lookup.
            uint32_t rasterIdx = cLuma4x4ToRaster[i];
            uint8_t leftMode = getNeighborIntra4x4Mode(mbX, mbY, rasterIdx, true, predModes);
            uint8_t topMode  = getNeighborIntra4x4Mode(mbX, mbY, rasterIdx, false, predModes);

            // §8.3.1.1: dcPredModePredictedFlag — when EITHER neighbor's macroblock
            // is unavailable, BOTH intraMxMPredModeA and intraMxMPredModeB are forced
            // to DC(2). The spec checks mbAddrA/mbAddrB availability, not individual
            // block availability. For edge blocks where the neighbor is in another MB,
            // getNeighborIntra4x4Mode returns DC(2) for unavailable. But for the OTHER
            // neighbor (which IS available, in the same MB), we must ALSO force DC(2)
            // when dcPredModePredictedFlag=1. [FM-22 fixed: was not forcing both to DC]
            bool leftMbUnavail = (rasterIdx % 4U == 0U) && (mbX == 0U);
            bool topMbUnavail  = (rasterIdx < 4U) && (mbY == 0U);
            if (leftMbUnavail || topMbUnavail)
            {
                leftMode = 2U; // DC
                topMode = 2U;  // DC
            }
            uint8_t mpm = (leftMode < topMode) ? leftMode : topMode;
            mpmPerBlock[rasterIdx] = mpm;

            uint32_t prevFlag = br.readBit();
            if (prevFlag)
            {
                predModes[rasterIdx] = mpm;
            }
            else
            {
                uint8_t rem = static_cast<uint8_t>(br.readBits(3U));
                predModes[rasterIdx] = (rem < mpm) ? rem : static_cast<uint8_t>(rem + 1U);
            }


#if SUB0H264_TRACE
            if (mbX == 10U && mbY == 0U && i < 4U)
                std::printf("[DBG]   blk%lu(raster%lu): prevFlag=%lu mpm=%u mode=%u\n",
                    (unsigned long)i, (unsigned long)rasterIdx, (unsigned long)prevFlag,
                    mpm, predModes[rasterIdx]);
#endif
        }

        // §7.3.5.1 mb_pred step 4: intra_chroma_pred_mode ue(v). [CHECKED §7.3.5.1]
        uint32_t chromaPredMode = br.readUev();

        // §7.3.5: coded_block_pattern me(v) — present for I_4x4 (NOT I_16x16). [CHECKED §7.3.5]
        uint32_t cbpCode = br.readUev();
        uint8_t cbp = 0U;
        if (cbpCode < 48U)
            cbp = cCbpTable[cbpCode][0]; // Intra CBP table (§9.1.2 Table 9-4)
        uint8_t cbpLuma = cbp & 0x0FU;
        uint8_t cbpChroma = (cbp >> 4U) & 0x03U;

        trace_.onBlockResidual(mbX, mbY, 99U, static_cast<int32_t>(cbpCode),
                               cbpLuma | (cbpChroma << 4U),
                               static_cast<uint32_t>(br.bitOffset()));

        // §7.3.5: mb_qp_delta se(v) — only if cbp > 0 for I_4x4. [CHECKED §7.3.5]
        // (FM-3: for I_16x16 this would always be present; for I_4x4 it is conditional.)
        if (cbp > 0U)
        {
            int32_t qpDelta = br.readSev();
            qp += qpDelta;
            qp = ((qp % 52) + 52) % 52; // §7.4.5: proper modular wrapping
        }

#if SUB0H264_TRACE
        if (mbX == 10U && mbY == 0U)
        {
            std::printf("[DBG] MB(10,0) I4x4: modes=[");
            for (uint32_t k = 0U; k < 16U; ++k) std::printf("%u ", predModes[k]);
            std::printf("] cbp=0x%02x cbpL=%u cbpC=%u qp=%d\n", cbp, cbpLuma, cbpChroma, qp);
        }
#endif

        // Zero NNZ for this MB — blocks not coded will retain 0.
        // ITU-T H.264 §9.2.1: nC context depends on neighboring NNZ.
        // Without this, uncoded blocks in I_4x4 MBs (groups where cbpLuma bit=0)
        // would use stale NNZ from the previous MB's decode, giving wrong nC
        // and causing CAVLC coeff_token misparse.
        {
            uint32_t mbIdx = mbY * widthInMbs_ + mbX;
            std::fill_n(&nnzLuma_[mbIdx * 16U], 16U, static_cast<uint8_t>(0U));
            std::fill_n(&nnzCb_[mbIdx * 4U], 4U, static_cast<uint8_t>(0U));
            std::fill_n(&nnzCr_[mbIdx * 4U], 4U, static_cast<uint8_t>(0U));
        }

        // Decode and reconstruct each 4x4 luma block
        uint8_t* mbLuma = activeFrame_->yMb(mbX, mbY);
        uint32_t yStride = activeFrame_->yStride();

        for (uint32_t blkIdx = 0U; blkIdx < 16U; ++blkIdx)
        {
            // H.264 §6.4.3: iterate in spec scan order, not raster.
            uint32_t blkX = cLuma4x4BlkX[blkIdx];
            uint32_t blkY = cLuma4x4BlkY[blkIdx];
            uint32_t rasterIdx = cLuma4x4ToRaster[blkIdx];

            // Generate 4x4 prediction
            uint8_t pred4x4[16];

            // Get neighbor samples for this 4x4 block — §8.3.1.1
            uint32_t absX = mbX * cMbSize + blkX;
            uint32_t absY = mbY * cMbSize + blkY;

            uint8_t topBuf[8], leftBuf[4], topLeftVal;
            const uint8_t *top, *topRight, *left, *topLeft;
            sampleIntra4x4Neighbors(absX, absY, blkIdx,
                                    topBuf, leftBuf, topLeftVal,
                                    top, topRight, left, topLeft);

            // predModes is stored in raster order
            intraPred4x4(static_cast<Intra4x4Mode>(predModes[rasterIdx]),
                         top, topRight, left, topLeft, pred4x4);

            // Trace: prediction mode with actual MPM
            trace_.emit({TraceEventType::BlockPredMode,
                         static_cast<uint16_t>(mbX), static_cast<uint16_t>(mbY),
                         blkIdx, rasterIdx, predModes[rasterIdx],
                         mpmPerBlock[rasterIdx], nullptr, 0U});

            // Decode residual if CBP indicates this 8x8 group has coefficients
            uint32_t group8x8 = blkIdx >> 2U; // scan order: 0-3=8x8_0, 4-7=8x8_1, ...
            bool hasResidual = (cbpLuma >> group8x8) & 1U;

            int16_t coeffs[16] = {};
            if (hasResidual)
            {
                int32_t nc = getLumaNc(mbX, mbY, rasterIdx);
                uint32_t blkBitBefore = br.bitOffset();
                ResidualBlock4x4 resBlock;
                decodeResidualBlock4x4(br, nc, cMaxCoeff4x4, 0U, resBlock);

#if SUB0H264_TRACE
                if (mbX == 9U && mbY == 0U)
                {
                    std::printf("[DBG]   MB9 luma scan%lu(raster%lu): nC=%d tc=%u bits=%lu\n",
                        (unsigned long)blkIdx, (unsigned long)rasterIdx,
                        nc, resBlock.totalCoeff,
                        (unsigned long)(br.bitOffset() - blkBitBefore));
                    if (blkIdx >= 12U)
                    {
                        std::printf("[DBG]     raw coeffs=[");
                        for (uint32_t k = 0U; k < 16U; ++k)
                            std::printf("%d ", resBlock.coeffs[k]);
                        std::printf("]\n");
                    }
                }
#endif

                for (uint32_t i = 0U; i < 16U; ++i)
                    coeffs[i] = resBlock.coeffs[i];

                // Trace: RAW coefficients (before dequant) — use BlockResidual data field
                trace_.emit({TraceEventType::BlockResidual,
                             static_cast<uint16_t>(mbX), static_cast<uint16_t>(mbY),
                             blkIdx, static_cast<uint32_t>(nc),
                             resBlock.totalCoeff,
                             br.bitOffset() - blkBitBefore, coeffs, 16U});

                uint32_t mbIdx = mbY * widthInMbs_ + mbX;
                nnzLuma_[mbIdx * 16U + rasterIdx] = resBlock.totalCoeff;

                dequantize4x4(coeffs, qp, Block4x4Plane::LumaIntra);

                // Trace: dequantized coefficients
                trace_.emit({TraceEventType::BlockCoeffs,
                             static_cast<uint16_t>(mbX), static_cast<uint16_t>(mbY),
                             blkIdx, 0U, 0U, 0U, coeffs, 16U});
            }

#if SUB0H264_TRACE
            if ((mbX == 1U || mbX == 3U) && mbY == 0U)
            {
                std::printf("[I4x4-DBG] MB(1,0) scan%lu raster%lu pos(%lu,%lu) mode=%u hasRes=%d\n",
                    (unsigned long)blkIdx, (unsigned long)rasterIdx,
                    (unsigned long)blkX, (unsigned long)blkY,
                    predModes[rasterIdx], hasResidual);
                std::printf("  pred=[%u %u %u %u / %u %u %u %u / %u %u %u %u / %u %u %u %u]\n",
                    pred4x4[0],pred4x4[1],pred4x4[2],pred4x4[3],
                    pred4x4[4],pred4x4[5],pred4x4[6],pred4x4[7],
                    pred4x4[8],pred4x4[9],pred4x4[10],pred4x4[11],
                    pred4x4[12],pred4x4[13],pred4x4[14],pred4x4[15]);
                if (hasResidual)
                {
                    std::printf("  dequant=[%d %d %d %d / %d %d %d %d / %d %d %d %d / %d %d %d %d]\n",
                        coeffs[0],coeffs[1],coeffs[2],coeffs[3],
                        coeffs[4],coeffs[5],coeffs[6],coeffs[7],
                        coeffs[8],coeffs[9],coeffs[10],coeffs[11],
                        coeffs[12],coeffs[13],coeffs[14],coeffs[15]);
                }
            }
            if (mbX == 9U && mbY == 0U && blkIdx >= 12U)
            {
                std::printf("[DBG]   MB9 scan%lu(raster%lu) pos(%lu,%lu): mode=%u pred=[%u %u %u %u] hasRes=%d qp=%d\n",
                    (unsigned long)blkIdx, (unsigned long)rasterIdx,
                    (unsigned long)blkX, (unsigned long)blkY,
                    predModes[rasterIdx],
                    pred4x4[0], pred4x4[1], pred4x4[2], pred4x4[3], hasResidual, qp);
                if (hasResidual)
                {
                    std::printf("[DBG]   coeffs (dequant)=[");
                    for (uint32_t k = 0U; k < 16U; ++k) std::printf("%d ", coeffs[k]);
                    std::printf("]\n");
                }
            }
#endif

            // Reconstruct: inverse DCT + prediction + clip
            uint8_t* outPtr = mbLuma + blkY * yStride + blkX;

#ifdef SUB0H264_TRACE_I4X4_BLOCKS
            if (mbX == 0U && mbY == 0U && blkIdx >= 7U && blkIdx <= 9U)
            {
                std::fprintf(stderr, "=== CAVLC BLK scan%u raster%u (%u,%u) mode=%u grp%u hasRes=%d ===\n",
                    blkIdx, rasterIdx, absX, absY, predModes[rasterIdx], blkIdx >> 2, hasResidual ? 1 : 0);
                std::fprintf(stderr, "  PRED_OUT=[%d %d %d %d][%d %d %d %d][%d %d %d %d][%d %d %d %d]\n",
                    pred4x4[0],pred4x4[1],pred4x4[2],pred4x4[3],
                    pred4x4[4],pred4x4[5],pred4x4[6],pred4x4[7],
                    pred4x4[8],pred4x4[9],pred4x4[10],pred4x4[11],
                    pred4x4[12],pred4x4[13],pred4x4[14],pred4x4[15]);
                if (hasResidual)
                    std::fprintf(stderr, "  COEFFS_DEQUANT=[%d %d %d %d][%d %d %d %d][%d %d %d %d][%d %d %d %d]\n",
                        coeffs[0],coeffs[1],coeffs[2],coeffs[3],
                        coeffs[4],coeffs[5],coeffs[6],coeffs[7],
                        coeffs[8],coeffs[9],coeffs[10],coeffs[11],
                        coeffs[12],coeffs[13],coeffs[14],coeffs[15]);
            }
#endif

            inverseDct4x4AddPred(coeffs, pred4x4, 4U, outPtr, yStride);

#ifdef SUB0H264_TRACE_I4X4_BLOCKS
            if (mbX == 0U && mbY == 0U && blkIdx >= 7U && blkIdx <= 9U)
            {
                std::fprintf(stderr, "  FRAME_AFTER=[%d %d %d %d][%d %d %d %d][%d %d %d %d][%d %d %d %d]\n",
                    outPtr[0], outPtr[1], outPtr[2], outPtr[3],
                    outPtr[yStride], outPtr[yStride+1], outPtr[yStride+2], outPtr[yStride+3],
                    outPtr[2*yStride], outPtr[2*yStride+1], outPtr[2*yStride+2], outPtr[2*yStride+3],
                    outPtr[3*yStride], outPtr[3*yStride+1], outPtr[3*yStride+2], outPtr[3*yStride+3]);
            }
#endif

            // Trace: prediction + output pixels
            if (trace_.hasCallback() && trace_.shouldTrace(mbX, mbY))
            {
                int16_t pixBuf[32]; // pred[16] then output[16]
                for (uint32_t k = 0; k < 16; ++k)
                    pixBuf[k] = pred4x4[k];
                for (uint32_t r = 0; r < 4; ++r)
                    for (uint32_t c = 0; c < 4; ++c)
                        pixBuf[16 + r * 4 + c] = outPtr[r * yStride + c];
                trace_.emit({TraceEventType::BlockPixels,
                             static_cast<uint16_t>(mbX), static_cast<uint16_t>(mbY),
                             blkIdx, 0U, 0U, 0U, pixBuf, 32U});
            }

#if SUB0H264_TRACE
            if (mbX == 9U && mbY == 0U && blkIdx >= 12U)
            {
                std::printf("[DBG]   output row0=[%u %u %u %u] row3=[%u %u %u %u]\n",
                    outPtr[0], outPtr[1], outPtr[2], outPtr[3],
                    outPtr[3*yStride], outPtr[3*yStride+1],
                    outPtr[3*yStride+2], outPtr[3*yStride+3]);
            }
#endif
        }

        // Store intra 4x4 modes for cross-MB MPM derivation
        {
            uint32_t mbIdx = mbY * widthInMbs_ + mbX;
            for (uint32_t k = 0U; k < 16U; ++k)
                mbIntra4x4Modes_[mbIdx * 16U + k] = predModes[k];
        }

        // Decode chroma
        decodeChromaMb(br, pps, chromaPredMode, cbpChroma, qp, mbX, mbY);

        return true;
    }

    /** Decode chroma for one macroblock. */
    void decodeChromaMb(BitReader& br, const Pps& pps,
                        uint32_t chromaPredMode, uint8_t cbpChroma,
                        int32_t qp, uint32_t mbX, uint32_t mbY) noexcept
    {
#if SUB0H264_TRACE
        if (mbY == 0U && (mbX < 2U || mbX == 9U))
            std::printf("[DBG] MB(%lu,0) chromaMb START bitOff=%lu cbpC=%u\n",
                (unsigned long)mbX, (unsigned long)br.bitOffset(), cbpChroma);
#endif
        auto chromaMode = static_cast<IntraChromaMode>(chromaPredMode);

        // Generate chroma predictions (8x8 for each plane)
        uint8_t predU[64], predV[64];
        intraPredChroma8x8(chromaMode, *activeFrame_, mbX, mbY, true, predU);
        intraPredChroma8x8(chromaMode, *activeFrame_, mbX, mbY, false, predV);

        // §7.4.2.2 Chroma QP (FM-10: must use chromaQp, not luma QP)
        int32_t chromaQp = computeChromaQp(qp, pps.chromaQpIndexOffset_);

        // Decode chroma DC if cbpChroma >= 1
        int16_t dcCb[4] = {}, dcCr[4] = {};
        if (cbpChroma >= 1U)
        {
            ResidualBlock4x4 dcBlockCb, dcBlockCr;
            decodeResidualBlock4x4(br, -1, 4U, 0U, dcBlockCb);
            decodeResidualBlock4x4(br, -1, 4U, 0U, dcBlockCr);

            for (uint32_t i = 0U; i < 4U; ++i)
            {
                dcCb[i] = dcBlockCb.coeffs[i];
                dcCr[i] = dcBlockCr.coeffs[i];
            }

            // Trace raw CAVLC-decoded DC coefficients (before Hadamard)
            if (trace_.shouldTrace(mbX, mbY))
            {
                trace_.onChromaDcRaw(mbX, mbY, dcCb, dcCr);
            }

            inverseHadamard2x2(dcCb);
            inverseHadamard2x2(dcCr);

            // Dequantize chroma DC — ITU-T H.264 §8.5.12.1. decodeChromaMb is
            // called from the intra path only → use CbIntra/CrIntra
            // weightScale[0][0] for the scaled variant.
            dequantChromaDcValues(dcCb, chromaQp, scalingLists_.list4x4[1][0]);
            dequantChromaDcValues(dcCr, chromaQp, scalingLists_.list4x4[2][0]);

            if (trace_.shouldTrace(mbX, mbY))
                trace_.onChromaDcDequant(mbX, mbY, dcCb, dcCr);
        }

        // ITU-T H.264 §7.3.5 residual_cavlc: all Cb AC blocks first, then all Cr AC blocks.
        // The outer loop over iCbCr (0=Cb, 1=Cr) is separate from the reconstruction loop.
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;

        // Stash per-block AC coefficients so we can separate decode from reconstruct.
        int16_t cbCoeffsBuf[4][16] = {};
        int16_t crCoeffsBuf[4][16] = {};

        // DC-only init from Hadamard output
        for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
        {
            cbCoeffsBuf[blkIdx][0] = dcCb[blkIdx];
            crCoeffsBuf[blkIdx][0] = dcCr[blkIdx];
        }

        // Decode all Cb AC blocks (iCbCr=0), then all Cr AC blocks (iCbCr=1).
        // Reference: ITU-T H.264 §7.3.5 residual_cavlc() syntax table.
        if (cbpChroma >= 2U)
        {
            for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
            {
                int32_t ncCb = getChromaNc(mbX, mbY, blkIdx, true);
                ResidualBlock4x4 acBlock;
                decodeResidualBlock4x4(br, ncCb, 15U, 1U, acBlock);
                for (uint32_t i = 1U; i < 16U; ++i)
                    cbCoeffsBuf[blkIdx][i] = acBlock.coeffs[i];
                nnzCb_[mbIdx * 4U + blkIdx] = acBlock.totalCoeff;
                // Save DC (already dequanted by Hadamard path), dequant AC, restore DC.
                // ITU-T H.264 §8.5.12.1: DC was scaled in the Hadamard dequant above.
                int16_t savedDcCb = cbCoeffsBuf[blkIdx][0];
                dequantize4x4(cbCoeffsBuf[blkIdx], chromaQp, Block4x4Plane::CbIntra);
                cbCoeffsBuf[blkIdx][0] = savedDcCb;
            }
            for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
            {
                int32_t ncCr = getChromaNc(mbX, mbY, blkIdx, false);
                ResidualBlock4x4 acBlock;
                decodeResidualBlock4x4(br, ncCr, 15U, 1U, acBlock);
                for (uint32_t i = 1U; i < 16U; ++i)
                    crCoeffsBuf[blkIdx][i] = acBlock.coeffs[i];
                nnzCr_[mbIdx * 4U + blkIdx] = acBlock.totalCoeff;
                int16_t savedDcCr = crCoeffsBuf[blkIdx][0];
                dequantize4x4(crCoeffsBuf[blkIdx], chromaQp, Block4x4Plane::CrIntra);
                crCoeffsBuf[blkIdx][0] = savedDcCr;
            }
        }

        // Reconstruct chroma 4x4 blocks using buffered coefficients.
        uint8_t* mbU = activeFrame_->uMb(mbX, mbY);
        uint8_t* mbV = activeFrame_->vMb(mbX, mbY);
        uint32_t uvStride = activeFrame_->uvStride();

        for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
        {
            uint32_t blkX = (blkIdx & 1U) * 4U;
            uint32_t blkY = (blkIdx >> 1U) * 4U;

            uint8_t* predCbPtr = predU + blkY * 8U + blkX;
            uint8_t* outCbPtr  = mbU + blkY * uvStride + blkX;
            inverseDct4x4AddPred(cbCoeffsBuf[blkIdx], predCbPtr, 8U, outCbPtr, uvStride);

            uint8_t* predCrPtr = predV + blkY * 8U + blkX;
            uint8_t* outCrPtr  = mbV + blkY * uvStride + blkX;
            inverseDct4x4AddPred(crCoeffsBuf[blkIdx], predCrPtr, 8U, outCrPtr, uvStride);
        }
    }

    // ── P-frame decode methods ──────────────────────────────────────────

    /** Fill all 16 4x4 blocks of an MB with the same MV (and optional MVD for CABAC context). */
    void setMbMotion(uint32_t mbIdx, MotionVector mv, int8_t refIdx,
                     MotionVector mvd = {0, 0}) noexcept
    {
        MbMotionInfo info = { mv, mvd, refIdx, true };
        for (uint32_t i = 0U; i < 16U; ++i)
            mbMotion_[mbIdx * 16U + i] = info;
    }

    /** Fill a partition's 4x4 blocks with MV + MVD — ITU-T H.264 §6.4.2.1.
     *
     *  Covers all P-slice mb_type values (Table 7-14):
     *   0: P_L0_16x16     — 1 partition, fills all 16 blocks
     *   1: P_L0_L0_16x8   — 2 partitions, each fills 2 rows × 4 cols = 8 blocks
     *   2: P_L0_L0_8x16   — 2 partitions, each fills 4 rows × 2 cols = 8 blocks
     *   3: P_8x8           — 4 sub-partitions, each fills 2×2 = 4 blocks
     *   4: P_8x8ref0       — same as P_8x8
     *
     *  @param partIdx  Partition index (0-1 for 16x8/8x16, 0-3 for P_8x8, 0 for 16x16)
     *  @param mvd      Decoded MVD — stored for CABAC §9.3.3.1.1.7 neighbor context
     */
    void setPartitionMotion(uint32_t mbIdx, uint32_t mbType, uint32_t partIdx,
                            MotionVector mv, int8_t refIdx,
                            MotionVector mvd = {0, 0}) noexcept
    {
        MbMotionInfo info = { mv, mvd, refIdx, true };
        switch (mbType)
        {
        case 0U: // P_L0_16x16: all 16 blocks
            for (uint32_t i = 0U; i < 16U; ++i)
                mbMotion_[mbIdx * 16U + i] = info;
            break;
        case 1U: // P_L0_L0_16x8: 2 rows per partition
        {
            uint32_t startRow = partIdx * 2U;
            for (uint32_t r = startRow; r < startRow + 2U; ++r)
                for (uint32_t c = 0U; c < 4U; ++c)
                    mbMotion_[mbIdx * 16U + r * 4U + c] = info;
            break;
        }
        case 2U: // P_L0_L0_8x16: 2 cols per partition
        {
            uint32_t startCol = partIdx * 2U;
            for (uint32_t r = 0U; r < 4U; ++r)
                for (uint32_t c = startCol; c < startCol + 2U; ++c)
                    mbMotion_[mbIdx * 16U + r * 4U + c] = info;
            break;
        }
        case 3U: // P_8x8
        case 4U: // P_8x8ref0
        {
            // 2x2 group of 4x4 blocks per sub-partition (§6.4.2.1)
            // Layout: 0=TL(r0-1,c0-1), 1=TR(r0-1,c2-3), 2=BL(r2-3,c0-1), 3=BR(r2-3,c2-3)
            uint32_t startRow = (partIdx >> 1U) * 2U;
            uint32_t startCol = (partIdx & 1U) * 2U;
            for (uint32_t r = startRow; r < startRow + 2U; ++r)
                for (uint32_t c = startCol; c < startCol + 2U; ++c)
                    mbMotion_[mbIdx * 16U + r * 4U + c] = info;
            break;
        }
        default: // Fallback: fill all (e.g., skip MBs, intra)
            for (uint32_t i = 0U; i < 16U; ++i)
                mbMotion_[mbIdx * 16U + i] = info;
            break;
        }
    }

    /** Get MV at a specific 4x4 block position (absolute pixel coords / 4).
     *  @param blk4x blk4y  Block position in 4x4 units (0 to width/4-1, etc.)
     */
    MbMotionInfo getMotionAt4x4(int32_t blk4x, int32_t blk4y) const noexcept
    {
        int32_t maxX = static_cast<int32_t>(widthInMbs_) * 4 - 1;
        int32_t maxY = static_cast<int32_t>(heightInMbs_) * 4 - 1;
        if (blk4x < 0 || blk4y < 0 || blk4x > maxX || blk4y > maxY)
            return {};
        uint32_t mbIdx = (blk4y / 4) * widthInMbs_ + (blk4x / 4);
        uint32_t blkIdx = (blk4y % 4) * 4 + (blk4x % 4);
        return mbMotion_[mbIdx * 16U + blkIdx];
    }

    /** Get MV neighbor info for a macroblock (legacy — uses bottom-right 4x4). */
    MbMotionInfo getMbMotionNeighbor(uint32_t mbX, uint32_t mbY, int32_t dx, int32_t dy) const noexcept
    {
        int32_t nx = static_cast<int32_t>(mbX) + dx;
        int32_t ny = static_cast<int32_t>(mbY) + dy;
        if (nx < 0 || ny < 0 || nx >= widthInMbs_ || ny >= heightInMbs_)
            return {};
        // For left neighbor: read right column (blk col 3). For top: bottom row (blk row 3).
        // For topRight: bottom-left block.
        uint32_t blkRow = (dy == -1) ? 3U : 0U; // top neighbor → bottom row of above MB
        uint32_t blkCol = (dx == -1) ? 3U : 0U; // left neighbor → right col of left MB
        if (dx == 1 && dy == -1) { blkRow = 3U; blkCol = 0U; } // topRight → bottom-left
        if (dx == -1 && dy == -1) { blkRow = 3U; blkCol = 3U; } // topLeft → bottom-right
        uint32_t mbIdx = ny * widthInMbs_ + nx;
        uint32_t blkIdx = blkRow * 4U + blkCol;
        return mbMotion_[mbIdx * 16U + blkIdx];
    }

    /** Decode a P_Skip macroblock: inferred MV, no residual.
     *
     *  VALIDATED: §8.4.1.1 zero-MV condition matches spec (unavailable neighbor
     *  → MV=0). MC full-pel copy verified pixel-exact vs IDR reference.
     *  KNOWN ISSUE: P-frame row 2+ chroma diffs vs ffmpeg/libavc. Our output
     *  matches IDR at MV=(0,0); reference decoders produce output consistent
     *  with MV=(16,8). Likely a bitstream alignment issue in preceding MB
     *  residual parsing that shifts the skip_run read position.
     */
    void decodePSkipMb(Frame& target, const Frame& ref,
                        uint32_t mbX, uint32_t mbY,
                        const SliceHeader& sh = {}) noexcept
    {
        // Neighbor derivation — §6.4.11.7 for 16x16 partition, mbPartIdx=0.
        // VALIDATED: getMbMotionNeighbor reads correct 4x4 block per partition layout.
        MbMotionInfo left    = getMbMotionNeighbor(mbX, mbY, -1, 0);
        MbMotionInfo top     = getMbMotionNeighbor(mbX, mbY, 0, -1);
        MbMotionInfo topRight = getMbMotionNeighbor(mbX, mbY, 1, -1);
        if (!topRight.available)
            topRight = getMbMotionNeighbor(mbX, mbY, -1, -1); // D fallback §8.4.1.3

        // §8.4.1.1: If mbAddrA or mbAddrB unavailable, or either has (ref=0,MV=0,0),
        // then skip MV = (0,0). Otherwise, MV = median predictor from §8.4.1.3.
        // VALIDATED against spec text. Produces MV=(0,0) when left unavailable (mbX=0).
        MotionVector skipMv = {0, 0};
        if (left.available && top.available &&
            !(left.refIdx == 0 && left.mv.x == 0 && left.mv.y == 0) &&
            !(top.refIdx == 0 && top.mv.x == 0 && top.mv.y == 0))
        {
            skipMv = computeMvPredictor(left, top, topRight, 0);
        }

        // Store MV for this MB (all 16 blocks same MV for skip)
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;


        setMbMotion(mbIdx, skipMv, 0);

        // Motion compensation: copy 16x16 block from reference
        int32_t refX = static_cast<int32_t>(mbX * cMbSize) + (skipMv.x >> 2);
        int32_t refY = static_cast<int32_t>(mbY * cMbSize) + (skipMv.y >> 2);
        uint32_t dx = static_cast<uint32_t>(skipMv.x) & 3U;
        uint32_t dy = static_cast<uint32_t>(skipMv.y) & 3U;

        lumaMotionComp(ref, refX, refY, dx, dy, cMbSize, cMbSize,
                        target.yMb(mbX, mbY), target.yStride());

        // Chroma — derive from luma MV (divide by 2, eighth-pel)
        int32_t chromaRefX = static_cast<int32_t>(mbX * cChromaBlockSize) + (skipMv.x >> 3);
        int32_t chromaRefY = static_cast<int32_t>(mbY * cChromaBlockSize) + (skipMv.y >> 3);
        uint32_t cdx = static_cast<uint32_t>(skipMv.x) & 7U;
        uint32_t cdy = static_cast<uint32_t>(skipMv.y) & 7U;

        // Diagnostic: verify ref frame chroma at MC position for debugging
        chromaMotionComp(ref, chromaRefX, chromaRefY, cdx, cdy,
                         cChromaBlockSize, cChromaBlockSize, true,
                         target.uMb(mbX, mbY), target.uvStride());
        chromaMotionComp(ref, chromaRefX, chromaRefY, cdx, cdy,
                         cChromaBlockSize, cChromaBlockSize, false,
                         target.vMb(mbX, mbY), target.uvStride());

        // Weighted prediction — §8.4.2.3.1 (P_Skip uses ref_idx=0)
        if (sh.hasWeightTable_)
        {
            const auto& w = sh.weightL0_[0];
            applyWeightedPred(target.yMb(mbX, mbY), target.yStride(), cMbSize, cMbSize,
                              sh.lumaLog2WeightDenom_, w.lumaWeight, w.lumaOffset, w.lumaWeightFlag);
            applyWeightedPred(target.uMb(mbX, mbY), target.uvStride(), cChromaBlockSize, cChromaBlockSize,
                              sh.chromaLog2WeightDenom_, w.chromaWeight[0], w.chromaOffset[0], w.chromaWeightFlag);
            applyWeightedPred(target.vMb(mbX, mbY), target.uvStride(), cChromaBlockSize, cChromaBlockSize,
                              sh.chromaLog2WeightDenom_, w.chromaWeight[1], w.chromaOffset[1], w.chromaWeightFlag);
        }

        // NNZ = 0 for skip MBs
        std::fill_n(&nnzLuma_[mbIdx * 16U], 16U, static_cast<uint8_t>(0U));
    }

    /** Decode P-inter macroblock — §7.3.5 mb_pred / sub_mb_pred.
     *
     *  Handles P_L0_16x16, P_L0_L0_16x8, P_L0_L0_8x16, P_8x8, P_8x8ref0.
     *
     *  VALIDATED: Bitstream parsing bit-aligned with libavc through MB(4,0).
     *  VALIDATED: P_8x8 sub-partition MVDs correctly stored (shadow var fix).
     *  VALIDATED: 8x16/16x8 directional shortcuts per §8.4.1.3.1.
     *  VALIDATED: Per-4x4-block MV storage for partition-aware neighbor derivation.
     *  KNOWN ISSUE: Residual bit consumption may be off by ~1 bit for some
     *  chroma AC blocks, causing skip_run misalignment after row 1.
     *
     *  @param mbQp  [in/out] Accumulated QP — ITU-T H.264 §7.4.5.
     */
    void decodePInterMb(BitReader& br, const Sps& sps, const Pps& pps,
                         const SliceHeader& sh,
                         int32_t& mbQp, uint32_t mbTypeRaw,
                         Frame& target, uint32_t mbX, uint32_t mbY,
                         uint8_t numRefIdxL0Active = 1U) noexcept
    {
        //TODO: refactor to separate MV decode from residual decode, to avoid shadow var confusion and enable more granular tracing.
        //TODO: consider refactoring to separate partition-level MV decode from per-4x4 storage, to avoid shadow var confusion and enable more granular tracing.
        //TODO: consider refactoring to separate chroma residual decode from luma, to avoid bitstream alignment issues and enable more granular tracing.
        //TODO: consider refactoring to separate residual decode from reconstruction, to enable more granular tracing and avoid confusion about when MVs vs residuals are available.
        //TODO: consider refactoring to separate prediction from reconstruction, to enable more granular tracing and avoid confusion about when residuals vs output pixels are available.
        //TODO: add specification references for the sequencing (high importance) and the logic at all steps

        (void)sps;
        // Trace numRefIdxL0Active for first MB of each frame
        if (mbX == 0U && mbY == 0U)
            trace_.onBlockResidual(mbX, mbY, 95U, numRefIdxL0Active, mbTypeRaw, 0U);
        // mbTypeRaw: 0=P_L0_16x16, 1=P_L0_L0_16x8, 2=P_L0_L0_8x16, 3=P_8x8, 4=P_8x8ref0

        // §7.3.5 macroblock_layer() sequencing for P-inter: [CHECKED §7.3.5]
        //   Step 1 — mb_type: decoded in caller.
        //   Step 2 — mb_pred (§7.3.5.1): ref_idx ALL partitions, then mvd ALL partitions.
        //             (FM-1: ref_idx and mvd MUST be separate loops, not interleaved.)
        //             For P_8x8: sub_mb_type all → ref_idx all → mvd all (§7.3.5.2).
        //   Step 3 — coded_block_pattern me(v): present (NOT I_16x16). [CHECKED §7.3.5]
        //   Step 4 — transform_size_8x8_flag u(1): MISSING HERE for CAVLC streams with
        //             transform_8x8_mode_flag=1. [UNCHECKED §7.3.5]
        //             (FM-3: condition: CodedBlockPatternLuma>0 && transform_8x8_mode_flag
        //              && mb_type!=I_NxN && noSubMbPartSizeLessThan8x8Flag.)
        //   Step 5 — mb_qp_delta se(v): only if cbp > 0. [CHECKED §7.3.5]
        //   Step 6 — residual(0,15). [CHECKED §7.3.5.3]
        // §7.3.5.1 mb_pred: ref_idx_l0 ALL partitions loop, THEN mvd_l0 ALL partitions loop.
        // 0=P_L0_16x16 (1 partition), 1=P_L0_L0_16x8 (2), 2=P_L0_L0_8x16 (2)
        // 3=P_8x8 (4 sub-MBs via sub_mb_pred §7.3.5.2), 4=P_8x8ref0 (4 sub-MBs, ref=0)
        int16_t mvdX[4] = {}, mvdY[4] = {};
        int16_t subMvdX[4] = {}, subMvdY[4] = {}; // Per-8x8 MVDs for P_8x8
        uint8_t refIdxL0[4] = {};  // Per-partition ref_idx (0-based)
        uint32_t numParts = 1U;

        if (mbTypeRaw <= 2U)
        {
            // 16x16, 16x8, or 8x16: 1 or 2 partitions
            numParts = (mbTypeRaw == 0U) ? 1U : 2U;

            // Read ref_idx for each partition — §7.3.5.1
            for (uint32_t p = 0U; p < numParts; ++p)
            {
                if (numRefIdxL0Active > 1U)
                {
                    if (numRefIdxL0Active == 2U)
                        refIdxL0[p] = static_cast<uint8_t>(1U - br.readBit()); // te(v) range=1 §9.1
                    else
                        refIdxL0[p] = static_cast<uint8_t>(br.readUev()); // te(v) range>1
                }
            }

            // Read MVD for each partition
            for (uint32_t p = 0U; p < numParts; ++p)
            {
                mvdX[p] = static_cast<int16_t>(br.readSev());
                mvdY[p] = static_cast<int16_t>(br.readSev());
            }
        }
        else
        {
            // P_8x8 or P_8x8ref0: sub_mb_pred — §7.3.5.2
            // Read 4 sub_mb_type values
            uint32_t subMbType[4];
            for (uint32_t s = 0U; s < 4U; ++s)
                subMbType[s] = br.readUev();

            // Read ref_idx for each 8x8 partition — §7.3.5.2
            // P_8x8ref0: all ref_idx = 0 (not read from bitstream).
            if (mbTypeRaw == 3U) // P_8x8 (not ref0)
            {
                for (uint32_t s = 0U; s < 4U; ++s)
                {
                    if (numRefIdxL0Active > 1U)
                    {
                        if (numRefIdxL0Active == 2U)
                            refIdxL0[s] = static_cast<uint8_t>(1U - br.readBit()); // te(v) §9.1
                        else
                            refIdxL0[s] = static_cast<uint8_t>(br.readUev());
                    }
                }
            }

            // Read MVD for each sub-partition — §7.3.5.2.
            // sub_mb_type: 0=P_L0_8x8(1 MVD), 1=P_L0_8x4(2), 2=P_L0_4x8(2), 3=P_L0_4x4(4)
            // Only P_L0_8x8 (sub_mb_type==0) fully supported — sub-partition MC
            // requires per-sub-partition MV storage not yet implemented.
            for (uint32_t s = 0U; s < 4U; ++s)
            {
                if (subMbType[s] != 0U)
                {
                    SUB0H264_UNSUPPORTED(
                        "P_8x8 sub_mb_type != 0 (8x4/4x8/4x4) not "
                        "implemented — sub-partition MC requires per-"
                        "sub-partition MV storage §7.3.5.2");
                    return;  // Graceful release degradation: skip MB decode
                }

                uint32_t numSubParts = 1U;
                if (subMbType[s] == 1U || subMbType[s] == 2U) numSubParts = 2U;
                else if (subMbType[s] == 3U) numSubParts = 4U;

                for (uint32_t sp = 0U; sp < numSubParts; ++sp)
                {
                    int16_t dx = static_cast<int16_t>(br.readSev());
                    int16_t dy = static_cast<int16_t>(br.readSev());
                    if (sp == 0U) { subMvdX[s] = dx; subMvdY[s] = dy; }
                }
            }
        }

        // Compute per-partition MV predictors — ITU-T H.264 §8.4.1.3.
        // Use per-4x4-block neighbor lookups for partition-aware prediction.
        int32_t mb4x = static_cast<int32_t>(mbX) * 4;
        int32_t mb4y = static_cast<int32_t>(mbY) * 4;

        MotionVector mvPart[2];
        MotionVector mvSub8x8[4] = {}; // Per-8x8 MVs for P_8x8
        if (mbTypeRaw <= 2U && numParts == 1U)
        {
            // P_L0_16x16: A=left(col-1,row0), B=top(col0,row-1), C=topRight(col4,row-1)
            MbMotionInfo a = getMotionAt4x4(mb4x - 1, mb4y);
            MbMotionInfo b = getMotionAt4x4(mb4x, mb4y - 1);

            MbMotionInfo c = getMotionAt4x4(mb4x + 4, mb4y - 1);
            if (!c.available)
                c = getMotionAt4x4(mb4x - 1, mb4y - 1); // top-left fallback
            MotionVector mvp = computeMvPredictor(a, b, c, static_cast<int8_t>(refIdxL0[0]));
            mvPart[0] = { static_cast<int16_t>(mvp.x + mvdX[0]),
                          static_cast<int16_t>(mvp.y + mvdY[0]) };
            trace_.onMvPrediction(mbX, mbY, 0, mvp, {mvdX[0], mvdY[0]}, mvPart[0], a, b, c);
        }
        else if (mbTypeRaw == 1U)
        {
            // P_L0_L0_16x8: §8.4.1.3.1
            // Partition 0 (top 16x8): B=top neighbor directly if same refIdx
            {
                MbMotionInfo a0 = getMotionAt4x4(mb4x - 1, mb4y);
                MbMotionInfo b0 = getMotionAt4x4(mb4x, mb4y - 1);
                MbMotionInfo c0 = getMotionAt4x4(mb4x + 4, mb4y - 1);
                if (!c0.available) c0 = getMotionAt4x4(mb4x - 1, mb4y - 1);
                int8_t ri0 = static_cast<int8_t>(refIdxL0[0]);
                MotionVector mvp0;
                if (b0.available && b0.refIdx == ri0)
                    mvp0 = b0.mv; // directional shortcut — §8.4.1.3.1
                else
                    mvp0 = computeMvPredictor(a0, b0, c0, ri0);
                mvPart[0] = { static_cast<int16_t>(mvp0.x + mvdX[0]),
                              static_cast<int16_t>(mvp0.y + mvdY[0]) };
                trace_.onMvPrediction(mbX, mbY, 0, mvp0, {mvdX[0], mvdY[0]}, mvPart[0], a0, b0, c0);
            }

            // Partition 1 (bottom 16x8): §8.4.1.3.1 directional shortcut: use LEFT.
            {
                MbMotionInfo a1 = getMotionAt4x4(mb4x - 1, mb4y + 2); // left at row 2
                MbMotionInfo b1 = { mvPart[0], {}, static_cast<int8_t>(refIdxL0[0]), true }; // partition 0
                // C = top-right of partition at (16, 7) relative to MB → unavailable.
                MbMotionInfo c1 = getMotionAt4x4(mb4x + 4, mb4y + 1);
                if (!c1.available)
                {
                    // D = top-left of partition at (-1, 7) → left MB's (col 3, row 1). §8.4.1.3
                    c1 = getMotionAt4x4(mb4x - 1, mb4y + 1);
                }
                int8_t ri1 = static_cast<int8_t>(refIdxL0[1]);
                MotionVector mvp1;
                if (a1.available && a1.refIdx == ri1)
                    mvp1 = a1.mv; // directional shortcut — §8.4.1.3.1
                else
                    mvp1 = computeMvPredictor(a1, b1, c1, ri1);
                mvPart[1] = { static_cast<int16_t>(mvp1.x + mvdX[1]),
                              static_cast<int16_t>(mvp1.y + mvdY[1]) };
                trace_.onMvPrediction(mbX, mbY, 1, mvp1, {mvdX[1], mvdY[1]}, mvPart[1], a1, b1, c1);
            }
        }
        else if (mbTypeRaw == 2U)
        {
            // P_L0_L0_8x16: §8.4.1.3.1
            // Partition 0 (left 8x16): A=left neighbor directly if same refIdx
            {
                MbMotionInfo a0 = getMotionAt4x4(mb4x - 1, mb4y);
                MbMotionInfo b0 = getMotionAt4x4(mb4x, mb4y - 1);
                MbMotionInfo c0 = getMotionAt4x4(mb4x + 2, mb4y - 1); // top of right half
                if (!c0.available) c0 = getMotionAt4x4(mb4x - 1, mb4y - 1);
                int8_t ri0 = static_cast<int8_t>(refIdxL0[0]);
                MotionVector mvp0;
                if (a0.available && a0.refIdx == ri0)
                    mvp0 = a0.mv; // directional shortcut — §8.4.1.3.1
                else
                    mvp0 = computeMvPredictor(a0, b0, c0, ri0);
                mvPart[0] = { static_cast<int16_t>(mvp0.x + mvdX[0]),
                              static_cast<int16_t>(mvp0.y + mvdY[0]) };
                trace_.onMvPrediction(mbX, mbY, 0, mvp0, {mvdX[0], mvdY[0]}, mvPart[0], a0, b0, c0);
            }

            // Partition 1 (right 8x16): C=topRight directly if same refIdx
            {
                MbMotionInfo a1 = { mvPart[0], {}, static_cast<int8_t>(refIdxL0[0]), true };
                MbMotionInfo b1 = getMotionAt4x4(mb4x + 2, mb4y - 1);
                MbMotionInfo c1 = getMotionAt4x4(mb4x + 4, mb4y - 1);
                if (!c1.available)
                    c1 = getMotionAt4x4(mb4x + 1, mb4y - 1); // D fallback
                int8_t ri1 = static_cast<int8_t>(refIdxL0[1]);
                MotionVector mvp1;
                if (c1.available && c1.refIdx == ri1)
                    mvp1 = c1.mv; // directional shortcut — §8.4.1.3.1
                else
                    mvp1 = computeMvPredictor(a1, b1, c1, ri1);
                mvPart[1] = { static_cast<int16_t>(mvp1.x + mvdX[1]),
                              static_cast<int16_t>(mvp1.y + mvdY[1]) };
                trace_.onMvPrediction(mbX, mbY, 1, mvp1, {mvdX[1], mvdY[1]}, mvPart[1], a1, b1, c1);
            }
        }
        else
        {
            // P_8x8/P_8x8ref0: per-8x8 sub-partition MV prediction — §8.4.1.3.
            // 4 sub-partitions in raster order: TL(0), TR(1), BL(2), BR(3).
            // Each 8x8 sub-partition has its own MV = MVP + MVD.
            // Store MVs immediately so subsequent sub-partitions can use them.
            // Sub-partition origins in 4x4 block coordinates:
            //   sub0=(mb4x,   mb4y),   sub1=(mb4x+2, mb4y)
            //   sub2=(mb4x,   mb4y+2), sub3=(mb4x+2, mb4y+2)
            static constexpr int32_t subOffX[4] = {0, 2, 0, 2};
            static constexpr int32_t subOffY[4] = {0, 0, 2, 2};

            uint32_t mbIdx = mbY * widthInMbs_ + mbX;
            for (uint32_t s = 0U; s < 4U; ++s)
            {
                int32_t sx = mb4x + subOffX[s];
                int32_t sy = mb4y + subOffY[s];

                // Neighbors for this 8x8 sub-partition — §8.4.1.3
                MbMotionInfo a = getMotionAt4x4(sx - 1, sy);
                MbMotionInfo b = getMotionAt4x4(sx, sy - 1);
                MbMotionInfo c = getMotionAt4x4(sx + 2, sy - 1);
                if (!c.available)
                    c = getMotionAt4x4(sx - 1, sy - 1); // D fallback

                MotionVector mvp = computeMvPredictor(a, b, c, static_cast<int8_t>(refIdxL0[s]));
                MotionVector mv = { static_cast<int16_t>(mvp.x + subMvdX[s]),
                                    static_cast<int16_t>(mvp.y + subMvdY[s]) };
                trace_.onMvPrediction(mbX, mbY, s, mvp, {subMvdX[s], subMvdY[s]}, mv, a, b, c);

                // Store this sub-partition's MV in its 4 blocks (2x2 in 4x4 grid)
                MbMotionInfo info = { mv, {subMvdX[s], subMvdY[s]}, static_cast<int8_t>(refIdxL0[s]), true };
                for (uint32_t r = 0U; r < 2U; ++r)
                    for (uint32_t cc = 0U; cc < 2U; ++cc)
                        mbMotion_[mbIdx * 16U + (subOffY[s] + r) * 4U + subOffX[s] + cc] = info;

                mvSub8x8[s] = mv;
            }
            // Per-sub-partition MVs already stored above.
        }

        // Store per-4x4-block MVs with actual ref_idx for neighbor derivation.
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;
        if (mbTypeRaw <= 2U)
        {
            if (numParts == 1U)
                setMbMotion(mbIdx, mvPart[0], static_cast<int8_t>(refIdxL0[0]));
            else
            {
                setPartitionMotion(mbIdx, mbTypeRaw, 0U, mvPart[0], static_cast<int8_t>(refIdxL0[0]));
                setPartitionMotion(mbIdx, mbTypeRaw, 1U, mvPart[1], static_cast<int8_t>(refIdxL0[1]));
            }
        }
        // P_8x8 MVs were stored incrementally in the sub-partition loop above.

        // Per-partition motion compensation — ITU-T H.264 §8.4.2
        // Reference frame lookup per partition via DPB L0 list.
        auto getRef = [this](uint8_t idx) -> const Frame& {
            const Frame* f = dpb_.getReference(idx);
            // Fall back to most recent ref if requested index exceeds DPB size.
            // This handles cases where numRefIdxL0Active (from PPS/override)
            // exceeds the actual number of references in the DPB (short GOPs).
            if (!f) f = dpb_.getReference(0U);
            return *f;
        };

        uint8_t predLuma[256];
        uint8_t predU[64], predV[64];

        if (mbTypeRaw <= 2U && numParts == 1U)
        {
            //TODO: refactor to unify with partition loop below (currently separate loops for partitions and sub-partitions, but could unify with appropriate indexing and offsets)
            // P_L0_16x16: single 16x16 partition
            const Frame& ref = getRef(refIdxL0[0]);
            MotionVector& mv = mvPart[0];
            int32_t refX = static_cast<int32_t>(mbX * cMbSize) + (mv.x >> 2);
            int32_t refY = static_cast<int32_t>(mbY * cMbSize) + (mv.y >> 2);
            lumaMotionComp(ref, refX, refY, static_cast<uint32_t>(mv.x) & 3U,
                           static_cast<uint32_t>(mv.y) & 3U,
                           cMbSize, cMbSize, predLuma, cMbSize);

            int32_t cRefX = static_cast<int32_t>(mbX * cChromaBlockSize) + (mv.x >> 3);
            int32_t cRefY = static_cast<int32_t>(mbY * cChromaBlockSize) + (mv.y >> 3);
            chromaMotionComp(ref, cRefX, cRefY, static_cast<uint32_t>(mv.x) & 7U,
                             static_cast<uint32_t>(mv.y) & 7U,
                             cChromaBlockSize, cChromaBlockSize, true, predU, cChromaBlockSize);
            chromaMotionComp(ref, cRefX, cRefY, static_cast<uint32_t>(mv.x) & 7U,
                             static_cast<uint32_t>(mv.y) & 7U,
                             cChromaBlockSize, cChromaBlockSize, false, predV, cChromaBlockSize);
            // Weighted prediction — §8.4.2.3.1
            if (sh.hasWeightTable_)
            {
                const auto& w = sh.weightL0_[refIdxL0[0]];
                applyWeightedPred(predLuma, cMbSize, cMbSize, cMbSize,
                                  sh.lumaLog2WeightDenom_, w.lumaWeight, w.lumaOffset, w.lumaWeightFlag);
                applyWeightedPred(predU, cChromaBlockSize, cChromaBlockSize, cChromaBlockSize,
                                  sh.chromaLog2WeightDenom_, w.chromaWeight[0], w.chromaOffset[0], w.chromaWeightFlag);
                applyWeightedPred(predV, cChromaBlockSize, cChromaBlockSize, cChromaBlockSize,
                                  sh.chromaLog2WeightDenom_, w.chromaWeight[1], w.chromaOffset[1], w.chromaWeightFlag);
            }
        }
        else if (mbTypeRaw == 1U)
        {
            // P_L0_L0_16x8: two 16x8 partitions (top half, bottom half)
            for (uint32_t p = 0U; p < 2U; ++p)
            {
                //TODO: refactor to avoid code duplication with partition loop below (currently separate loops for partitions and sub-partitions, but could unify with appropriate indexing and offsets)
                const Frame& ref = getRef(refIdxL0[p]);
                MotionVector& mv = mvPart[p];
                uint32_t partOffY = p * 8U;
                int32_t refX = static_cast<int32_t>(mbX * cMbSize) + (mv.x >> 2);
                int32_t refY = static_cast<int32_t>(mbY * cMbSize + partOffY) + (mv.y >> 2);
                lumaMotionComp(ref, refX, refY,
                               static_cast<uint32_t>(mv.x) & 3U,
                               static_cast<uint32_t>(mv.y) & 3U,
                               16U, 8U,
                               predLuma + partOffY * cMbSize, cMbSize);

                uint32_t cPartOffY = p * 4U;
                int32_t cRefX = static_cast<int32_t>(mbX * cChromaBlockSize) + (mv.x >> 3);
                int32_t cRefY = static_cast<int32_t>(mbY * cChromaBlockSize + cPartOffY) + (mv.y >> 3);
                chromaMotionComp(ref, cRefX, cRefY,
                                 static_cast<uint32_t>(mv.x) & 7U,
                                 static_cast<uint32_t>(mv.y) & 7U,
                                 8U, 4U, true,
                                 predU + cPartOffY * cChromaBlockSize, cChromaBlockSize);
                chromaMotionComp(ref, cRefX, cRefY,
                                 static_cast<uint32_t>(mv.x) & 7U,
                                 static_cast<uint32_t>(mv.y) & 7U,
                                 8U, 4U, false,
                                 predV + cPartOffY * cChromaBlockSize, cChromaBlockSize);
                // Weighted prediction — §8.4.2.3.1 (per 16x8 partition)
                if (sh.hasWeightTable_)
                {
                    const auto& w = sh.weightL0_[refIdxL0[p]];
                    applyWeightedPred(predLuma + partOffY * cMbSize, cMbSize, 16U, 8U,
                                      sh.lumaLog2WeightDenom_, w.lumaWeight, w.lumaOffset, w.lumaWeightFlag);
                    applyWeightedPred(predU + cPartOffY * cChromaBlockSize, cChromaBlockSize, 8U, 4U,
                                      sh.chromaLog2WeightDenom_, w.chromaWeight[0], w.chromaOffset[0], w.chromaWeightFlag);
                    applyWeightedPred(predV + cPartOffY * cChromaBlockSize, cChromaBlockSize, 8U, 4U,
                                      sh.chromaLog2WeightDenom_, w.chromaWeight[1], w.chromaOffset[1], w.chromaWeightFlag);
                }
            }
        }
        else if (mbTypeRaw == 2U)
        {
            // P_L0_L0_8x16: two 8x16 partitions (left half, right half)
            for (uint32_t p = 0U; p < 2U; ++p)
            {
                //TODO: refactor to avoid code duplication with partition loop above (currently separate loops for partitions and sub-partitions, but could unify with appropriate indexing and offsets)
                const Frame& ref = getRef(refIdxL0[p]);
                MotionVector& mv = mvPart[p];
                uint32_t partOffX = p * 8U;
                int32_t refX = static_cast<int32_t>(mbX * cMbSize + partOffX) + (mv.x >> 2);
                int32_t refY = static_cast<int32_t>(mbY * cMbSize) + (mv.y >> 2);
                lumaMotionComp(ref, refX, refY,
                               static_cast<uint32_t>(mv.x) & 3U,
                               static_cast<uint32_t>(mv.y) & 3U,
                               8U, 16U,
                               predLuma + partOffX, cMbSize);

                uint32_t cPartOffX = p * 4U;
                int32_t cRefX = static_cast<int32_t>(mbX * cChromaBlockSize + cPartOffX) + (mv.x >> 3);
                int32_t cRefY = static_cast<int32_t>(mbY * cChromaBlockSize) + (mv.y >> 3);
                chromaMotionComp(ref, cRefX, cRefY,
                                 static_cast<uint32_t>(mv.x) & 7U,
                                 static_cast<uint32_t>(mv.y) & 7U,
                                 4U, 8U, true,
                                 predU + cPartOffX, cChromaBlockSize);
                chromaMotionComp(ref, cRefX, cRefY,
                                 static_cast<uint32_t>(mv.x) & 7U,
                                 static_cast<uint32_t>(mv.y) & 7U,
                                 4U, 8U, false,
                                 predV + cPartOffX, cChromaBlockSize);
                // Weighted prediction — §8.4.2.3.1 (per 8x16 partition)
                if (sh.hasWeightTable_)
                {
                    const auto& w = sh.weightL0_[refIdxL0[p]];
                    applyWeightedPred(predLuma + partOffX, cMbSize, 8U, 16U,
                                      sh.lumaLog2WeightDenom_, w.lumaWeight, w.lumaOffset, w.lumaWeightFlag);
                    applyWeightedPred(predU + cPartOffX, cChromaBlockSize, 4U, 8U,
                                      sh.chromaLog2WeightDenom_, w.chromaWeight[0], w.chromaOffset[0], w.chromaWeightFlag);
                    applyWeightedPred(predV + cPartOffX, cChromaBlockSize, 4U, 8U,
                                      sh.chromaLog2WeightDenom_, w.chromaWeight[1], w.chromaOffset[1], w.chromaWeightFlag);
                }
            }
        }
        else
        {
            // P_8x8 / P_8x8ref0: per-8x8 motion compensation — §8.4.2.
            // 4 sub-partitions: TL(0), TR(1), BL(2), BR(3), each 8x8 luma / 4x4 chroma.
            static constexpr uint32_t subLumaOffX[4] = {0, 8, 0, 8};
            static constexpr uint32_t subLumaOffY[4] = {0, 0, 8, 8};
            static constexpr uint32_t subChromaOffX[4] = {0, 4, 0, 4};
            static constexpr uint32_t subChromaOffY[4] = {0, 0, 4, 4};

            for (uint32_t s = 0U; s < 4U; ++s)
            {
                //TODO: refactor to avoid code duplication with partition loop above (currently separate loops for partitions and sub-partitions, but could unify with appropriate indexing and offsets)
                const Frame& ref = getRef(refIdxL0[s]);
                MotionVector& mv = mvSub8x8[s];
                uint32_t lox = subLumaOffX[s], loy = subLumaOffY[s];
                int32_t refX = static_cast<int32_t>(mbX * cMbSize + lox) + (mv.x >> 2);
                int32_t refY = static_cast<int32_t>(mbY * cMbSize + loy) + (mv.y >> 2);
                lumaMotionComp(ref, refX, refY,
                               static_cast<uint32_t>(mv.x) & 3U,
                               static_cast<uint32_t>(mv.y) & 3U,
                               8U, 8U,
                               predLuma + loy * cMbSize + lox, cMbSize);

                uint32_t cox = subChromaOffX[s], coy = subChromaOffY[s];
                int32_t cRefX = static_cast<int32_t>(mbX * cChromaBlockSize + cox) + (mv.x >> 3);
                int32_t cRefY = static_cast<int32_t>(mbY * cChromaBlockSize + coy) + (mv.y >> 3);
                chromaMotionComp(ref, cRefX, cRefY,
                                 static_cast<uint32_t>(mv.x) & 7U,
                                 static_cast<uint32_t>(mv.y) & 7U,
                                 4U, 4U, true,
                                 predU + coy * cChromaBlockSize + cox, cChromaBlockSize);
                chromaMotionComp(ref, cRefX, cRefY,
                                 static_cast<uint32_t>(mv.x) & 7U,
                                 static_cast<uint32_t>(mv.y) & 7U,
                                 4U, 4U, false,
                                 predV + coy * cChromaBlockSize + cox, cChromaBlockSize);
                // Weighted prediction — §8.4.2.3.1 (per 8x8 sub-MB)
                if (sh.hasWeightTable_)
                {
                    const auto& w = sh.weightL0_[refIdxL0[s]];
                    applyWeightedPred(predLuma + loy * cMbSize + lox, cMbSize, 8U, 8U,
                                      sh.lumaLog2WeightDenom_, w.lumaWeight, w.lumaOffset, w.lumaWeightFlag);
                    applyWeightedPred(predU + coy * cChromaBlockSize + cox, cChromaBlockSize, 4U, 4U,
                                      sh.chromaLog2WeightDenom_, w.chromaWeight[0], w.chromaOffset[0], w.chromaWeightFlag);
                    applyWeightedPred(predV + coy * cChromaBlockSize + cox, cChromaBlockSize, 4U, 4U,
                                      sh.chromaLog2WeightDenom_, w.chromaWeight[1], w.chromaOffset[1], w.chromaWeightFlag);
                }
            }
        }

        // §7.3.5: coded_block_pattern me(v) — present for P-inter (not I_16x16). [CHECKED §7.3.5]
        trace_.onMbEnd(mbX, mbY, static_cast<uint32_t>(br.bitOffset())); // pre-CBP bit
        uint32_t cbpCode = br.readUev();
        uint8_t cbp = 0U;
        if (cbpCode < 48U)
            cbp = cCbpTable[cbpCode][1]; // Inter CBP table (§9.1.2 Table 9-4)

        uint8_t cbpLuma = cbp & 0x0FU;
        uint8_t cbpChroma = (cbp >> 4U) & 0x03U;

        // §7.3.5 FM-3: transform_size_8x8_flag u(1) not decoded after CBP for CAVLC P-inter.
        // Condition from spec: CodedBlockPatternLuma>0 && transform_8x8_mode_flag
        //   && mb_type!=I_NxN && noSubMbPartSizeLessThan8x8Flag.
        // For P_L0_16x16/16x8/8x16, noSubMbPartSizeLessThan8x8Flag=1 always, so the
        // flag would be present when cbpLuma>0 and 8x8 mode enabled — but we don't read it.
        if (pps.transform8x8Mode_ != 0U && cbpLuma != 0U)
        {
            SUB0H264_UNSUPPORTED(
                "CAVLC P-inter: §7.3.5 requires transform_size_8x8_flag u(1) "
                "after CBP when pps.transform8x8Mode_=1 and cbpLuma>0; not "
                "implemented (FM-3)");
            return;  // Graceful release degradation: skip MB decode
        }
        // Trace CBP: use BlockResidual with blkIdx=99 as CBP marker
        trace_.onBlockResidual(mbX, mbY, 99U, cbpCode, cbpLuma | (cbpChroma << 4U),
                               static_cast<uint32_t>(br.bitOffset()));
        int32_t qp = mbQp;
        // §7.3.5: mb_qp_delta se(v) — only if cbp > 0 for P-inter. [CHECKED §7.3.5]
        if (cbp > 0U)
        {
            int32_t qpDelta = br.readSev();
            qp += qpDelta;
            qp = ((qp % 52) + 52) % 52; // §7.4.5: proper modular wrapping
        }

        // §7.3.5.3: residual(0,15) — luma then chroma. [CHECKED §7.3.5.3]
        uint32_t yStride = target.yStride();
        for (uint32_t blkIdx = 0U; blkIdx < 16U; ++blkIdx)
        {
            uint32_t blkX = cLuma4x4BlkX[blkIdx];
            uint32_t blkY = cLuma4x4BlkY[blkIdx];
            uint32_t rasterIdx = cLuma4x4ToRaster[blkIdx];

            //TODO: consider tracking non-zero coeffs count per block for early zeroing and/or skipping inverse transform if zero (currently tracked in nnzLuma_ but not used for skipping)
            uint32_t group8x8 = blkIdx >> 2U;
            bool hasResidual = (cbpLuma >> group8x8) & 1U;

            //TODO: comment if coeffs need to be zeroed or not (depends on how inverseQuantize4x4 handles coeffs with zero totalCoeff)
            //TODO: consider unifying coeffs storage for luma/chroma (currently separate ResidualBlock4x4 structs for luma DC, chroma DC, chroma AC, and direct coeffs array for luma AC)
            int16_t coeffs[16] = {};
            if (hasResidual)
            {
                int32_t nc = getLumaNc(mbX, mbY, rasterIdx);
                uint32_t bitBefore = static_cast<uint32_t>(br.bitOffset());
                ResidualBlock4x4 resBlock;
                decodeResidualBlock4x4(br, nc, cMaxCoeff4x4, 0U, resBlock);
                uint32_t bitsUsed = static_cast<uint32_t>(br.bitOffset()) - bitBefore;
                trace_.onBlockResidual(mbX, mbY, blkIdx, nc, resBlock.totalCoeff, bitsUsed);
                for (uint32_t i = 0U; i < 16U; ++i)
                    coeffs[i] = resBlock.coeffs[i];
                nnzLuma_[mbIdx * 16U + rasterIdx] = resBlock.totalCoeff;
                dequantize4x4(coeffs, qp, Block4x4Plane::LumaInter);
            }

            //TODO: consider separate handling for DC vs AC blocks (currently combined in single ResidualBlock4x4 and decodeResidualBlock4x4 calls with nc=-1 for DC and nc from getLumaNc for AC, but could be split into separate paths for clarity and/or optimization)
            uint8_t* predPtr = predLuma + blkY * cMbSize + blkX;
            uint8_t* outPtr = target.yMb(mbX, mbY) + blkY * yStride + blkX;
            inverseDct4x4AddPred(coeffs, predPtr, cMbSize, outPtr, yStride);
        }

        // Decode chroma residual and reconstruct
        // §7.4.2.2 Chroma QP (FM-10: must use chromaQp, not luma QP)
        int32_t chromaQp = computeChromaQp(qp, pps.chromaQpIndexOffset_);

        int16_t dcCb[4] = {}, dcCr[4] = {};
        if (cbpChroma >= 1U)
        {
            uint32_t dcBitStart = static_cast<uint32_t>(br.bitOffset());
            ResidualBlock4x4 dcBlockCb, dcBlockCr;
            decodeResidualBlock4x4(br, -1, 4U, 0U, dcBlockCb);
            uint32_t dcCbBits = static_cast<uint32_t>(br.bitOffset()) - dcBitStart;
            trace_.onBlockResidual(mbX, mbY, 16U, -1, dcBlockCb.totalCoeff, dcCbBits);
            uint32_t dcCrStart = static_cast<uint32_t>(br.bitOffset());
            decodeResidualBlock4x4(br, -1, 4U, 0U, dcBlockCr);
            uint32_t dcCrBits = static_cast<uint32_t>(br.bitOffset()) - dcCrStart;
            trace_.onBlockResidual(mbX, mbY, 17U, -1, dcBlockCr.totalCoeff, dcCrBits);
            for (uint32_t i = 0U; i < 4U; ++i) { dcCb[i] = dcBlockCb.coeffs[i]; dcCr[i] = dcBlockCr.coeffs[i]; }
            inverseHadamard2x2(dcCb);
            inverseHadamard2x2(dcCr);

            // §8.5.11 + §8.5.12.1: Chroma DC dequant (FM-10: chromaQp used, not luma QP).
            // P-inter path → CbInter/CrInter weightScale[0][0].
            dequantChromaDcValues(dcCb, chromaQp, scalingLists_.list4x4[4][0]);
            dequantChromaDcValues(dcCr, chromaQp, scalingLists_.list4x4[5][0]);
        }

        // Stash per-block AC coefficients — decode all Cb first, then Cr (§7.3.5)
        int16_t cbCoeffsBuf[4][16] = {};
        int16_t crCoeffsBuf[4][16] = {};
        for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
        {
            cbCoeffsBuf[blkIdx][0] = dcCb[blkIdx];
            crCoeffsBuf[blkIdx][0] = dcCr[blkIdx];
        }

        if (cbpChroma >= 2U)
        {
            // All Cb AC blocks first — §7.3.5.3
            for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
            {
                int32_t ncCb = getChromaNc(mbX, mbY, blkIdx, true);
                uint32_t acBitBefore = static_cast<uint32_t>(br.bitOffset());
                ResidualBlock4x4 acBlock;
                decodeResidualBlock4x4(br, ncCb, 15U, 1U, acBlock);
                uint32_t acBits = static_cast<uint32_t>(br.bitOffset()) - acBitBefore;
                trace_.onBlockResidual(mbX, mbY, 18U + blkIdx, ncCb, acBlock.totalCoeff, acBits);
                for (uint32_t i = 1U; i < 16U; ++i) cbCoeffsBuf[blkIdx][i] = acBlock.coeffs[i];
                nnzCb_[mbIdx * 4U + blkIdx] = acBlock.totalCoeff;
                int16_t savedDc = cbCoeffsBuf[blkIdx][0];
                dequantize4x4(cbCoeffsBuf[blkIdx], chromaQp, Block4x4Plane::CbInter);
                cbCoeffsBuf[blkIdx][0] = savedDc;
            }
            // Then all Cr AC blocks — §7.3.5.3
            for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
            {
                int32_t ncCr = getChromaNc(mbX, mbY, blkIdx, false);
                uint32_t acBitBefore = static_cast<uint32_t>(br.bitOffset());
                ResidualBlock4x4 acBlock;
                decodeResidualBlock4x4(br, ncCr, 15U, 1U, acBlock);
                uint32_t acBits = static_cast<uint32_t>(br.bitOffset()) - acBitBefore;
                trace_.onBlockResidual(mbX, mbY, 22U + blkIdx, ncCr, acBlock.totalCoeff, acBits);
                for (uint32_t i = 1U; i < 16U; ++i) crCoeffsBuf[blkIdx][i] = acBlock.coeffs[i];
                nnzCr_[mbIdx * 4U + blkIdx] = acBlock.totalCoeff;
                int16_t savedDc = crCoeffsBuf[blkIdx][0];
                dequantize4x4(crCoeffsBuf[blkIdx], chromaQp, Block4x4Plane::CrInter);
                crCoeffsBuf[blkIdx][0] = savedDc;
            }
        }

        // Reconstruct chroma using motion-compensated predictions
        uint32_t uvStride = target.uvStride();
        for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
        {
            uint32_t blkX = (blkIdx & 1U) * 4U;
            uint32_t blkY = (blkIdx >> 1U) * 4U;
            inverseDct4x4AddPred(cbCoeffsBuf[blkIdx], predU + blkY * 8U + blkX, 8U,
                                 target.uMb(mbX, mbY) + blkY * uvStride + blkX, uvStride);
            inverseDct4x4AddPred(crCoeffsBuf[blkIdx], predV + blkY * 8U + blkX, 8U,
                                 target.vMb(mbX, mbY) + blkY * uvStride + blkX, uvStride);
        }
        mbQp = qp; // Propagate accumulated QP to next MB
        trace_.onMbEnd(mbX, mbY, static_cast<uint32_t>(br.bitOffset()));
    }

    /** Decode an intra MB within a P-slice (mb_type offset already applied).
     *
     *  NOTE: The I-MB decode functions write to currentFrame_. For P-slices,
     *  the decode target is a separate DPB entry. We copy the decoded MB
     *  from currentFrame_ → target after decode. §7.3.5
     */
    bool decodeIntraMbInPSlice(BitReader& br, const Sps& sps, const Pps& pps,
                                const SliceHeader& sh, int32_t& mbQp,
                                uint32_t mbTypeRaw, Frame& target,
                                uint32_t mbX, uint32_t mbY) noexcept
    {
        (void)sh; (void)target;
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;
        setMbMotion(mbIdx, {0, 0}, -1);

        //TODO: Should we track I_NxN here for future neighbor context derivation? It's not needed for intra decode since no CABAC, but it would be needed if we later switch to CABAC for intra in P-slices.
        if (mbTypeRaw == 25U)
        {
            // Intra-in-P I_PCM (§7.3.5) — same alignment hazard as other
            // I_PCM paths. NOT IMPLEMENTED — subsequent bitstream is
            // misaligned after the raw PCM samples.
            SUB0H264_UNSUPPORTED(
                "Intra-in-P I_PCM (§7.3.5) not implemented — "
                "subsequent bitstream is misaligned.");
            return false;
        }

        // With activeFrame_ pointing to the DPB target, intra decoders write
        // directly into the correct frame. No neighbourhood copy needed since
        // previous MBs (inter or intra) already wrote their pixels there.
        // No copy-back needed since activeFrame_ IS the target.

        bool ok;
        if (isI16x16(static_cast<uint8_t>(mbTypeRaw)))
            ok = decodeI16x16Mb(br, sps, pps, mbTypeRaw, mbQp, mbX, mbY);
        else
        //TODO: We currently treat all non-I_16x16 MBs in P-slices as I_4x4, but we should ideally support both I_4x4 and I_8x8 (with transform8x8Mode_) in P-slices. This requires separate neighbor context handling since the left/top neighbors could be either I_4x4 or I_16x16.
            ok = decodeI4x4Mb(br, sps, pps, mbQp, mbX, mbY);

        return ok;
    }

    // ── CABAC-specific MB decode methods ────────────────────────────────

    /** Decode an intra MB using CABAC.
     *  @param mbQp  [in/out] Accumulated QP — ITU-T H.264 §7.4.5.
     */
    bool decodeCabacIntraMb(BitReader& br, const Sps& sps, const Pps& pps,
                             const SliceHeader& sh, int32_t& mbQp,
                             uint32_t mbX, uint32_t mbY) noexcept
    {
        (void)br; (void)sh;
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;
        setMbMotion(mbIdx, {0, 0}, -1);

        // §9.3.3.1.2: condTermFlagN for mb_type in I-slices.
        // condTermFlagN = 0 when neighbor unavailable OR is I_NxN.
        // condTermFlagN = 1 ONLY when neighbor available AND NOT I_NxN.
        // Verified: ffmpeg/NXPlay both use condTerm=0 for unavailable.
        // Pass true="treat as I_NxN (condTerm=0)" to the decode function.
        bool leftCondZero, topCondZero;
        cabacNeighbor_.mbTypeCtxI(mbX, mbY, leftCondZero, topCondZero);

        uint32_t mbTypeRaw = cabacDecodeMbTypeI(cabacEngine_, cabacCtx_.data(),
                                                 leftCondZero, topCondZero);
#ifdef SUB0H264_TRACE_I4X4_BLOCKS
        std::fprintf(stderr, "  MB(%u,%u) mbTypeRaw=%u leftCond=%d topCond=%d\n",
            mbX, mbY, mbTypeRaw, leftCondZero, topCondZero);
#endif

        // Track I_NxN (I_4x4) for future neighbor context derivation
        cabacNeighbor_[mbIdx].setI4x4(mbTypeRaw == 0U);
        if (mbTypeRaw == 25U)
        {
            // CABAC I-slice I_PCM (§9.3.3.1.1) — raw samples follow.
            // NOT IMPLEMENTED — subsequent bitstream misaligned.
            SUB0H264_UNSUPPORTED(
                "CABAC I-slice I_PCM (§9.3.3.1.1) not implemented — "
                "subsequent bitstream is misaligned.");
            return false;
        }
        if (mbTypeRaw == 0U)
        {
            // I_4x4: decode pred modes + residual via CABAC
            return decodeCabacI4x4Mb(sps, pps, mbQp, mbX, mbY);
        }
        else
        {
            // I_16x16: decode using existing path with CABAC residual
            return decodeCabacI16x16Mb(sps, pps, mbTypeRaw, mbQp, mbX, mbY);
        }
    }

    /** Decode I_4x4 MB with CABAC residual. */
    bool decodeCabacI4x4Mb(const Sps& sps, const Pps& pps,
                            int32_t& qp, uint32_t mbX, uint32_t mbY) noexcept
    {
        //TODO: This function is currently only used for I_4x4 MBs in P-slices, but it should ideally also be used for I_4x4 MBs in I-slices when CABAC is enabled for intra. This requires some adjustments to neighbor context handling since the left/top neighbors could be either I_4x4 or I_16x16, and the current implementation assumes all neighbors are I_4x4 for simplicity. We may need to track I_NxN status separately for each neighbor to handle this correctly.
        //TODO: add specification references for the sequencing (high importance) and the logic at all steps

        (void)sps;
        // §7.3.5 macroblock_layer() sequencing for I_NxN (CABAC): [CHECKED §7.3.5]
        //   Step 1 — mb_type: decoded in caller (mbTypeRaw==0 for I_NxN).
        //   Step 2 — transform_size_8x8_flag ae(v): BEFORE mb_pred for I_NxN. [CHECKED §7.3.5]
        //             (FM-1: must precede pred mode loop — determines 4x4 vs 8x8 block size.)
        //   Step 3 — mb_pred: intra4x4_pred_mode per block below. [CHECKED §7.3.5.1]
        //   Step 4 — intra_chroma_pred_mode ae(v) below. [CHECKED §7.3.5.1]
        //   Step 5 — coded_block_pattern ae(v): present for I_NxN. [CHECKED §7.3.5]
        //   Step 6 — mb_qp_delta ae(v): only if cbp > 0. [CHECKED §7.3.5]
        //   Step 7 — residual(0,15). [CHECKED §7.3.5.3]

        // §7.3.5: transform_size_8x8_flag before mb_pred for I_NxN.
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;
        bool use8x8Transform = false;
        if (pps.transform8x8Mode_ != 0U)
            use8x8Transform = decodeCabacTransform8x8Flag(mbX, mbY, mbIdx);
        mbTransform8x8_[mbIdx] = use8x8Transform ? 1U : 0U;

        uint8_t predModes[16] = {};
        uint32_t numModeBlocks = use8x8Transform ? 4U : 16U;
#ifdef SUB0H264_TRACE_I4X4_BLOCKS
        if (mbX == 0U && mbY == 0U)
            std::fprintf(stderr, "PRED_MODE_DECODE: engine R=%u O=%u bp=%u ctx68=%u ctx69=%u\n",
                cabacEngine_.range(), cabacEngine_.offset(),
                static_cast<uint32_t>(cabacEngine_.bitPosition()),
                static_cast<uint32_t>(cabacCtx_[68].mpsState),
                static_cast<uint32_t>(cabacCtx_[69].mpsState));
#endif
        for (uint32_t i = 0U; i < numModeBlocks; ++i)
        {
            // For I_8x8: 4 blocks in 8x8 scan order (TL, TR, BL, BR)
            // For I_4x4: 16 blocks in spec scan order §6.4.3
            uint32_t rasterIdx;
            if (use8x8Transform)
            {
                // 8x8 block i → top-left 4x4 raster index for neighbor lookup
                // Block 0→raster 0, Block 1→raster 2, Block 2→raster 8, Block 3→raster 10
                static constexpr uint32_t c8x8ToRaster[4] = {0U, 2U, 8U, 10U};
                rasterIdx = c8x8ToRaster[i];
            }
            else
            {
                rasterIdx = cLuma4x4ToRaster[i];
            }

            // §8.3.1.2.1: neighbor mode derivation (always needed — MPM uses both)
            uint8_t leftMode = getNeighborIntra4x4Mode(mbX, mbY, rasterIdx, true, predModes);
            uint8_t topMode  = getNeighborIntra4x4Mode(mbX, mbY, rasterIdx, false, predModes);

            // §8.3.1.1: dcPredModePredictedFlag — force BOTH to DC when either
            // neighbor's macroblock is unavailable. [FM-22]
            bool leftMbUnavail = (rasterIdx % 4U == 0U) && (mbX == 0U);
            bool topMbUnavail  = (rasterIdx < 4U) && (mbY == 0U);
            if (leftMbUnavail || topMbUnavail)
            {
                leftMode = 2U;
                topMode = 2U;
            }
            uint8_t mpm = (leftMode < topMode) ? leftMode : topMode;

            // §8.3.1.1: Derivation of Intra4x4PredMode
            // prev_intra4x4_pred_mode_flag=1 → mode = predIntra4x4PredMode (MPM)
            // prev_intra4x4_pred_mode_flag=0 → mode = rem if rem < MPM, else rem + 1
            // CABAC: flag at ctx[68], rem at ctx[69] (3 context-coded bins, LSB-first)
            uint8_t result = cabacDecodeIntra4x4PredMode(cabacEngine_, cabacCtx_.data());
            uint8_t mode;
            if (result == 0xFFU)
                mode = mpm; // prev_flag=1: use most probable mode
            else
                mode = (result < mpm) ? result : static_cast<uint8_t>(result + 1U);
#ifdef SUB0H264_EXP_FORCE_MODE1_SCAN5
            // Experiment: force mode=1 (H) for scan block 5 of MB(0,0) to verify
            // that the prediction mode is the source of the pixel diff.
            if (mbX == 0U && mbY == 0U && i == 5U)
                mode = 1U;
#endif
#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                std::fprintf(stderr, "  MB(%u,%u) scan%u raster%u: mpm=%u result=%u mode=%u R=%u O=%u bp=%u c68=%u c69=%u\n",
                    mbX, mbY, i, rasterIdx, mpm, result, mode,
                    cabacEngine_.range(), cabacEngine_.offset(),
                    static_cast<uint32_t>(cabacEngine_.bitPosition()),
                    static_cast<uint32_t>(cabacCtx_[68].mpsState),
                    static_cast<uint32_t>(cabacCtx_[69].mpsState));
#endif

            if (use8x8Transform)
            {
                // §6.4.3: scan order maps 8x8 block to constituent 4x4 blocks
                // Propagate mode to all 4 constituent 4x4 blocks for neighbor lookup
                static constexpr uint32_t c8x8Sub[4][4] = {
                    {0U, 1U, 4U, 5U}, {2U, 3U, 6U, 7U},
                    {8U, 9U, 12U, 13U}, {10U, 11U, 14U, 15U}
                };
                for (uint32_t s = 0U; s < 4U; ++s)
                    predModes[c8x8Sub[i][s]] = mode;
            }
            else
            {
                predModes[rasterIdx] = mode;
            }
        }

        // Chroma pred mode — §9.3.3.1.1.7: ctxInc from neighbor chroma modes
        uint32_t chromaCtxInc = cabacNeighbor_.chromaModeCtxInc(mbX, mbY);
        uint32_t chromaPredMode = cabacDecodeIntraChromaMode(cabacEngine_, cabacCtx_.data(),
                                                              chromaCtxInc);
        cabacNeighbor_[mbIdx].chromaMode = static_cast<uint8_t>(chromaPredMode);

        // CBP via CABAC — §9.3.3.1.1.4 with per-block neighbor CBP
        auto cbpN = cabacNeighbor_.cbpNeighbors(mbX, mbY);
        uint8_t cbp = cabacDecodeCbp(cabacEngine_, cabacCtx_.data(),
                                      cbpN.left & 0x0FU, cbpN.top & 0x0FU,
                                      (cbpN.left >> 4U) & 3U, (cbpN.top >> 4U) & 3U);
        uint8_t cbpLuma = cbp & 0x0FU;
        uint8_t cbpChroma = (cbp >> 4U) & 0x03U;
        cabacNeighbor_[mbIdx].cbp = cbp;

        // §7.3.5: mb_qp_delta ae(v) — only if cbp > 0 for I_NxN (NOT always like I_16x16).
        // §9.3.3.1.1.5: when mb_qp_delta is NOT decoded (i.e. cbp==0), the
        // syntax element is treated as having value 0 → reset prevHadDelta to
        // false. Verified bit-exact against JM read_dQuant_CABAC + the
        // `if (!cbp) last_dquant = 0;` reset in cabac.c L1298-1301.
        // [CHECKED §7.3.5] (FM-3: cbp encodes both luma and chroma; cbp>0 correctly covers both.)
        if (cbp > 0U)
        {
            int32_t qpDelta = cabacDecodeMbQpDelta(cabacEngine_, cabacCtx_.data(),
                                                    prevMbHadNonZeroQpDelta_);
            prevMbHadNonZeroQpDelta_ = (qpDelta != 0);
            qp += qpDelta;
            qp = ((qp % 52) + 52) % 52; // §7.4.5: proper modular wrapping
        }
        else
        {
            prevMbHadNonZeroQpDelta_ = false;
        }

#ifdef SUB0H264_TRACE_I4X4_BLOCKS
        if (mbX == 0U && mbY == 0U)
        {
            std::fprintf(stderr, "=== MB(0,0) I4x4: cbp=0x%02x cbpLuma=%u cbpChroma=%u qp=%d ===\n",
                cbp, cbpLuma, cbpChroma, qp);
            // binTrace moved to slice init
        }
#endif
        // Decode luma residual — §7.3.5.3
        uint8_t* mbLuma = activeFrame_->yMb(mbX, mbY);
        uint32_t yStride = activeFrame_->yStride();

        if (use8x8Transform)
        {
            // I_8x8: 4 × 8x8 residual blocks — §7.3.5.3 with ctxBlockCat=5
            for (uint32_t blk8 = 0U; blk8 < 4U; ++blk8)
            {
                uint32_t blkX = (blk8 & 1U) * 8U;
                uint32_t blkY = (blk8 >> 1U) * 8U;
                uint32_t absX = mbX * cMbSize + blkX;
                uint32_t absY = mbY * cMbSize + blkY;
                bool hasResidual = (cbpLuma >> blk8) & 1U;

                // 8x8 intra prediction — §8.3.2 with reference sample filtering
                // The prediction mode is stored in predModes[] at the top-left
                // 4x4 sub-block of this 8x8 block.
                static constexpr uint32_t c8x8TopLeft[4] = {0U, 2U, 8U, 10U};
                uint8_t mode8x8 = predModes[c8x8TopLeft[blk8]];
                uint8_t pred8x8[64];
                intraPred8x8Luma(static_cast<Intra4x4Mode>(mode8x8),
                                  *activeFrame_, absX, absY, pred8x8);

                int16_t coeffs[64] = {};
                if (hasResidual)
                {
                    int16_t scanCoeffs[64] = {};
                    cabacDecodeResidual8x8(cabacEngine_, cabacCtx_.data(), scanCoeffs);
                    // Reorder from 8x8 scan to raster via zigzag — §6.4.8
                    for (uint32_t k = 0U; k < 64U; ++k)
                        coeffs[cZigzag8x8[k]] = scanCoeffs[k];
                    dequantize8x8(coeffs, qp, false);
                }

                // §6.4.3: 8x8→4x4 raster index mapping. blk8 is in raster scan order:
                // blk8=0→(0,0), blk8=1→(1,0), blk8=2→(0,1), blk8=3→(1,1)
                // base4x4 = top-left 4x4 raster index of the 8x8 block
                uint32_t base4x4 = (blk8 >> 1U) * 8U + (blk8 & 1U) * 2U;
                for (uint32_t dy = 0U; dy < 2U; ++dy)
                    for (uint32_t dx = 0U; dx < 2U; ++dx)
                        nnzLuma_[mbIdx * 16U + base4x4 + dy * 4U + dx] = hasResidual ? 1U : 0U;

                uint8_t* outPtr = mbLuma + blkY * yStride + blkX;
                inverseDct8x8AddPred(coeffs, pred8x8, 8U, outPtr, yStride);

            }
        }
        else
        {
            // I_4x4: 16 × 4x4 residual blocks (spec scan order §6.4.3)
#ifdef SUB0H264_P0_I8X8_DIAG
            if (mbY == 0U && mbX <= 16U)
                std::fprintf(stderr, "[I_4x4] MB(%u,0) enter (cbpLuma=0x%x)\n",
                             mbX, static_cast<unsigned>(cbpLuma));
#endif
            for (uint32_t blkIdx = 0U; blkIdx < 16U; ++blkIdx)
            {
                //TODO: consider precomputing absolute positions for each block in a separate table for clarity and/or performance (currently computed on the fly using cLuma4x4BlkX/Y and mbX/Y, but could be simplified with a direct lookup)
                uint32_t blkX = cLuma4x4BlkX[blkIdx];
                uint32_t blkY = cLuma4x4BlkY[blkIdx];
                uint32_t rasterIdx = cLuma4x4ToRaster[blkIdx];
                uint32_t absX = mbX * cMbSize + blkX;
                uint32_t absY = mbY * cMbSize + blkY;

                // §6.4.2: Availability of neighboring samples for intra prediction
                // §8.3.1.2: Intra_4x4 prediction process
                // §8.3.1.1: Reference sample availability (FM-16: uses activeFrame_, not currentFrame_)
                uint8_t pred4x4[16];
                uint8_t topBuf[8], leftBuf[4], topLeftVal;
                const uint8_t *top, *topRight, *left, *topLeft;
                sampleIntra4x4Neighbors(absX, absY, blkIdx,
                                        topBuf, leftBuf, topLeftVal,
                                        top, topRight, left, topLeft);

                intraPred4x4(static_cast<Intra4x4Mode>(predModes[rasterIdx]), top, topRight, left, topLeft, pred4x4);
                // (debug traces removed — see git history for investigation)

                // §7.3.5.3: Residual data syntax — cbpLuma bit per 8x8 group
                // §7.4.5: CodedBlockPatternLuma bit i corresponds to 8x8 block i
                uint32_t group8x8 = blkIdx >> 2U;
                bool hasResidual = (cbpLuma >> group8x8) & 1U;

                int16_t coeffs[16] = {};
                if (hasResidual)
                {
                    // §9.3.3.1.1.9: coded_block_flag ctxIdxInc for I_4x4 luma (cat 2)
                    // Intra MB: unavailable → condTermFlag=1 (coded assumed). [CHECKED §9.3.3.1.1.9]
                    uint32_t leftNnz = 1U, topNnz = 1U; // unavailable → coded (intra)
                    if (rasterIdx % 4U > 0U)
                        leftNnz = nnzLuma_[mbIdx * 16U + rasterIdx - 1U];
                    else if (mbX > 0U)
                        leftNnz = nnzLuma_[(mbIdx - 1U) * 16U + rasterIdx + 3U];
                    if (rasterIdx >= 4U)
                        topNnz = nnzLuma_[mbIdx * 16U + rasterIdx - 4U];
                    else if (mbY > 0U)
                        topNnz = nnzLuma_[(mbIdx - widthInMbs_) * 16U + rasterIdx + 12U];
                    uint32_t cbfCtxInc = (leftNnz != 0U ? 1U : 0U) + (topNnz != 0U ? 2U : 0U);

                    int16_t scanCoeffs[16] = {};
#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                    std::fprintf(stderr, "BLK_START MB(%u,%u) scan%u raster%u cbfInc=%u binIdx=%u\n",
                        mbX, mbY, blkIdx, rasterIdx, cbfCtxInc,
                        cabacEngine_.binTraceCount());
#endif
                    uint32_t numNonZero = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                                  scanCoeffs, 16U, 2U, cbfCtxInc);
                    for (uint32_t k = 0U; k < 16U; ++k)
                        coeffs[cZigzag4x4[k]] = scanCoeffs[k];
                    nnzLuma_[mbIdx * 16U + rasterIdx] = (numNonZero > 0U) ? 1U : 0U;

#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                    if (mbX == 0U && mbY == 0U)
                    {
                        std::fprintf(stderr, "  CBF: leftNnz=%u topNnz=%u cbfCtxInc=%u numNZ=%u\n",
                            leftNnz, topNnz, cbfCtxInc, numNonZero);
                        std::fprintf(stderr, "  SCAN=[%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d]\n",
                            scanCoeffs[0],scanCoeffs[1],scanCoeffs[2],scanCoeffs[3],
                            scanCoeffs[4],scanCoeffs[5],scanCoeffs[6],scanCoeffs[7],
                            scanCoeffs[8],scanCoeffs[9],scanCoeffs[10],scanCoeffs[11],
                            scanCoeffs[12],scanCoeffs[13],scanCoeffs[14],scanCoeffs[15]);
                    }
#endif

                    // Trace: raw scan coefficients before dequant (CABAC path)
                    trace_.onBlockResidual(mbX, mbY, blkIdx,
                                           static_cast<int32_t>(cbfCtxInc),
                                           numNonZero,
                                           static_cast<uint32_t>(cabacEngine_.bitPosition()));
                    // Trace: dequantized coefficients (CABAC path)
                    trace_.emit({TraceEventType::BlockCoeffs,
                                 static_cast<uint16_t>(mbX), static_cast<uint16_t>(mbY),
                                 blkIdx, 0U, 0U, 0U, scanCoeffs, 16U});

#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                    if (blkIdx <= 2U)
                    {
                        std::fprintf(stderr, "  MB(%u,%u) scan%u RASTER_PRE_DQ=[%d %d %d %d][%d %d %d %d][%d %d %d %d][%d %d %d %d]\n",
                            mbX, mbY, blkIdx,
                            coeffs[0],coeffs[1],coeffs[2],coeffs[3],
                            coeffs[4],coeffs[5],coeffs[6],coeffs[7],
                            coeffs[8],coeffs[9],coeffs[10],coeffs[11],
                            coeffs[12],coeffs[13],coeffs[14],coeffs[15]);
                    }
#endif
                    dequantize4x4(coeffs, qp, Block4x4Plane::LumaIntra);
#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                    if (blkIdx <= 2U)
                    {
                        std::fprintf(stderr, "  MB(%u,%u) scan%u RASTER_POST_DQ=[%d %d %d %d][%d %d %d %d][%d %d %d %d][%d %d %d %d]\n",
                            mbX, mbY, blkIdx,
                            coeffs[0],coeffs[1],coeffs[2],coeffs[3],
                            coeffs[4],coeffs[5],coeffs[6],coeffs[7],
                            coeffs[8],coeffs[9],coeffs[10],coeffs[11],
                            coeffs[12],coeffs[13],coeffs[14],coeffs[15]);
                    }
#endif
                }
                else
                {
                    // CBF=0: no residual for this block, coeffs remain zero-initialized
                    nnzLuma_[mbIdx * 16U + rasterIdx] = 0U;
                }

                // Trace: prediction + output pixels (CABAC path)
                trace_.emit({TraceEventType::BlockPixels,
                             static_cast<uint16_t>(mbX), static_cast<uint16_t>(mbY),
                             blkIdx, 0U, 0U, 0U, nullptr, 0U});

                uint8_t* outPtr = mbLuma + blkY * yStride + blkX;

#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                if (mbX == 0U && mbY == 0U)
                {
                    std::fprintf(stderr, "=== BLK scan%u raster%u (%u,%u) mode=%u grp%u hasRes=%d ===\n",
                        blkIdx, rasterIdx, absX, absY, predModes[rasterIdx], group8x8, hasResidual ? 1 : 0);
                    if (hasResidual)
                    {
                        std::fprintf(stderr, "  COEFFS_DEQUANT=[%d %d %d %d][%d %d %d %d][%d %d %d %d][%d %d %d %d]\n",
                            coeffs[0],coeffs[1],coeffs[2],coeffs[3],
                            coeffs[4],coeffs[5],coeffs[6],coeffs[7],
                            coeffs[8],coeffs[9],coeffs[10],coeffs[11],
                            coeffs[12],coeffs[13],coeffs[14],coeffs[15]);
                    }
                }
#endif

                inverseDct4x4AddPred(coeffs, pred4x4, 4U, outPtr, yStride);

#ifdef SUB0H264_TRACE_I4X4_BLOCKS
                if (mbX == 0U && mbY == 0U)
                {
                    std::fprintf(stderr, "  OUT=[%d %d %d %d][%d %d %d %d][%d %d %d %d][%d %d %d %d]\n",
                        outPtr[0], outPtr[1], outPtr[2], outPtr[3],
                        outPtr[yStride], outPtr[yStride+1], outPtr[yStride+2], outPtr[yStride+3],
                        outPtr[2*yStride], outPtr[2*yStride+1], outPtr[2*yStride+2], outPtr[2*yStride+3],
                        outPtr[3*yStride], outPtr[3*yStride+1], outPtr[3*yStride+2], outPtr[3*yStride+3]);
                }
#endif
            }
        }

        // Store pred modes for cross-MB MPM derivation (§8.3.1.1)
        for (uint32_t k = 0U; k < 16U; ++k)
            mbIntra4x4Modes_[mbIdx * 16U + k] = predModes[k];

        // Chroma (use existing CAVLC chroma path — structure is the same,
        // only entropy coding differs. For CABAC chroma, use CABAC residual.)
        decodeChromaCabac(pps, chromaPredMode, cbpChroma, qp, mbX, mbY);
        return true;
    }

    /** Decode I_16x16 MB with CABAC. */
    bool decodeCabacI16x16Mb(const Sps& sps, const Pps& pps,
                              uint32_t mbTypeRaw, int32_t& qp,
                              uint32_t mbX, uint32_t mbY) noexcept
    {
#ifdef SUB0H264_P0_I8X8_DIAG
        if (mbY == 0U && mbX <= 16U)
            std::fprintf(stderr, "[I_16x16] MB(%u,0) mbType=%u\n",
                         mbX, mbTypeRaw);
#endif
        // §7.3.5 macroblock_layer() sequencing for Intra_16x16 (CABAC): [CHECKED §7.3.5]
        //   Step 1 — mb_type: decoded in caller (encodes predMode, cbpLuma, cbpChroma).
        //   Step 2 — mb_pred: intra_chroma_pred_mode ae(v). [CHECKED §7.3.5.1]
        //             NOTE (FM-3): coded_block_pattern ABSENT for I_16x16. [CHECKED §7.3.5]
        //   Step 3 — mb_qp_delta ae(v): ALWAYS present for I_16x16. [CHECKED §7.3.5]
        //   Step 4 — residual DC luma (always) + AC luma (if cbpLuma) +
        //             chroma DC (if cbpChroma>=1) + chroma AC (if cbpChroma==2). [CHECKED §7.3.5.3]
        // Note: Used for both I-slices and intra-in-P-slices (neighbor context
        // handled by CabacNeighborCtx which tracks I_NxN per MB).
        (void)sps;
        uint8_t predMode = i16x16PredMode(static_cast<uint8_t>(mbTypeRaw));
        uint8_t cbpLuma  = i16x16CbpLuma(static_cast<uint8_t>(mbTypeRaw));
        uint8_t cbpChroma = i16x16CbpChroma(static_cast<uint8_t>(mbTypeRaw));


        uint32_t mbIdx = mbY * widthInMbs_ + mbX;
        cabacNeighbor_[mbIdx].cbp = cbpLuma | (cbpChroma << 4U); // Store for neighbor context
        cabacNeighbor_[mbIdx].setI16x16(true);
        // §9.3.3.1.1.7: intra_chroma_pred_mode — ctxInc from neighbor chroma modes
        uint32_t chromaCtxInc = cabacNeighbor_.chromaModeCtxInc(mbX, mbY);
        uint32_t chromaPredMode = cabacDecodeIntraChromaMode(cabacEngine_, cabacCtx_.data(),
                                                              chromaCtxInc);
        cabacNeighbor_[mbIdx].chromaMode = static_cast<uint8_t>(chromaPredMode);

        // §9.3.3.1.1.5: mb_qp_delta — ctxInc depends on previous MB's delta. [CHECKED §9.3.3.1.1.5]
        int32_t qpDelta = cabacDecodeMbQpDelta(cabacEngine_, cabacCtx_.data(),
                                                prevMbHadNonZeroQpDelta_);
        prevMbHadNonZeroQpDelta_ = (qpDelta != 0);
        qp += qpDelta;
        qp = ((qp % 52) + 52) % 52;

        // Generate 16x16 prediction
        uint8_t lumaPred[256];
        intraPred16x16(static_cast<Intra16x16Mode>(predMode), *activeFrame_, mbX, mbY, lumaPred);

#ifdef SUB0H264_P0_I8X8_DIAG
        if (mbX == 15U && mbY == 0U)
        {
            std::fprintf(stderr, "  pred[16x16] row 0 cols 0-15: ");
            for (int c = 0; c < 16; ++c) std::fprintf(stderr, " %3u", lumaPred[c]);
            std::fprintf(stderr, "\n  pred[16x16] col 0 rows 0-15: ");
            for (int r = 0; r < 16; ++r) std::fprintf(stderr, " %3u", lumaPred[r*16]);
            std::fprintf(stderr, "\n");
        }
#endif

        // Decode DC block via CABAC (category 0 = Luma DC)
        // §9.3.3.1.1.9: coded_block_flag for luma DC (ctxBlockCat=0).
        // transBlockN = luma DC of neighbor IF neighbor is I_16x16.
        // condTermFlagN per spec:
        //   - Unavailable + current Intra → 1
        //   - Available + NOT I_16x16 → transBlock not available → 0
        //   - Available + I_16x16 → actual coded_block_flag of luma DC
        uint32_t dcCbfCtxInc = cabacNeighbor_.lumaDcCbfCtxInc(mbX, mbY, true);
        int16_t dcScan[16] = {};
        uint32_t dcNonZero = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                      dcScan, 16U, 0U, dcCbfCtxInc);
        cabacNeighbor_[mbIdx].setLumaDcCbf(dcNonZero > 0U);

        int16_t dcCoeffs[16] = {};
        for (uint32_t i = 0U; i < 16U; ++i)
            dcCoeffs[cZigzag4x4[i]] = dcScan[i];

#ifdef SUB0H264_P0_I8X8_DIAG
        if (mbX == 15U && mbY == 0U)
        {
            std::fprintf(stderr, "\n=== P0 I_16x16 DIAG: MB(15,0) mbType=%u predMode=%u cbpLuma=%u ===\n",
                         mbTypeRaw, predMode, cbpLuma);
            std::fprintf(stderr, "  DC scan coeffs (pre-zigzag): ");
            for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %3d", dcScan[i]);
            std::fprintf(stderr, "\n");
            std::fprintf(stderr, "  DC raster coeffs (post-zigzag): ");
            for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %3d", dcCoeffs[i]);
            std::fprintf(stderr, "\n  dcCbfCtxInc=%u dcNonZero=%u qp=%d\n",
                         dcCbfCtxInc, dcNonZero, qp);
        }
#endif

        inverseHadamard4x4(dcCoeffs);

        // §8.5.12.1: Intra16x16 luma DC dequant after Hadamard. Scaling list
        // weightScale[0][0] applied via the dequant helper's second arg.
        dequantLumaDcValues(dcCoeffs, qp, scalingLists_.list4x4[0][0]);

#ifdef SUB0H264_P0_I8X8_DIAG
        if (mbX == 15U && mbY == 0U)
        {
            std::fprintf(stderr, "  DC after Hadamard+dequant: ");
            for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %5d", dcCoeffs[i]);
            std::fprintf(stderr, "\n");
        }
#endif

        // Decode AC blocks (spec scan order §6.4.3)
        uint8_t* mbLuma = activeFrame_->yMb(mbX, mbY);
        uint32_t yStride = activeFrame_->yStride();

        for (uint32_t blkIdx = 0U; blkIdx < 16U; ++blkIdx)
        {
            uint32_t blkX = cLuma4x4BlkX[blkIdx];
            uint32_t blkY = cLuma4x4BlkY[blkIdx];
            uint32_t rasterIdx = cLuma4x4ToRaster[blkIdx];

            int16_t coeffs[16] = {};
            // Hadamard output is in raster order — DC already dequantized above
            coeffs[0] = dcCoeffs[rasterIdx];

            if (cbpLuma)
            {
                //TODO: We should consider splitting the AC decode into a separate function for clarity, since the DC decode is already handled via the Hadamard path and the AC decode has a different context derivation (cbfInc) than the I_4x4 case. This would also allow us to add more specific specification references for the AC decode logic, which is currently missing and important for validation.
                // §9.3.3.1.1.9: coded_block_flag ctxIdxInc = condTermFlagA + 2*condTermFlagB
                // Unavailable neighbors (edge of frame) → condTermFlag=1 (coded assumed).
                // [CHECKED §9.3.3.1.1.9]
                uint32_t leftNnz = 1U, topNnz = 1U; // default: unavailable → coded
                if (rasterIdx % 4U > 0U)
                    leftNnz = nnzLuma_[mbIdx * 16U + rasterIdx - 1U];
                else if (mbX > 0U)
                    leftNnz = nnzLuma_[(mbIdx - 1U) * 16U + rasterIdx + 3U];
                if (rasterIdx >= 4U)
                    topNnz = nnzLuma_[mbIdx * 16U + rasterIdx - 4U];
                else if (mbY > 0U)
                    topNnz = nnzLuma_[(mbIdx - widthInMbs_) * 16U + rasterIdx + 12U];
                uint32_t cbfInc = (leftNnz != 0U ? 1U : 0U) + (topNnz != 0U ? 2U : 0U);

                int16_t acScan[16] = {};
                uint32_t numSig = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                          acScan, 15U, 1U, cbfInc);
                // AC coefficients in scan order 0..14 → raster positions via zigzag.
                // Zigzag indices 1..15 correspond to AC positions (skip DC at index 0).
                for (uint32_t i = 0U; i < 15U; ++i)
                    coeffs[cZigzag4x4[i + 1U]] = acScan[i];
                nnzLuma_[mbIdx * 16U + rasterIdx] = (numSig > 0U) ? 1U : 0U;
            }

            // Save DC (already dequantized via Hadamard path), dequant AC, restore DC.
            // §8.5.12.1: DC was scaled above; dequantize4x4 would re-dequant it.
            int16_t savedDc = coeffs[0];
            dequantize4x4(coeffs, qp, Block4x4Plane::LumaIntra);
            coeffs[0] = savedDc;
            uint8_t* predPtr = lumaPred + blkY * 16U + blkX;
            uint8_t* outPtr = mbLuma + blkY * yStride + blkX;
            inverseDct4x4AddPred(coeffs, predPtr, 16U, outPtr, yStride);
        }

        decodeChromaCabac(pps, chromaPredMode, cbpChroma, qp, mbX, mbY);
        return true;
    }

    /** Decode P-inter MB with CABAC entropy.
     *  @param mbQp  [in/out] Accumulated QP — ITU-T H.264 §7.4.5.
     */
    void decodeCabacPInterMb(BitReader& br, const Sps& sps, const Pps& pps,
                              const SliceHeader& sh,
                              int32_t& mbQp, uint32_t mbTypeRaw,
                              Frame& target,
                              uint32_t mbX, uint32_t mbY) noexcept
    {
        // §7.3.5 macroblock_layer() sequencing for P-inter (CABAC): [PARTIAL §7.3.5]
        //   Step 1 — mb_type: decoded in caller (mbTypeRaw 0-4 for P-inter).
        //   Step 2 — mb_pred (§7.3.5.1): [UNCHECKED §7.3.5.1]
        //     FM-16: ref_idx_l0 NOT decoded — correct only when
        //            num_ref_idx_l0_active_minus1 == 0 (§7.3.5.1 condition).
        //            Guarded by assert(sh.numRefIdxActiveL0_ <= 1U).
        //     FM-16: Only P_L0_16x16 handled; mbTypeRaw asserted == 0.
        //   Step 3 — mc (16x16 only). [UNCHECKED §7.3.5]
        //   Step 4 — coded_block_pattern ae(v): present for P-inter (not I_16x16). [CHECKED §7.3.5]
        //   Step 5 — transform_size_8x8_flag ae(v) when cbpLuma>0 && 8x8mode. [CHECKED §7.3.5]
        //             Spec condition: CodedBlockPatternLuma>0 && transform_8x8_mode_flag
        //             && mb_type!=I_NxN && noSubMbPartSizeLessThan8x8Flag.
        //             For P_L0_16x16: noSubMbPartSizeLessThan8x8Flag=1 always.
        //   Step 6 — mb_qp_delta ae(v): only if cbp > 0. [CHECKED §7.3.5]
        //   Step 7 — residual(0,15). [CHECKED §7.3.5.3]

        (void)br; (void)sps;

        // §7.3.5.1 mb_pred / §7.3.5.2 sub_mb_pred for CABAC P-inter.
        // Supports all partition types: 16x16, 16x8, 8x16, 8x8. [CHECKED §7.3.5]
        uint32_t numParts = (mbTypeRaw == 0U) ? 1U : (mbTypeRaw <= 2U ? 2U : 4U);
        uint32_t numRefIdxL0Active = sh.numRefIdxActiveL0_;
        uint8_t refIdxL0[4] = {};
        int16_t mvdX[4] = {}, mvdY[4] = {};
        // Per-sub-partition MVDs for P_8x8 — sub_mb_type != 0 uses 2 or 4 slots.
        int16_t subMvdX[4][4] = {};
        int16_t subMvdY[4][4] = {};
        uint32_t subMbType[4] = {};

        // Sub-partition layout per sub_mb_type (offsets + size in 4x4 units
        // relative to the 8x8 block TL). -1 marks unused slot. §7.3.5.2 /
        // Table 7-13. Shared by MVD decode and MC dispatch.
        static constexpr int8_t cSubPartLayout[4][4][4] = {
            {{0, 0, 2, 2}, {-1, -1, -1, -1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}},
            {{0, 0, 2, 1}, { 0,  1,  2,  1}, {-1, -1, -1, -1}, {-1, -1, -1, -1}},
            {{0, 0, 1, 2}, { 1,  0,  1,  2}, {-1, -1, -1, -1}, {-1, -1, -1, -1}},
            {{0, 0, 1, 1}, { 1,  0,  1,  1}, { 0,  1,  1,  1}, { 1,  1,  1,  1}},
        };
        const int32_t mb4x = static_cast<int32_t>(mbX) * 4;
        const int32_t mb4y = static_cast<int32_t>(mbY) * 4;

        // §9.3.3.1.1.7 — MVD context: absMvdNeighbor = |mvdA[comp]| + |mvdB[comp]|
        // where A=left-neighbor partition, B=top-neighbor partition of the partition top-left.
        // Unified path: every MVD (both cross-MB and within-MB, including P_8x8
        // sub-partitions) has been stored to mbMotion_[].mvd by the time we
        // read a later neighbour. The .available flag is intentionally NOT
        // checked here — within-MB partitions store .mvd before MC dispatch
        // sets .available; out-of-frame coords return zero-init mvd via
        // getMotionAt4x4 (correct per spec). For intra/skip neighbours the
        // entry's mvd is 0, matching spec treatment.
        auto getMvdComp = [&](int32_t blk4x, int32_t blk4y, bool xComp) -> int32_t {
            MbMotionInfo info = getMotionAt4x4(blk4x, blk4y);
            return xComp ? info.mvd.x : info.mvd.y;
        };
        auto absMvdNbr = [&](int32_t tlX, int32_t tlY, bool xComp) -> int32_t {
            return std::abs(getMvdComp(tlX - 1, tlY,     xComp))   // left
                 + std::abs(getMvdComp(tlX,     tlY - 1, xComp));  // top
        };

        // §9.3.3.1.1.6: ref_idx context derivation — ctxInc from partition neighbors.
        // condTermFlagA = (refIdxA > 0), condTermFlagB = (refIdxB > 0).
        // Uses 4x4-block-level neighbor lookup (same as JM's get4x4Neighbour).
        // Unavailable neighbors → condTermFlag = 0. [CHECKED §9.3.3.1.1.6]
        auto refIdxCtxInc = [&](int32_t tlX, int32_t tlY) -> uint32_t {
            // §9.3.3.1.1.6 condTermN: 0 if unavailable, intra, or refIdx==0;
            // 1 otherwise. Intra/unavailable have refIdx=-1 (zero-init), so
            // refIdx > 0 alone correctly excludes all three cases — no need
            // to gate on `.available`. (This matters for P_8x8 sub-partitions
            // that have refIdx written early but `available` set only after
            // MC, so neighbour MV prediction sees C unavailable until ready.)
            MbMotionInfo left = getMotionAt4x4(tlX - 1, tlY);
            MbMotionInfo top  = getMotionAt4x4(tlX,     tlY - 1);
            uint32_t cA = (left.refIdx > 0) ? 1U : 0U;
            uint32_t cB = (top.refIdx  > 0) ? 2U : 0U;
            return cA + cB;
        };

        if (mbTypeRaw <= 2U)
        {
            // 16x16 / 16x8 / 8x16: §7.3.5.1 ref_idx ALL first, then mvd ALL
            // Store each refIdx immediately so the second partition's context
            // derivation (§9.3.3.1.1.6) can see the first partition's refIdx.
            {
                uint32_t mi = mbY * widthInMbs_ + mbX;
                for (uint32_t p = 0U; p < numParts; ++p)
                {
                    if (numRefIdxL0Active > 1U)
                    {
                        int32_t tlX = mb4x + (mbTypeRaw == 2U && p == 1U ? 2 : 0);
                        int32_t tlY = mb4y + (mbTypeRaw == 1U && p == 1U ? 2 : 0);
                        uint32_t ctxInc = refIdxCtxInc(tlX, tlY);
                        refIdxL0[p] = cabacDecodeRefIdx(cabacEngine_, cabacCtx_.data(), ctxInc,
                                                         numRefIdxL0Active - 1U);
                    }
                    // Store partition refIdx to mbMotion_ for neighbor context
                    int8_t ri = static_cast<int8_t>(refIdxL0[p]);
                    if (mbTypeRaw == 0U) // 16x16: all blocks
                    {
                        for (uint32_t i = 0U; i < 16U; ++i)
                        {
                            mbMotion_[mi * 16U + i].refIdx = ri;
                            mbMotion_[mi * 16U + i].available = true;
                        }
                    }
                    else if (mbTypeRaw == 1U) // 16x8: 2 rows per partition
                    {
                        uint32_t sr = p * 2U;
                        for (uint32_t r = sr; r < sr + 2U; ++r)
                            for (uint32_t c = 0U; c < 4U; ++c)
                            {
                                mbMotion_[mi * 16U + r * 4U + c].refIdx = ri;
                                mbMotion_[mi * 16U + r * 4U + c].available = true;
                            }
                    }
                    else // 8x16: 2 cols per partition
                    {
                        uint32_t sc = p * 2U;
                        for (uint32_t r = 0U; r < 4U; ++r)
                            for (uint32_t c = sc; c < sc + 2U; ++c)
                            {
                                mbMotion_[mi * 16U + r * 4U + c].refIdx = ri;
                                mbMotion_[mi * 16U + r * 4U + c].available = true;
                            }
                    }
                }
            }
            // Top-left 4x4 position of each partition (in absolute 4x4 units):
            //   16x16 (p=0): (mb4x, mb4y)
            //   16x8  (p=0): (mb4x, mb4y),   (p=1): (mb4x, mb4y+2)
            //   8x16  (p=0): (mb4x, mb4y),   (p=1): (mb4x+2, mb4y)
            for (uint32_t p = 0U; p < numParts; ++p)
            {
                int32_t tlX = mb4x + (mbTypeRaw == 2U && p == 1U ? 2 : 0); // 8x16 p1 → col+2
                int32_t tlY = mb4y + (mbTypeRaw == 1U && p == 1U ? 2 : 0); // 16x8 p1 → row+2
                int32_t nbrX = absMvdNbr(tlX, tlY, true);
                int32_t nbrY = absMvdNbr(tlX, tlY, false);
                mvdX[p] = cabacDecodeMvd(cabacEngine_, cabacCtx_.data(), cCtxMvdX, nbrX);
                mvdY[p] = cabacDecodeMvd(cabacEngine_, cabacCtx_.data(), cCtxMvdY, nbrY);
                // Store partition 0's MVD to mbMotion_ so partition 1's context
                // lookup (via getMvdComp → getMotionAt4x4) picks it up.
                uint32_t mi = mbY * widthInMbs_ + mbX;
                uint32_t r0 = 0U, r1 = 4U, c0 = 0U, c1 = 4U;
                if (mbTypeRaw == 1U) { r0 = p * 2U; r1 = r0 + 2U; }        // 16x8
                else if (mbTypeRaw == 2U) { c0 = p * 2U; c1 = c0 + 2U; }   // 8x16
                for (uint32_t r = r0; r < r1; ++r)
                    for (uint32_t c = c0; c < c1; ++c)
                        mbMotion_[mi * 16U + r * 4U + c].mvd = {mvdX[p], mvdY[p]};
            }
        }
        else
        {
            // P_8x8: §7.3.5.2 sub_mb_type ALL, then ref_idx ALL, then mvd ALL
            for (uint32_t s = 0U; s < 4U; ++s)
                subMbType[s] = cabacDecodeSubMbTypeP(cabacEngine_, cabacCtx_.data());
            // P_8x8 sub-partition origins in 4x4 units: (0,0),(2,0),(0,2),(2,2)
            static constexpr int32_t c8x8Off4x[4] = {0, 2, 0, 2};
            static constexpr int32_t c8x8Off4y[4] = {0, 0, 2, 2};
            // ref_idx per sub-partition — §7.3.5.2
            // Must store each refIdx immediately so subsequent sub-partitions
            // can use it for context derivation (§9.3.3.1.1.6 needs neighbor refIdx).
            for (uint32_t s = 0U; s < 4U; ++s)
            {
                if (numRefIdxL0Active > 1U)
                {
                    int32_t tlX = mb4x + c8x8Off4x[s];
                    int32_t tlY = mb4y + c8x8Off4y[s];
                    uint32_t ctxInc = refIdxCtxInc(tlX, tlY);
#if SUB0H264_TRACE
                    if (mbX < 2U && mbY == 0U)
                    {
                        MbMotionInfo leftN = getMotionAt4x4(tlX - 1, tlY);
                        MbMotionInfo topN  = getMotionAt4x4(tlX, tlY - 1);
                        std::fprintf(stderr, "REFIDX_CTX MB(%u,%u) sub%u tl=(%d,%d) leftAvail=%d leftRef=%d topAvail=%d topRef=%d ctxInc=%u\n",
                            mbX, mbY, s, tlX, tlY,
                            leftN.available ? 1 : 0, leftN.refIdx,
                            topN.available ? 1 : 0, topN.refIdx, ctxInc);
                    }
#endif
                    refIdxL0[s] = cabacDecodeRefIdx(cabacEngine_, cabacCtx_.data(), ctxInc,
                                                     numRefIdxL0Active - 1U);
                }
                // Store refIdx (only) to mbMotion_ so neighbour refIdx
                // context (refIdxCtxInc) sees it during decode.
                // CRITICAL: do NOT set .available here — that field gates the
                // C-neighbour MV-predictor lookup, and the MV isn't computed
                // until doPartitionMC runs. Setting available=true with
                // mv={0,0} causes downstream sub-partitions to read a bogus
                // (0,0) C predictor instead of falling back to D. .available
                // is set in doPartitionMC's per-block write below.
                uint32_t sr = (s >> 1U) * 2U;
                uint32_t sc = (s & 1U) * 2U;
                int8_t ri = static_cast<int8_t>(refIdxL0[s]);
                uint32_t mi = mbY * widthInMbs_ + mbX;
                for (uint32_t r = sr; r < sr + 2U; ++r)
                    for (uint32_t cc = sc; cc < sc + 2U; ++cc)
                        mbMotion_[mi * 16U + r * 4U + cc].refIdx = ri;
            }
            // MVD per sub-partition — §7.3.5.2 / §9.3.3.1.1.7.
            uint32_t mi = mbY * widthInMbs_ + mbX;
            for (uint32_t s = 0U; s < 4U; ++s)
            {
                uint32_t numSubParts = 1U;
                if (subMbType[s] == 1U || subMbType[s] == 2U) numSubParts = 2U;
                else if (subMbType[s] == 3U) numSubParts = 4U;

                for (uint32_t sp = 0U; sp < numSubParts; ++sp)
                {
                    // Absolute 4x4 block coords of this sub-partition's top-left.
                    int32_t spOffX = cSubPartLayout[subMbType[s]][sp][0];
                    int32_t spOffY = cSubPartLayout[subMbType[s]][sp][1];
                    int32_t spW4   = cSubPartLayout[subMbType[s]][sp][2];
                    int32_t spH4   = cSubPartLayout[subMbType[s]][sp][3];
                    int32_t tlX    = mb4x + c8x8Off4x[s] + spOffX;
                    int32_t tlY    = mb4y + c8x8Off4y[s] + spOffY;
                    // Neighbor MVDs from actual 4x4 block positions — the
                    // previous sub-partition's MVD (within this MB) has been
                    // stored in mbMotion_ already, so absMvdNbr picks it up.
                    int32_t ctxX = absMvdNbr(tlX, tlY, true);
                    int32_t ctxY = absMvdNbr(tlX, tlY, false);
                    int16_t dx = cabacDecodeMvd(cabacEngine_, cabacCtx_.data(), cCtxMvdX, ctxX);
                    int16_t dy = cabacDecodeMvd(cabacEngine_, cabacCtx_.data(), cCtxMvdY, ctxY);
                    if (sp == 0U) { mvdX[s] = dx; mvdY[s] = dy; }
                    subMvdX[s][sp] = dx;
                    subMvdY[s][sp] = dy;
                    // Store this sub-partition's MVD to every 4x4 block it
                    // covers so later neighbors' context derivation sees it.
                    for (int32_t r = tlY; r < tlY + spH4; ++r)
                        for (int32_t c = tlX; c < tlX + spW4; ++c)
                        {
                            uint32_t idx = mi * 16U
                                         + (static_cast<uint32_t>(r) - mb4y) * 4U
                                         + (static_cast<uint32_t>(c) - mb4x);
                            mbMotion_[idx].mvd = {dx, dy};
                        }
                }
            }
        }

        // MV prediction + MC — same logic as CAVLC path (§8.4.1.3)
        // mb4x/mb4y declared earlier for MVD context derivation
        uint32_t mbIdx = mbY * widthInMbs_ + mbX;

        uint8_t predLuma[256], predU[64], predV[64];
        std::memset(predLuma, 128U, 256U);
        std::memset(predU, 128U, 64U);
        std::memset(predV, 128U, 64U);

        // Per-partition reference lookup via DPB L0 list — §8.4.2
        auto getRef = [this](uint8_t idx) -> const Frame& {
            const Frame* f = dpb_.getReference(idx);
            if (!f) f = dpb_.getReference(0U);
            return *f;
        };

        // Per-partition MV prediction, MC, and storage
        auto doPartitionMC = [&](uint32_t partIdx, uint8_t partRefIdx,
                                  int16_t partMvdX, int16_t partMvdY,
                                  uint32_t partX, uint32_t partY,
                                  uint32_t partW, uint32_t partH)
        {
            const Frame& ref = getRef(partRefIdx);

            // MV predictor — same neighbor lookup as CAVLC
            MbMotionInfo a = getMotionAt4x4(mb4x + static_cast<int32_t>(partX) / 4 - 1,
                                             mb4y + static_cast<int32_t>(partY) / 4);
            MbMotionInfo b = getMotionAt4x4(mb4x + static_cast<int32_t>(partX) / 4,
                                             mb4y + static_cast<int32_t>(partY) / 4 - 1);
            MbMotionInfo c = getMotionAt4x4(mb4x + static_cast<int32_t>(partX + partW) / 4,
                                             mb4y + static_cast<int32_t>(partY) / 4 - 1);
            if (!c.available)
                c = getMotionAt4x4(mb4x + static_cast<int32_t>(partX) / 4 - 1,
                                    mb4y + static_cast<int32_t>(partY) / 4 - 1);

            int8_t ri = static_cast<int8_t>(partRefIdx);
            MotionVector mvp;
            // §8.4.1.3 directional shortcuts
            if (mbTypeRaw == 1U && partIdx == 0U && b.available && b.refIdx == ri)
                mvp = b.mv;
            else if (mbTypeRaw == 1U && partIdx == 1U && a.available && a.refIdx == ri)
                mvp = a.mv;
            else if (mbTypeRaw == 2U && partIdx == 0U && a.available && a.refIdx == ri)
                mvp = a.mv;
            else if (mbTypeRaw == 2U && partIdx == 1U && c.available && c.refIdx == ri)
                mvp = c.mv;
            else
                mvp = computeMvPredictor(a, b, c, ri);

            MotionVector mv = { static_cast<int16_t>(mvp.x + partMvdX),
                                static_cast<int16_t>(mvp.y + partMvdY) };

            // Store MV per-4x4-block across exactly the partition's footprint
            // — this matches sub-partitions of P_8x8 (8x4/4x8/4x4) which need
            // finer-grained coverage than setPartitionMotion's 2x2 block grid.
            {
                MbMotionInfo info = { mv, {partMvdX, partMvdY}, ri, true };
                uint32_t r0 = partY / 4U, r1 = (partY + partH) / 4U;
                uint32_t c0 = partX / 4U, c1 = (partX + partW) / 4U;
                for (uint32_t rr = r0; rr < r1; ++rr)
                    for (uint32_t cc = c0; cc < c1; ++cc)
                        mbMotion_[mbIdx * 16U + rr * 4U + cc] = info;
            }

            // Luma MC
            int32_t refX = static_cast<int32_t>(mbX * cMbSize + partX) + (mv.x >> 2);
            int32_t refY = static_cast<int32_t>(mbY * cMbSize + partY) + (mv.y >> 2);
            uint32_t dx = static_cast<uint32_t>(mv.x) & 3U;
            uint32_t dy = static_cast<uint32_t>(mv.y) & 3U;
            lumaMotionComp(ref, refX, refY, dx, dy, partW, partH,
                           predLuma + partY * cMbSize + partX, cMbSize);

            // Chroma MC
            uint32_t cPartX = partX / 2U, cPartY = partY / 2U;
            uint32_t cPartW = partW / 2U, cPartH = partH / 2U;
            int32_t cRefX = static_cast<int32_t>(mbX * cChromaBlockSize + cPartX) + (mv.x >> 3);
            int32_t cRefY = static_cast<int32_t>(mbY * cChromaBlockSize + cPartY) + (mv.y >> 3);
            uint32_t cdx = static_cast<uint32_t>(mv.x) & 7U;
            uint32_t cdy = static_cast<uint32_t>(mv.y) & 7U;
            chromaMotionComp(ref, cRefX, cRefY, cdx, cdy, cPartW, cPartH, true,
                             predU + cPartY * cChromaBlockSize + cPartX, cChromaBlockSize);
            chromaMotionComp(ref, cRefX, cRefY, cdx, cdy, cPartW, cPartH, false,
                             predV + cPartY * cChromaBlockSize + cPartX, cChromaBlockSize);

            // Weighted prediction
            if (sh.hasWeightTable_)
            {
                const auto& w = sh.weightL0_[partRefIdx];
                applyWeightedPred(predLuma + partY * cMbSize + partX, cMbSize,
                                  partW, partH, sh.lumaLog2WeightDenom_,
                                  w.lumaWeight, w.lumaOffset, w.lumaWeightFlag);
                applyWeightedPred(predU + cPartY * cChromaBlockSize + cPartX, cChromaBlockSize,
                                  cPartW, cPartH, sh.chromaLog2WeightDenom_,
                                  w.chromaWeight[0], w.chromaOffset[0], w.chromaWeightFlag);
                applyWeightedPred(predV + cPartY * cChromaBlockSize + cPartX, cChromaBlockSize,
                                  cPartW, cPartH, sh.chromaLog2WeightDenom_,
                                  w.chromaWeight[1], w.chromaOffset[1], w.chromaWeightFlag);
            }
        };

        // (per-step bp trace for this MB printed inline in MVD decode loop)

        if (mbTypeRaw == 0U)
        {
            doPartitionMC(0U, refIdxL0[0], mvdX[0], mvdY[0], 0U, 0U, 16U, 16U);
        }
        else if (mbTypeRaw == 1U) // 16x8
        {
            doPartitionMC(0U, refIdxL0[0], mvdX[0], mvdY[0], 0U, 0U, 16U, 8U);
            doPartitionMC(1U, refIdxL0[1], mvdX[1], mvdY[1], 0U, 8U, 16U, 8U);
        }
        else if (mbTypeRaw == 2U) // 8x16
        {
            doPartitionMC(0U, refIdxL0[0], mvdX[0], mvdY[0], 0U, 0U, 8U, 16U);
            doPartitionMC(1U, refIdxL0[1], mvdX[1], mvdY[1], 8U, 0U, 8U, 16U);
        }
        else // P_8x8 (and P_8x8ref0)
        {
            static constexpr uint32_t c8x8X[4] = {0U, 8U, 0U, 8U};
            static constexpr uint32_t c8x8Y[4] = {0U, 0U, 8U, 8U};
            for (uint32_t s = 0U; s < 4U; ++s)
            {
                uint32_t numSubParts = 1U;
                if (subMbType[s] == 1U || subMbType[s] == 2U) numSubParts = 2U;
                else if (subMbType[s] == 3U) numSubParts = 4U;

                for (uint32_t sp = 0U; sp < numSubParts; ++sp)
                {
                    // Offset + size within 8x8 block (4x4 units) → pixels.
                    uint32_t subX = c8x8X[s] + cSubPartLayout[subMbType[s]][sp][0] * 4U;
                    uint32_t subY = c8x8Y[s] + cSubPartLayout[subMbType[s]][sp][1] * 4U;
                    uint32_t subW = cSubPartLayout[subMbType[s]][sp][2] * 4U;
                    uint32_t subH = cSubPartLayout[subMbType[s]][sp][3] * 4U;
                    doPartitionMC(s, refIdxL0[s],
                                  subMvdX[s][sp], subMvdY[s][sp],
                                  subX, subY, subW, subH);
                }
            }
        }

        // §7.3.5: coded_block_pattern ae(v) for P-inter. [CHECKED §7.3.5]
        // §9.3.3.1.1.4: ctxIdxInc derived from left/top neighbor CBP flags.
        uint8_t cbp;
        {
            auto cbpN = cabacNeighbor_.cbpNeighbors(mbX, mbY);
            cbp = cabacDecodeCbp(cabacEngine_, cabacCtx_.data(),
                                 cbpN.left & 0x0FU, cbpN.top & 0x0FU,
                                 (cbpN.left >> 4U) & 3U, (cbpN.top >> 4U) & 3U);
        }
        uint8_t cbpLuma = cbp & 0x0FU;
        uint8_t cbpChroma = (cbp >> 4U) & 0x03U;
        cabacNeighbor_[mbIdx].cbp = cbp;

        // §7.3.5: transform_size_8x8_flag ae(v) after CBP for P-inter. [CHECKED §7.3.5]
        // Condition per spec §7.3.5: CodedBlockPatternLuma>0 && transform_8x8_mode_flag
        //   && mb_type!=I_NxN && noSubMbPartSizeLessThan8x8Flag.
        // §7.3.5 noSubMbPartSizeLessThan8x8Flag:
        //   Non-8x8 partition types (16x16, 16x8, 8x16): always 1 (no sub-partitions).
        //   P_8x8: 0 if any sub-partition has width<8 or height<8 (subMbType 1,2,3).
        //          1 only if all 4 sub-partitions are 8x8 (subMbType == 0). [CHECKED §7.3.5]
        bool noSubMbPartSizeLessThan8x8Flag = true;
        if (mbTypeRaw == 3U) // P_8x8
        {
            for (uint32_t s = 0U; s < 4U; ++s)
                if (subMbType[s] != 0U) { noSubMbPartSizeLessThan8x8Flag = false; break; }
        }
        bool use8x8Inter = false;
        if (cbpLuma != 0U && pps.transform8x8Mode_ != 0U && noSubMbPartSizeLessThan8x8Flag)
            use8x8Inter = decodeCabacTransform8x8Flag(mbX, mbY, mbIdx);
        mbTransform8x8_[mbIdx] = use8x8Inter ? 1U : 0U;

        // §7.3.5: mb_qp_delta ae(v) — only if cbp > 0 for P-inter. [CHECKED §7.3.5]
        // §9.3.3.1.1.5 / JM cabac.c L1298-1301: when cbp==0 the syntax element
        // is absent (treated as 0) → reset prevHadDelta to false so the next
        // MB's mb_qp_delta bin0 uses ctxInc=0.
        int32_t qp = mbQp;
        if (cbp > 0U)
        {
            int32_t qpDelta = cabacDecodeMbQpDelta(cabacEngine_, cabacCtx_.data(),
                                                    prevMbHadNonZeroQpDelta_);
            prevMbHadNonZeroQpDelta_ = (qpDelta != 0);
            qp += qpDelta;
            qp = ((qp % 52) + 52) % 52; // §7.4.5: proper modular wrapping
        }
        else
        {
            prevMbHadNonZeroQpDelta_ = false;
        }
        mbQp = qp; // Propagate accumulated QP

        // §7.3.5.3: residual(0,15) — luma then chroma. [CHECKED §7.3.5.3]
        uint32_t yStride = target.yStride();

        if (use8x8Inter)
        {
            // 8x8 inter transform — §7.3.5.3 with ctxBlockCat=5
            for (uint32_t blk8 = 0U; blk8 < 4U; ++blk8)
            {
                uint32_t blkX = (blk8 & 1U) * 8U;
                uint32_t blkY = (blk8 >> 1U) * 8U;
                bool hasResidual = (cbpLuma >> blk8) & 1U;

                int16_t coeffs[64] = {};
                if (hasResidual)
                {
                    int16_t scanCoeffs[64] = {};
                    cabacDecodeResidual8x8(cabacEngine_, cabacCtx_.data(), scanCoeffs);
                    for (uint32_t k = 0U; k < 64U; ++k)
                        coeffs[cZigzag8x8[k]] = scanCoeffs[k];
                    dequantize8x8(coeffs, qp, true);
                }

                // Mark constituent 4x4 blocks NNZ
                uint32_t base4x4 = (blk8 >> 1U) * 8U + (blk8 & 1U) * 2U;
                for (uint32_t ny = 0U; ny < 2U; ++ny)
                    for (uint32_t nx = 0U; nx < 2U; ++nx)
                        nnzLuma_[mbIdx * 16U + base4x4 + ny * 4U + nx] = hasResidual ? 1U : 0U;

                uint8_t* predPtr = predLuma + blkY * cMbSize + blkX;
                uint8_t* outPtr = target.yMb(mbX, mbY) + blkY * yStride + blkX;
                inverseDct8x8AddPred(coeffs, predPtr, cMbSize, outPtr, yStride);
            }
        }
        else
        {
            // 4x4 inter transform (spec scan order §6.4.3)
            for (uint32_t blkIdx = 0U; blkIdx < 16U; ++blkIdx)
            {
                uint32_t blkX = cLuma4x4BlkX[blkIdx];
                uint32_t blkY = cLuma4x4BlkY[blkIdx];
                uint32_t rasterIdx = cLuma4x4ToRaster[blkIdx];
                uint32_t group8x8 = blkIdx >> 2U;

                int16_t coeffs[16] = {};
                if ((cbpLuma >> group8x8) & 1U)
                {
                    // §9.3.3.1.1.9: coded_block_flag ctxIdxInc for luma 4x4 (cat 2)
                    // For P-inter: unavailable neighbor → condTermFlag=0 (verified empirically).
                    // NOTE: spec §9.3.3.1.1.9 says condTermFlag=1 for ctxBlockCat<5, but
                    // that is the intra interpretation — for inter the effective behavior is 0.
                    // [TODO: verify against reference encoder output vs spec §9.3.3.1.1.9]
                    uint32_t leftNnz = 0U, topNnz = 0U; // unavailable → 0 for P-inter (empirical)
                    if (rasterIdx % 4U > 0U)
                        leftNnz = nnzLuma_[mbIdx * 16U + rasterIdx - 1U];
                    else if (mbX > 0U)
                        leftNnz = nnzLuma_[(mbIdx - 1U) * 16U + rasterIdx + 3U];
                    if (rasterIdx >= 4U)
                        topNnz = nnzLuma_[mbIdx * 16U + rasterIdx - 4U];
                    else if (mbY > 0U)
                        topNnz = nnzLuma_[(mbIdx - widthInMbs_) * 16U + rasterIdx + 12U];
                    uint32_t cbfCtxInc = (leftNnz != 0U ? 1U : 0U) + (topNnz != 0U ? 2U : 0U);

                    int16_t scanCoeffs[16] = {};
                    uint32_t numNonZero = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                                  scanCoeffs, 16U, 2U, cbfCtxInc);
                    // §6.4.3 unscan zigzag → raster order BEFORE dequant/IDCT.
                    for (uint32_t k = 0U; k < 16U; ++k)
                        coeffs[cZigzag4x4[k]] = scanCoeffs[k];
                    nnzLuma_[mbIdx * 16U + rasterIdx] = (numNonZero > 0U) ? 1U : 0U;
                    dequantize4x4(coeffs, qp, Block4x4Plane::LumaInter);
                }
                else
                {
                    nnzLuma_[mbIdx * 16U + rasterIdx] = 0U;
                }

                uint8_t* predPtr = predLuma + blkY * cMbSize + blkX;
                uint8_t* outPtr = target.yMb(mbX, mbY) + blkY * yStride + blkX;
                inverseDct4x4AddPred(coeffs, predPtr, cMbSize, outPtr, yStride);
            }
        }

        // Chroma residual
        // §7.4.2.2 Chroma QP (FM-10: must use chromaQp, not luma QP)
        int32_t chromaQp = computeChromaQp(qp, pps.chromaQpIndexOffset_);

        // §9.3.3.1.1.9: chroma DC cbf context — uses actual neighbor CBF.
        // P-inter current MB is NOT intra.
        uint32_t cbDcCbfInc2 = cabacNeighbor_.chromaDcCbfCtxInc(mbX, mbY, false, true);
        uint32_t crDcCbfInc2 = cabacNeighbor_.chromaDcCbfCtxInc(mbX, mbY, false, false);

        int16_t dcCb[4] = {}, dcCr[4] = {};
        if (cbpChroma >= 1U)
        {
            uint32_t cbDcNnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        dcCb, 4U, 3U, cbDcCbfInc2);
            uint32_t crDcNnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        dcCr, 4U, 3U, crDcCbfInc2);
            cabacNeighbor_[mbIdx].setCbDcCbf(cbDcNnz > 0U);
            cabacNeighbor_[mbIdx].setCrDcCbf(crDcNnz > 0U);
            inverseHadamard2x2(dcCb);
            inverseHadamard2x2(dcCr);

            // CABAC P-inter chroma DC → CbInter/CrInter weightScale[0][0].
            dequantChromaDcValues(dcCb, chromaQp, scalingLists_.list4x4[4][0]);
            dequantChromaDcValues(dcCr, chromaQp, scalingLists_.list4x4[5][0]);
        }

        // §7.3.5.3: Chroma AC — ALL Cb first, then ALL Cr. Same order fix as intra.
        int16_t cbAcPInter[4][16] = {}, crAcPInter[4][16] = {};
        if (cbpChroma >= 2U)
        {
            for (uint32_t bi = 0U; bi < 4U; ++bi)
            {
                uint32_t cbfInc = chromaAcCbfCtxInc(mbX, mbY, bi, true, false);
                int16_t acScan[16] = {};
                uint32_t nnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        acScan, 15U, 4U, cbfInc);
                for (uint32_t i = 0U; i < 15U; ++i)
                    cbAcPInter[bi][cZigzag4x4[i + 1U]] = acScan[i];
                nnzCb_[mbIdx * 4U + bi] = (nnz > 0U) ? 1U : 0U;
            }
            for (uint32_t bi = 0U; bi < 4U; ++bi)
            {
                uint32_t cbfInc = chromaAcCbfCtxInc(mbX, mbY, bi, false, false);
                int16_t acScan[16] = {};
                uint32_t nnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        acScan, 15U, 4U, cbfInc);
                for (uint32_t i = 0U; i < 15U; ++i)
                    crAcPInter[bi][cZigzag4x4[i + 1U]] = acScan[i];
                nnzCr_[mbIdx * 4U + bi] = (nnz > 0U) ? 1U : 0U;
            }
        }


        uint32_t uvStride = target.uvStride();
        for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
        {
            uint32_t blkX = (blkIdx & 1U) * 4U;
            uint32_t blkY = (blkIdx >> 1U) * 4U;
            int16_t cbCoeffs[16] = {}, crCoeffs[16] = {};
            cbCoeffs[0] = dcCb[blkIdx];
            crCoeffs[0] = dcCr[blkIdx];

            if (cbpChroma >= 2U)
            {
                for (uint32_t k = 1U; k < 16U; ++k)
                {
                    cbCoeffs[k] = cbAcPInter[blkIdx][k];
                    crCoeffs[k] = crAcPInter[blkIdx][k];
                }
                int16_t savedDcCb = cbCoeffs[0];
                dequantize4x4(cbCoeffs, chromaQp, Block4x4Plane::CbInter);
                cbCoeffs[0] = savedDcCb;
                int16_t savedDcCr = crCoeffs[0];
                dequantize4x4(crCoeffs, chromaQp, Block4x4Plane::CrInter);
                crCoeffs[0] = savedDcCr;
            }

            inverseDct4x4AddPred(cbCoeffs, predU + blkY * 8U + blkX, 8U, target.uMb(mbX, mbY) + blkY * uvStride + blkX, uvStride);
            inverseDct4x4AddPred(crCoeffs, predV + blkY * 8U + blkX, 8U, target.vMb(mbX, mbY) + blkY * uvStride + blkX, uvStride);
        }
    }

    /** Decode chroma using CABAC residual. */
    void decodeChromaCabac(const Pps& pps, uint32_t chromaPredMode,
                            uint8_t cbpChroma, int32_t qp,
                            uint32_t mbX, uint32_t mbY) noexcept
    {
        auto chromaMode = static_cast<IntraChromaMode>(chromaPredMode);
        uint8_t predU[64], predV[64];
        intraPredChroma8x8(chromaMode, *activeFrame_, mbX, mbY, true, predU);
        intraPredChroma8x8(chromaMode, *activeFrame_, mbX, mbY, false, predV);

        // §7.4.2.2 Chroma QP (FM-10: must use chromaQp, not luma QP)
        int32_t chromaQp = computeChromaQp(qp, pps.chromaQpIndexOffset_);

        uint32_t mbIdx = mbY * widthInMbs_ + mbX;

        // §9.3.3.1.1.9: chroma DC cbf context — uses actual neighbor CBF.
        // transBlockN available when neighbor not skip/I_PCM AND CodedBlockPatternChroma != 0.
        uint32_t cbDcCbfInc = cabacNeighbor_.chromaDcCbfCtxInc(mbX, mbY, true, true);
        uint32_t crDcCbfInc = cabacNeighbor_.chromaDcCbfCtxInc(mbX, mbY, true, false);

        int16_t dcCb[4] = {}, dcCr[4] = {};
        if (cbpChroma >= 1U)
        {
            uint32_t cbDcNnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        dcCb, 4U, 3U, cbDcCbfInc);
            uint32_t crDcNnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        dcCr, 4U, 3U, crDcCbfInc);
            cabacNeighbor_[mbIdx].setCbDcCbf(cbDcNnz > 0U);
            cabacNeighbor_[mbIdx].setCrDcCbf(crDcNnz > 0U);
            inverseHadamard2x2(dcCb);
            inverseHadamard2x2(dcCr);

            // Intra chroma DC → CbIntra/CrIntra weightScale[0][0].
            dequantChromaDcValues(dcCb, chromaQp, scalingLists_.list4x4[1][0]);
            dequantChromaDcValues(dcCr, chromaQp, scalingLists_.list4x4[2][0]);
        }

        // §7.3.5.3: Chroma AC decode — ALL Cb blocks first, then ALL Cr blocks.
        // Spec loop: for(iCbCr=0..1) for(i4x4=0..3) residual_block_cabac().
        // MUST NOT interleave Cb/Cr — that desyncs the CABAC engine.
        int16_t cbAcCoeffs[4][16] = {}, crAcCoeffs[4][16] = {};
        if (cbpChroma >= 2U)
        {
            for (uint32_t bi = 0U; bi < 4U; ++bi)
            {
                uint32_t cbfInc = chromaAcCbfCtxInc(mbX, mbY, bi, true, true);
                int16_t acScan[16] = {};
                uint32_t nnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        acScan, 15U, 4U, cbfInc);
                for (uint32_t i = 0U; i < 15U; ++i)
                    cbAcCoeffs[bi][cZigzag4x4[i + 1U]] = acScan[i];
                nnzCb_[mbIdx * 4U + bi] = (nnz > 0U) ? 1U : 0U;
            }
            for (uint32_t bi = 0U; bi < 4U; ++bi)
            {
                uint32_t cbfInc = chromaAcCbfCtxInc(mbX, mbY, bi, false, true);
                int16_t acScan[16] = {};
                uint32_t nnz = cabacDecodeResidual4x4(cabacEngine_, cabacCtx_.data(),
                                                        acScan, 15U, 4U, cbfInc);
                for (uint32_t i = 0U; i < 15U; ++i)
                    crAcCoeffs[bi][cZigzag4x4[i + 1U]] = acScan[i];
                nnzCr_[mbIdx * 4U + bi] = (nnz > 0U) ? 1U : 0U;
            }
        }

        // Reconstruct chroma — dequant AC, preserve DC, IDCT+pred
        uint8_t* mbU = activeFrame_->uMb(mbX, mbY);
        uint8_t* mbV = activeFrame_->vMb(mbX, mbY);
        uint32_t uvStride = activeFrame_->uvStride();
        for (uint32_t blkIdx = 0U; blkIdx < 4U; ++blkIdx)
        {
            uint32_t blkX = (blkIdx & 1U) * 4U;
            uint32_t blkY = (blkIdx >> 1U) * 4U;
            int16_t cbCoeffs[16] = {}, crCoeffs[16] = {};
            cbCoeffs[0] = dcCb[blkIdx]; crCoeffs[0] = dcCr[blkIdx];

            if (cbpChroma >= 2U)
            {
                for (uint32_t k = 1U; k < 16U; ++k)
                {
                    cbCoeffs[k] = cbAcCoeffs[blkIdx][k];
                    crCoeffs[k] = crAcCoeffs[blkIdx][k];
                }
                int16_t savedDcCb = cbCoeffs[0];
                dequantize4x4(cbCoeffs, chromaQp, Block4x4Plane::CbIntra);
                cbCoeffs[0] = savedDcCb;
                int16_t savedDcCr = crCoeffs[0];
                dequantize4x4(crCoeffs, chromaQp, Block4x4Plane::CrIntra);
                crCoeffs[0] = savedDcCr;
            }

            inverseDct4x4AddPred(cbCoeffs, predU + blkY * 8U + blkX, 8U, mbU + blkY * uvStride + blkX, uvStride);
            inverseDct4x4AddPred(crCoeffs, predV + blkY * 8U + blkX, 8U, mbV + blkY * uvStride + blkX, uvStride);
        }
    }
};

} // namespace sub0h264

#endif // CROG_SUB0H264_DECODER_HPP
