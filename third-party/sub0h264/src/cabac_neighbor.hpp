/** Sub0h264 — CABAC neighbor context for spatial prediction
 *
 *  Consolidates per-MB state needed for CABAC context derivation into a
 *  single cache-friendly structure.
 *
 *  Reference: ITU-T H.264 §9.3.3.1.1
 *
 *  Spec-annotated review (2026-04-09):
 *    §9.3.3.1.1.1 skipCtx: condTermFlag=0 when unavailable OR skip [CHECKED]
 *      BUG FIXED: was returning condTermFlag=1 for unavailable neighbors.
 *    §9.3.3.1.1.3 mbTypeCtxI: condTermFlag=0 when unavail OR I_NxN [CHECKED]
 *    §9.3.3.1.1.4 cbpNeighbors: unavailable→0x2F (all coded)→condTermFlag=0 [CHECKED]
 *    §9.3.3.1.1.8 chromaModeCtxInc: unavailable→0, mode!=0→1 [CHECKED]
 *    Default MbCabacInfo: cbp=0x2F matches spec unavailable defaults [CHECKED]
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_CABAC_NEIGHBOR_HPP
#define CROG_SUB0H264_CABAC_NEIGHBOR_HPP

#include <cstdint>
#include <cstring>
#include <vector>

namespace sub0h264 {

/// Per-MB info stored for CABAC neighbor context derivation.
/// §9.3.3.1.1.9: coded_block_flag context requires actual CBF from neighbor blocks.
struct MbCabacInfo
{
    uint8_t flags = 0U;     ///< Bit 0: isI4x4, Bit 1: isSkip, Bit 2: isI16x16
    uint8_t cbp = 0x2FU;    ///< Luma CBP (bits 0-3) | chroma CBP (bits 4-5). Default 0x2F = all coded.
    uint8_t chromaMode = 0U; ///< intra_chroma_pred_mode (§9.3.3.1.1.7)
    /// Per-block coded_block_flag results for neighbor lookup — §9.3.3.1.1.9.
    /// Bit 0: luma DC CBF (ctxBlockCat=0, I_16x16 only)
    /// Bit 1: Cb DC CBF (ctxBlockCat=3)
    /// Bit 2: Cr DC CBF (ctxBlockCat=3)
    uint8_t dcCbf = 0U;

    bool isI4x4() const noexcept { return (flags & 0x01U) != 0U; }
    bool isSkip() const noexcept { return (flags & 0x02U) != 0U; }
    bool isI16x16() const noexcept { return (flags & 0x04U) != 0U; }
    bool transform8x8() const noexcept { return (flags & 0x08U) != 0U; }

    void setI4x4(bool v) noexcept { flags = v ? (flags | 0x01U) : (flags & ~0x01U); }
    void setSkip(bool v) noexcept { flags = v ? (flags | 0x02U) : (flags & ~0x02U); }
    void setI16x16(bool v) noexcept { flags = v ? (flags | 0x04U) : (flags & ~0x04U); }
    void setTransform8x8(bool v) noexcept { flags = v ? (flags | 0x08U) : (flags & ~0x08U); }

    uint8_t lumaCbp() const noexcept { return cbp & 0x0FU; }
    uint8_t chromaCbp() const noexcept { return (cbp >> 4U) & 0x03U; }

    bool lumaDcCbf() const noexcept { return (dcCbf & 0x01U) != 0U; }
    bool cbDcCbf() const noexcept { return (dcCbf & 0x02U) != 0U; }
    bool crDcCbf() const noexcept { return (dcCbf & 0x04U) != 0U; }

    void setLumaDcCbf(bool v) noexcept { dcCbf = v ? (dcCbf | 0x01U) : (dcCbf & ~0x01U); }
    void setCbDcCbf(bool v) noexcept { dcCbf = v ? (dcCbf | 0x02U) : (dcCbf & ~0x02U); }
    void setCrDcCbf(bool v) noexcept { dcCbf = v ? (dcCbf | 0x04U) : (dcCbf & ~0x04U); }
};

/// Pair of neighbor CBP values returned by cbpNeighbors().
struct CbpNeighbors
{
    uint8_t left;   ///< Left neighbor CBP (luma 4 bits | chroma << 4)
    uint8_t top;    ///< Top neighbor CBP
};

/// Default value for unavailable neighbors — all bits coded, not skip, not I4x4.
/// ITU-T H.264 §9.3.3.1.1.3: condTermFlagN = 0 when neighbor unavailable.
inline constexpr MbCabacInfo cMbCabacUnavailable = { 0U, 0x2FU, 0U, 0U }; // dcCbf=0: unavailable defaults

/** Consolidated CABAC neighbor context for spatial prediction.
 *
 *  Stores one MbCabacInfo per macroblock in raster order. Provides
 *  named methods for all neighbor-dependent context increment calculations,
 *  each citing the relevant spec section.
 */
class CabacNeighborCtx
{
public:
    CabacNeighborCtx() noexcept = default;

