/** Sub0h264 — Slice header parser
 *
 *  Parses H.264 slice headers. Reference: ITU-T H.264 §7.3.3
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_SLICE_HPP
#define CROG_SUB0H264_SLICE_HPP

#include "bitstream.hpp"
#include "sps.hpp"
#include "pps.hpp"
#include "sub0h264/sub0h264_types.hpp"

#include <cstdint>

namespace sub0h264 {

/** H.264 slice types. Values 5-9 map to 0-4 (all slices in picture are same type). */
/// H.264 slice types — ITU-T H.264 Table 7-6.
enum class SliceType : uint8_t
{
    P  = 0U,
    B  = 1U,
    I  = 2U,
    SP = 3U,
    SI = 4U,
};

/** Memory management control operation — ITU-T H.264 §7.3.3.3 Table 7-9. */
struct MmcoCommand
{
    uint8_t op = 0U;       ///< memory_management_control_operation (1-6)
    uint32_t value1 = 0U;  ///< difference_of_pic_nums_minus1 / long_term_pic_num / long_term_frame_idx
    uint32_t value2 = 0U;  ///< long_term_frame_idx (for op=3) / max_long_term_frame_idx_plus1 (for op=4)
};

/** Decoded reference picture marking operation (for IDR and non-IDR). */
struct DecRefPicMarking
{
    bool noOutputOfPriorPics_ = false;
    bool longTermReference_ = false;
    bool adaptiveRefPicMarking_ = false;

    /// MMCO commands — §7.3.3.3. Up to 32 commands.
    static constexpr uint32_t cMaxMmcoCommands = 32U;
    MmcoCommand mmcoCommands_[cMaxMmcoCommands] = {};
    uint32_t numMmcoCommands_ = 0U;
};

/** Parsed slice header. */
struct SliceHeader
{
    // Core fields
    uint32_t firstMbInSlice_ = 0U;
    SliceType sliceType_ = SliceType::I;
    uint8_t ppsId_ = 0U;
    uint16_t frameNum_ = 0U;

    // IDR-specific
    bool isIdr_ = false;
    uint32_t idrPicId_ = 0U;

    // Picture order count (POC)
    int32_t picOrderCntLsb_ = 0;
    int32_t deltaPicOrderCntBottom_ = 0;
    int32_t deltaPicOrderCnt_[2] = {};

    // Field-specific (we support frame-only, but parse for compatibility)
    bool fieldPicFlag_ = false;
    bool bottomFieldFlag_ = false;

    // Redundant picture count
    uint8_t redundantPicCnt_ = 0U;

    // Reference picture list modification — §7.3.3.1
    bool refPicListModificationL0_ = false;
    bool refPicListModificationL1_ = false;
    /// Reordering commands: pairs of (idc, value). idc=3 terminates.
    static constexpr uint32_t cMaxReorderCmds = 32U;
    struct ReorderCmd
    {
        uint8_t idc = 3U;   ///< modification_of_pic_nums_idc (0-3)
        uint32_t value = 0U; ///< abs_diff_pic_num_minus1 or long_term_pic_num
    };
    ReorderCmd reorderCmdsL0_[cMaxReorderCmds] = {};
    uint32_t numReorderCmdsL0_ = 0U;
    ReorderCmd reorderCmdsL1_[cMaxReorderCmds] = {};
    uint32_t numReorderCmdsL1_ = 0U;

    // QP
    int32_t sliceQpDelta_ = 0;
    // Effective SliceQPY = pps.picInitQp_ + sliceQpDelta_ (§7.4.3). No inline
    // helper here — sliceQp() requires PPS context unavailable in SliceHeader.

    // Deblocking filter
    uint8_t disableDeblockingFilter_ = 0U;   ///< 0=enabled, 1=disabled, 2=disabled at slice boundary
    int32_t sliceAlphaC0Offset_ = 0;         ///< FilterOffsetA = slice_alpha_c0_offset_div2 * 2 (§8.7.2)
    int32_t sliceBetaOffset_ = 0;            ///< FilterOffsetB = slice_beta_offset_div2 * 2 (§8.7.2)

    // CABAC
    uint8_t cabacInitIdc_ = 0U;              ///< 0-2 (only for CABAC slices)

    // B-slice specific
    bool directSpatialMvPred_ = false;
    uint8_t numRefIdxActiveL0_ = 0U;
    uint8_t numRefIdxActiveL1_ = 0U;