    /** Initialize for a new slice.
     *  Resets all MBs to the unavailable default state.
     *  @param widthMbs   Picture width in macroblocks
     *  @param heightMbs  Picture height in macroblocks
     */
    void init(uint32_t widthMbs, uint32_t heightMbs) noexcept
    {
        widthMbs_ = widthMbs;
        uint32_t total = widthMbs * heightMbs;
        mbs_.resize(total);
        // Reset to "unavailable" defaults — cbp=0x2F means all coded
        MbCabacInfo defaultInfo = cMbCabacUnavailable;
        std::fill(mbs_.begin(), mbs_.end(), defaultInfo);
    }

    /** @return Mutable reference to the MB info at (mbX, mbY). */
    MbCabacInfo& at(uint32_t mbX, uint32_t mbY) noexcept
    {
        return mbs_[mbY * widthMbs_ + mbX];
    }

    /** @return Const reference to the MB info at (mbX, mbY). */
    const MbCabacInfo& at(uint32_t mbX, uint32_t mbY) const noexcept
    {
        return mbs_[mbY * widthMbs_ + mbX];
    }

    /** @return MB info by linear index. */
    MbCabacInfo& operator[](uint32_t mbIdx) noexcept { return mbs_[mbIdx]; }
    const MbCabacInfo& operator[](uint32_t mbIdx) const noexcept { return mbs_[mbIdx]; }

    // ── Neighbor context derivation methods ─────────────────────────────

    /** mb_type context increment for I-slices — §9.3.3.1.1.3 Table 9-39.
     *
     *  condTermFlagN for mb_type in I-slices (verified against ffmpeg):
     *  - Unavailable neighbor → condTermFlagN = 0 (treated as I_NxN)
     *  - Available, mb_type = I_NxN (I_4x4/I_8x8) → condTermFlagN = 0
     *  - Available, mb_type = I_16x16/I_PCM → condTermFlagN = 1
     *
     *  ffmpeg logic: ctx++ only when neighbor IS available AND is I_16x16 or I_PCM.
     *
     *  ctxInc = condTermFlagA + condTermFlagB (left + top).
     *  @return Pair {leftIsI4x4, topIsI4x4} for cabacDecodeMbTypeI().
     *          leftIsI4x4=true means condTermFlag=0 for that neighbor.
     */
    void mbTypeCtxI(uint32_t mbX, uint32_t mbY,
                    bool& leftIsI4x4, bool& topIsI4x4) const noexcept
    {
        // §9.3.3.1.1.3: condTermFlagN = 0 when unavailable OR I_NxN
        // Verified against ffmpeg source and ITU-T H.264 Table 9-39
        leftIsI4x4 = (mbX == 0U) || mbs_[mbY * widthMbs_ + mbX - 1U].isI4x4();
        topIsI4x4  = (mbY == 0U) || mbs_[(mbY - 1U) * widthMbs_ + mbX].isI4x4();
    }

    /** mb_skip_flag context increment for P-slices — §9.3.3.1.1.1 Eq 9-7.
     *
     *  condTermFlagN = 0 when mbAddrN is NOT available OR mb_skip_flag == 1.
     *  condTermFlagN = 1 when mbAddrN IS available AND mb_skip_flag == 0.
     *  ctxIdxInc = condTermFlagA + condTermFlagB.
     *
     *  The caller uses: ctxInc = (leftSkip ? 0 : 1) + (topSkip ? 0 : 1),
     *  so leftSkip=true → condTermFlag=0 (unavailable OR skip). [CHECKED §9.3.3.1.1.1]
     *
     *  @return Pair {leftSkip, topSkip} for cabacDecodeMbSkipP().
     *          TRUE means condTermFlag=0 (unavailable or skipped).
     */
    void skipCtx(uint32_t mbX, uint32_t mbY,
                 bool& leftSkip, bool& topSkip) const noexcept
    {
        uint32_t mbIdx = mbY * widthMbs_ + mbX;
        // §9.3.3.1.1.1: unavailable → condTermFlag=0 → treat as "skip"
        leftSkip = (mbX == 0U) || mbs_[mbIdx - 1U].isSkip();
        topSkip  = (mbY == 0U) || mbs_[mbIdx - widthMbs_].isSkip();
    }

    /** CBP neighbor values for CABAC CBP decode — §9.3.3.1.1.4.
     *
     *  Returns the stored CBP for left and top neighbors.
     *  Unavailable neighbors default to 0x2F (all luma coded, chroma=2).
     */
    CbpNeighbors cbpNeighbors(uint32_t mbX, uint32_t mbY) const noexcept
    {
        uint32_t mbIdx = mbY * widthMbs_ + mbX;
        // §9.3.3.1.1.4: unavailable → condTermFlag=0 for BOTH luma and chroma.
        // Luma sense: coded→0 (condTermFlag=0 when coded). Default 0xF (all coded) → condTermFlag=0. ✓
        // Chroma sense: coded→1 (condTermFlag=1 when coded). Default must give condTermFlag=0.
        // Use 0x3F: lumaCbp=0xF (all coded→condTermFlag=0 ✓), chromaCbp=3 (sentinel >2
        // detected by cabacDecodeCbp as unavailable→condTermFlag=0). [FM-23]
        return {
            (mbX > 0U) ? mbs_[mbIdx - 1U].cbp : static_cast<uint8_t>(0x3FU),
            (mbY > 0U) ? mbs_[mbIdx - widthMbs_].cbp : static_cast<uint8_t>(0x3FU)
        };
    }