    // Dec ref pic marking
    DecRefPicMarking decRefPicMarking_;

    // Weighted prediction table — §7.3.3.2
    // Stored when weighted_pred_flag=1 (P) or weighted_bipred_idc=1 (B).
    static constexpr uint32_t cMaxRefs = 16U;
    uint8_t lumaLog2WeightDenom_ = 0U;
    uint8_t chromaLog2WeightDenom_ = 0U;
    struct WeightEntry
    {
        int16_t lumaWeight = 0;
        int16_t lumaOffset = 0;
        int16_t chromaWeight[2] = {};
        int16_t chromaOffset[2] = {};
        bool lumaWeightFlag = false;
        bool chromaWeightFlag = false;
    };
    WeightEntry weightL0_[cMaxRefs] = {};
    WeightEntry weightL1_[cMaxRefs] = {};
    bool hasWeightTable_ = false;
};

/** Parse pred_weight_table() — ITU-T H.264 §7.3.3.2.
 *  Stores weight/offset values in the slice header for weighted MC.
 *
 *  Field order per §7.3.3.2:
 *    luma_log2_weight_denom ue(v)
 *    if( ChromaArrayType != 0 ) chroma_log2_weight_denom ue(v)
 *    for i in [0, numRefIdxL0]:
 *      luma_weight_l0_flag u(1); if set: luma_weight_l0[i] se(v), luma_offset_l0[i] se(v)
 *      if( ChromaArrayType != 0 ): chroma_weight_l0_flag u(1); ...
 *    if( slice_type % 5 == 1 ): L1 list mirrors L0 structure [CHECKED §7.3.3.2]
 */
inline void parsePredWeightTable(BitReader& br, SliceHeader& sh,
                                  int32_t chromaFormatIdc) noexcept
{
    sh.lumaLog2WeightDenom_ = static_cast<uint8_t>(br.readUev());

    if (chromaFormatIdc != 0)
        sh.chromaLog2WeightDenom_ = static_cast<uint8_t>(br.readUev());

    // Default weights: 2^logWD (implicit 1.0 scaling)
    int16_t defaultLumaW = static_cast<int16_t>(1 << sh.lumaLog2WeightDenom_);
    int16_t defaultChromaW = static_cast<int16_t>(1 << sh.chromaLog2WeightDenom_);

    for (uint8_t i = 0U; i < sh.numRefIdxActiveL0_; ++i)
    {
        sh.weightL0_[i].lumaWeight = defaultLumaW;
        sh.weightL0_[i].lumaOffset = 0;
        sh.weightL0_[i].lumaWeightFlag = false;
        bool lumaWeightFlag = br.readBit() != 0U;
        if (lumaWeightFlag)
        {
            sh.weightL0_[i].lumaWeight = static_cast<int16_t>(br.readSev());
            sh.weightL0_[i].lumaOffset = static_cast<int16_t>(br.readSev());
            sh.weightL0_[i].lumaWeightFlag = true;
        }
        sh.weightL0_[i].chromaWeight[0] = defaultChromaW;
        sh.weightL0_[i].chromaWeight[1] = defaultChromaW;
        sh.weightL0_[i].chromaOffset[0] = 0;
        sh.weightL0_[i].chromaOffset[1] = 0;
        sh.weightL0_[i].chromaWeightFlag = false;
        if (chromaFormatIdc != 0)
        {
            bool chromaWeightFlag = br.readBit() != 0U;
            if (chromaWeightFlag)
            {
                sh.weightL0_[i].chromaWeightFlag = true;
                for (uint32_t j = 0U; j < 2U; ++j)
                {
                    sh.weightL0_[i].chromaWeight[j] = static_cast<int16_t>(br.readSev());
                    sh.weightL0_[i].chromaOffset[j] = static_cast<int16_t>(br.readSev());
                }
            }
        }
    }

    if (sh.sliceType_ == SliceType::B)
    {
        for (uint8_t i = 0U; i < sh.numRefIdxActiveL1_; ++i)
        {
            sh.weightL1_[i].lumaWeight = defaultLumaW;
            sh.weightL1_[i].lumaOffset = 0;
            sh.weightL1_[i].lumaWeightFlag = false;
            bool lumaWeightFlag = br.readBit() != 0U;
            if (lumaWeightFlag)
            {
                sh.weightL1_[i].lumaWeight = static_cast<int16_t>(br.readSev());
                sh.weightL1_[i].lumaOffset = static_cast<int16_t>(br.readSev());
                sh.weightL1_[i].lumaWeightFlag = true;
            }
            sh.weightL1_[i].chromaWeight[0] = defaultChromaW;
            sh.weightL1_[i].chromaWeight[1] = defaultChromaW;
            sh.weightL1_[i].chromaOffset[0] = 0;
            sh.weightL1_[i].chromaOffset[1] = 0;
            sh.weightL1_[i].chromaWeightFlag = false;
            if (chromaFormatIdc != 0)
            {
                bool chromaWeightFlag = br.readBit() != 0U;
                if (chromaWeightFlag)
                {
                    sh.weightL1_[i].chromaWeightFlag = true;
                    for (uint32_t j = 0U; j < 2U; ++j)
                    {
                        sh.weightL1_[i].chromaWeight[j] = static_cast<int16_t>(br.readSev());
                        sh.weightL1_[i].chromaOffset[j] = static_cast<int16_t>(br.readSev());
                    }
                }
            }
        }
    }

    sh.hasWeightTable_ = true;
}

/** Parse ref_pic_list_modification() — ITU-T H.264 §7.3.3.1.
 *  Stores reordering commands in the SliceHeader for use during
 *  reference list construction per §8.2.4.3.
 *
 *  L0: present when slice_type % 5 != 2 (I) && slice_type % 5 != 4 (SI).
 *  L1: present when slice_type % 5 == 1 (B).
 *  modification_of_pic_nums_idc terminates loop when == 3. [CHECKED §7.3.3.1]
 *  NOTE: MVC NAL types 20/21 call ref_pic_list_mvc_modification() per spec;
 *  we always call the non-MVC variant (MVC not supported). [PARTIAL §7.3.3.1]
 */
inline void parseRefPicListModification(BitReader& br, SliceHeader& sh) noexcept
{
    // ref_pic_list_modification for L0 (P, B, SP slices)
    if (sh.sliceType_ != SliceType::I && sh.sliceType_ != SliceType::SI)
    {
        sh.refPicListModificationL0_ = br.readBit() != 0U;
        if (sh.refPicListModificationL0_)
        {
            sh.numReorderCmdsL0_ = 0U;
            uint32_t op;
            do {
                op = br.readUev();
                uint32_t val = 0U;
                if (op == 0U || op == 1U)
                    val = br.readUev(); // abs_diff_pic_num_minus1
                else if (op == 2U)
                    val = br.readUev(); // long_term_pic_num
                if (sh.numReorderCmdsL0_ < SliceHeader::cMaxReorderCmds)
                {
                    sh.reorderCmdsL0_[sh.numReorderCmdsL0_].idc = static_cast<uint8_t>(op);
                    sh.reorderCmdsL0_[sh.numReorderCmdsL0_].value = val;
                    ++sh.numReorderCmdsL0_;
                }
            } while (op != 3U && !br.isExhausted());
        }
    }

    // ref_pic_list_modification for L1 (B slices)
    if (sh.sliceType_ == SliceType::B)
    {
        sh.refPicListModificationL1_ = br.readBit() != 0U;
        if (sh.refPicListModificationL1_)
        {
            sh.numReorderCmdsL1_ = 0U;
            uint32_t op;
            do {
                op = br.readUev();
                uint32_t val = 0U;
                if (op == 0U || op == 1U)
                    val = br.readUev();
                else if (op == 2U)
                    val = br.readUev();
                if (sh.numReorderCmdsL1_ < SliceHeader::cMaxReorderCmds)
                {
                    sh.reorderCmdsL1_[sh.numReorderCmdsL1_].idc = static_cast<uint8_t>(op);
                    sh.reorderCmdsL1_[sh.numReorderCmdsL1_].value = val;
                    ++sh.numReorderCmdsL1_;
                }
            } while (op != 3U && !br.isExhausted());
        }
    }
}