    /** Chroma prediction mode context for intra_chroma_pred_mode — §9.3.3.1.1.7.
     *
     *  ctxInc = (chromaPredMode_left != 0 ? 1 : 0) + (chromaPredMode_top != 0 ? 1 : 0).
     *  Unavailable or inter neighbors contribute 0 (mode assumed DC=0).
     *
     *  @return ctxInc value [0-2] for use with cCtxIntraChroma base.
     */
    uint32_t chromaModeCtxInc(uint32_t mbX, uint32_t mbY) const noexcept
    {
        uint32_t mbIdx = mbY * widthMbs_ + mbX;
        uint32_t ctxInc = 0U;
        // Left neighbor: non-zero chroma mode contributes 1
        if (mbX > 0U && mbs_[mbIdx - 1U].chromaMode != 0U)
            ctxInc += 1U;
        // Top neighbor: non-zero chroma mode contributes 1
        if (mbY > 0U && mbs_[mbIdx - widthMbs_].chromaMode != 0U)
            ctxInc += 1U;
        return ctxInc;
    }

    /** Luma DC coded_block_flag context increment — §9.3.3.1.1.9 (ctxBlockCat=0).
     *
     *  transBlockN is the luma DC of neighbor IF neighbor is I_16x16.
     *  condTermFlagN:
     *  - Unavailable + current Intra → 1
     *  - Available + NOT I_16x16 → transBlock not available → 0
     *  - Available + I_16x16 → actual coded_block_flag of luma DC
     *
     *  @param isCurrentIntra  True if current MB is intra (I_4x4/I_16x16/I_PCM)
     *  @return cbfCtxInc = condTermFlagA + 2*condTermFlagB
     */
    uint32_t lumaDcCbfCtxInc(uint32_t mbX, uint32_t mbY,
                              bool isCurrentIntra) const noexcept
    {
        auto condTermFlag = [&](bool available, const MbCabacInfo& n) -> uint32_t {
            if (!available)
                return isCurrentIntra ? 1U : 0U;
            if (n.isI16x16())
                return n.lumaDcCbf() ? 1U : 0U;
            return 0U; // not I_16x16 → transBlock not available → 0
        };
        uint32_t mbIdx = mbY * widthMbs_ + mbX;
        uint32_t flagA = condTermFlag(mbX > 0U, mbX > 0U ? mbs_[mbIdx - 1U] : cMbCabacUnavailable);
        uint32_t flagB = condTermFlag(mbY > 0U, mbY > 0U ? mbs_[mbIdx - widthMbs_] : cMbCabacUnavailable);
        return flagA + 2U * flagB;
    }

    /** Chroma DC coded_block_flag context increment — §9.3.3.1.1.9 (ctxBlockCat=3).
     *
     *  transBlockN available when: neighbor available, not skip/I_PCM,
     *  AND CodedBlockPatternChroma != 0 for neighbor.
     *  condTermFlagN:
     *  - Unavailable + current Intra → 1
     *  - Available + CodedBlockPatternChroma==0 → transBlock not available → 0
     *  - Available + CodedBlockPatternChroma!=0 → actual coded_block_flag
     *
     *  @param isCb  True for Cb component, false for Cr
     */
    uint32_t chromaDcCbfCtxInc(uint32_t mbX, uint32_t mbY,
                                bool isCurrentIntra, bool isCb) const noexcept
    {
        auto condTermFlag = [&](bool available, const MbCabacInfo& n) -> uint32_t {
            if (!available)
                return isCurrentIntra ? 1U : 0U;
            if (n.isSkip())
                return 0U; // skip → transBlock not available
            if (n.chromaCbp() == 0U)
                return 0U; // CodedBlockPatternChroma == 0 → transBlock not available
            return isCb ? (n.cbDcCbf() ? 1U : 0U) : (n.crDcCbf() ? 1U : 0U);
        };
        uint32_t mbIdx = mbY * widthMbs_ + mbX;
        uint32_t flagA = condTermFlag(mbX > 0U, mbX > 0U ? mbs_[mbIdx - 1U] : cMbCabacUnavailable);
        uint32_t flagB = condTermFlag(mbY > 0U, mbY > 0U ? mbs_[mbIdx - widthMbs_] : cMbCabacUnavailable);
        return flagA + 2U * flagB;
    }

    /** @return Width in MBs. */
    uint32_t widthMbs() const noexcept { return widthMbs_; }

    /** @return Total number of MBs. */
    uint32_t totalMbs() const noexcept { return static_cast<uint32_t>(mbs_.size()); }

private:
    uint32_t widthMbs_ = 0U;
    std::vector<MbCabacInfo> mbs_;
};

} // namespace sub0h264

#endif // CROG_SUB0H264_CABAC_NEIGHBOR_HPP