/** Parse dec_ref_pic_marking() — ITU-T H.264 §7.3.3.3.
 *  Stores MMCO commands for application by the DPB after decode.
 *
 *  IDR path: no_output_of_prior_pics_flag u(1), long_term_reference_flag u(1).
 *  Non-IDR: adaptive_ref_pic_marking_mode_flag u(1); if set, do-while MMCO
 *    ops 1-6 with associated parameters per Table 7-9. [CHECKED §7.3.3.3]
 *  MMCO parameter read order (FM-1 verified):
 *    op=1: difference_of_pic_nums_minus1 → value1
 *    op=2: long_term_pic_num → value1
 *    op=3: difference_of_pic_nums_minus1 → value1, long_term_frame_idx → value2
 *    op=4: max_long_term_frame_idx_plus1 → value1
 *    op=6: long_term_frame_idx → value2
 */
inline void parseDecRefPicMarking(BitReader& br, bool isIdr,
                                   DecRefPicMarking& marking) noexcept
{
    if (isIdr)
    {
        marking.noOutputOfPriorPics_ = br.readBit() != 0U;
        marking.longTermReference_ = br.readBit() != 0U;
    }
    else
    {
        marking.adaptiveRefPicMarking_ = br.readBit() != 0U;
        if (marking.adaptiveRefPicMarking_)
        {
            marking.numMmcoCommands_ = 0U;
            uint32_t op;
            do {
                op = br.readUev();
                MmcoCommand cmd;
                cmd.op = static_cast<uint8_t>(op);
                if (op == 1U || op == 3U)
                    cmd.value1 = br.readUev(); // difference_of_pic_nums_minus1
                if (op == 2U)
                    cmd.value1 = br.readUev(); // long_term_pic_num
                if (op == 3U || op == 6U)
                    cmd.value2 = br.readUev(); // long_term_frame_idx
                if (op == 4U)
                    cmd.value1 = br.readUev(); // max_long_term_frame_idx_plus1
                if (op != 0U && marking.numMmcoCommands_ < DecRefPicMarking::cMaxMmcoCommands)
                    marking.mmcoCommands_[marking.numMmcoCommands_++] = cmd;
            } while (op != 0U && !br.isExhausted());
        }
    }
}

/** Parse a slice header from RBSP data.
 *
 *  @param br     BitReader positioned at start of slice header RBSP
 *  @param sps    Active SPS (resolved via PPS)
 *  @param pps    Active PPS
 *  @param isIdr  True if this NAL is an IDR slice (type 5)
 *  @param nalRefIdc  nal_ref_idc from NAL header
 *  @param[out] sh  Parsed slice header
 *  @return Result::Ok on success
 */
inline Result parseSliceHeader(BitReader& br, const Sps& sps, const Pps& pps,
                               bool isIdr, uint8_t nalRefIdc,
                               SliceHeader& sh) noexcept
{
    sh = SliceHeader{};
    sh.isIdr_ = isIdr;

    // §7.3.3 slice_header() — field ordering verified. [CHECKED §7.3.3]
    // PARTIAL: colour_plane_id u(2) absent — only needed when
    //   separate_colour_plane_flag==1 (chroma_format_idc==3); we reject
    //   non-4:2:0 in SPS parsing so this flag is never set. [PARTIAL §7.3.3]
    // PARTIAL: slice_group_change_cycle u(v) absent — only needed when FMO
    //   active (num_slice_groups_minus1>0); we reject FMO in PPS parsing. [PARTIAL §7.3.3]

    // first_mb_in_slice ue(v)
    sh.firstMbInSlice_ = br.readUev();
    if (sh.firstMbInSlice_ >= sps.totalMbs())
        return Result::ErrorInvalidParam;

    // slice_type (0-9, values 5-9 mean "all slices in picture are this type")
    uint32_t sliceTypeRaw = br.readUev();
    if (sliceTypeRaw > 9U)
        return Result::ErrorInvalidParam;
    if (sliceTypeRaw >= 5U)
        sliceTypeRaw -= 5U;
    sh.sliceType_ = static_cast<SliceType>(sliceTypeRaw);

    // pic_parameter_set_id
    sh.ppsId_ = static_cast<uint8_t>(br.readUev());

    // frame_num
    sh.frameNum_ = static_cast<uint16_t>(br.readBits(sps.bitsInFrameNum_));

    // field_pic_flag (only if not frame_mbs_only)
    if (!sps.frameMbsOnly_)
    {
        sh.fieldPicFlag_ = br.readBit() != 0U;
        if (sh.fieldPicFlag_)
            sh.bottomFieldFlag_ = br.readBit() != 0U;
    }

    // idr_pic_id (only for IDR slices)
    if (isIdr)
    {
        sh.idrPicId_ = br.readUev();
        if (sh.idrPicId_ > 65535U)
            return Result::ErrorInvalidParam;
    }

    // Picture order count
    if (sps.picOrderCntType_ == 0U)
    {
        sh.picOrderCntLsb_ = static_cast<int32_t>(br.readBits(sps.log2MaxPicOrderCntLsb_));

        if (pps.picOrderPresent_ && !sh.fieldPicFlag_)
            sh.deltaPicOrderCntBottom_ = br.readSev();
    }

    if (sps.picOrderCntType_ == 1U && !sps.deltaPicOrderAlwaysZero_)
    {
        sh.deltaPicOrderCnt_[0] = br.readSev();
        if (pps.picOrderPresent_ && !sh.fieldPicFlag_)
            sh.deltaPicOrderCnt_[1] = br.readSev();
    }

    // redundant_pic_cnt
    if (pps.redundantPicCntPresent_)
        sh.redundantPicCnt_ = static_cast<uint8_t>(br.readUev());

    // direct_spatial_mv_pred_flag (B slices)
    if (sh.sliceType_ == SliceType::B)
        sh.directSpatialMvPred_ = br.readBit() != 0U;

    // num_ref_idx_active_override_flag
    sh.numRefIdxActiveL0_ = pps.numRefIdxL0Active_;
    sh.numRefIdxActiveL1_ = pps.numRefIdxL1Active_;

    if (sh.sliceType_ == SliceType::P || sh.sliceType_ == SliceType::SP ||
        sh.sliceType_ == SliceType::B)
    {
        bool overrideFlag = br.readBit() != 0U;
        if (overrideFlag)
        {
            sh.numRefIdxActiveL0_ = static_cast<uint8_t>(br.readUev() + 1U);
            if (sh.sliceType_ == SliceType::B)
                sh.numRefIdxActiveL1_ = static_cast<uint8_t>(br.readUev() + 1U);
        }
    }

    // ref_pic_list_modification — §7.3.3.1
    // Stores reordering commands for application during L0/L1 list construction.
    parseRefPicListModification(br, sh);

    // pred_weight_table() — §7.3.3.2
    // Present for P/SP slices with weighted_pred_flag=1, or B slices with
    // weighted_bipred_idc=1. Parses weight/offset values for weighted MC.
    if ((sh.sliceType_ == SliceType::P || sh.sliceType_ == SliceType::SP) &&
        pps.weightedPredFlag_)
    {
        parsePredWeightTable(br, sh, sps.chromaFormatIdc_);
    }
    else if (sh.sliceType_ == SliceType::B && pps.weightedBipredIdc_ == 1U)
    {
        parsePredWeightTable(br, sh, sps.chromaFormatIdc_);
    }

    // dec_ref_pic_marking — §7.3.3.3 (if nal_ref_idc != 0)
    // Stores MMCO commands for DPB application after decode.
    if (nalRefIdc != 0U)
        parseDecRefPicMarking(br, isIdr, sh.decRefPicMarking_);

    // cabac_init_idc (only for CABAC slices)
    if (pps.isCabac() && sh.sliceType_ != SliceType::I && sh.sliceType_ != SliceType::SI)
        sh.cabacInitIdc_ = static_cast<uint8_t>(br.readUev());

    // slice_qp_delta
    sh.sliceQpDelta_ = br.readSev();

    // SP/SI specific (skip for now)
    if (sh.sliceType_ == SliceType::SP || sh.sliceType_ == SliceType::SI)
    {
        if (sh.sliceType_ == SliceType::SP)
            br.readBit(); // sp_for_switch_flag
        br.readSev();     // slice_qs_delta
    }

    // deblocking_filter_control
    if (pps.deblockingFilterControlPresent_)
    {
        sh.disableDeblockingFilter_ = static_cast<uint8_t>(br.readUev());
        if (sh.disableDeblockingFilter_ != 1U)
        {
            sh.sliceAlphaC0Offset_ = br.readSev() * 2;
            sh.sliceBetaOffset_ = br.readSev() * 2;
        }
    }

    return Result::Ok;
}

} // namespace sub0h264

#endif // CROG_SUB0H264_SLICE_HPP
