/** Sub0h264 — Picture Parameter Set (PPS) parser
 *
 *  Parses H.264 PPS NAL units. Reference: ITU-T H.264 §7.3.2.2
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_PPS_HPP
#define CROG_SUB0H264_PPS_HPP

#include "bitstream.hpp"
#include "sps.hpp"
#include "sub0h264/sub0h264_types.hpp"

#include <cstdint>
#include <cstring>

namespace sub0h264 {

/** Maximum supported PPS count (spec allows 0-255). */
inline constexpr uint32_t cMaxPpsCount = 256U;

/** Parsed Picture Parameter Set. */
struct Pps
{
    bool valid_ = false;

    // Identity
    uint8_t ppsId_ = 0U;
    uint8_t spsId_ = 0U;      ///< References an SPS by ID

    // Entropy coding
    uint8_t entropyCodingMode_ = 0U;   ///< 0=CAVLC, 1=CABAC
    bool isCabac() const noexcept { return entropyCodingMode_ == 1U; }

    // Slice groups (only 1 supported)
    uint8_t numSliceGroups_ = 1U;

    // Reference index defaults
    uint8_t numRefIdxL0Active_ = 1U;   ///< num_ref_idx_l0_default_active_minus1 + 1
    uint8_t numRefIdxL1Active_ = 1U;   ///< num_ref_idx_l1_default_active_minus1 + 1

    // Prediction flags
    uint8_t weightedPredFlag_ = 0U;
    uint8_t weightedBipredIdc_ = 0U;   ///< 0=default, 1=explicit, 2=implicit

    // QP
    int8_t picInitQp_ = static_cast<int8_t>(cDefaultPicInitQp);  ///< pic_init_qp_minus26 + 26
    int8_t picInitQs_ = static_cast<int8_t>(cDefaultPicInitQp);
    int8_t chromaQpIndexOffset_ = 0;
    int8_t secondChromaQpIndexOffset_ = 0;

    // Flags
    uint8_t deblockingFilterControlPresent_ = 0U;
    uint8_t constrainedIntraPred_ = 0U;
    uint8_t redundantPicCntPresent_ = 0U;
    uint8_t picOrderPresent_ = 0U;

    // High profile
    uint8_t transform8x8Mode_ = 0U;
    bool picScalingMatrixPresent_ = false;
    ScalingList4x4 scalingList4x4_[6]{};
    ScalingList8x8 scalingList8x8_[2]{};
};

/** Parse a PPS NAL unit.
 *
 *  @param br    BitReader positioned at the first byte of RBSP (after NAL header)
 *  @param spsArray  Array of parsed SPS (indexed by spsId)
 *  @param[out] pps   Parsed PPS
 *  @return Result::Ok on success
 */
inline Result parsePps(BitReader& br, const Sps* spsArray, Pps& pps) noexcept
{
    pps = Pps{};

    // §7.3.2.2 pic_parameter_set_rbsp() — field ordering verified [CHECKED §7.3.2.2]

    // pic_parameter_set_id ue(v)
    pps.ppsId_ = static_cast<uint8_t>(br.readUev());
    if (pps.ppsId_ >= cMaxPpsCount)
        return Result::ErrorInvalidParam;

    // seq_parameter_set_id
    pps.spsId_ = static_cast<uint8_t>(br.readUev());
    if (pps.spsId_ >= cMaxSpsCount)
        return Result::ErrorInvalidParam;

    // Validate referenced SPS exists
    if (!spsArray[pps.spsId_].valid_)
        return Result::ErrorInvalidParam;

    const Sps& sps = spsArray[pps.spsId_];

    // entropy_coding_mode_flag: 0=CAVLC, 1=CABAC
    pps.entropyCodingMode_ = static_cast<uint8_t>(br.readBit());

    // bottom_field_pic_order_in_frame_present_flag
    pps.picOrderPresent_ = static_cast<uint8_t>(br.readBit());

    // num_slice_groups_minus1
    pps.numSliceGroups_ = static_cast<uint8_t>(br.readUev() + 1U);
    if (pps.numSliceGroups_ != 1U)
        return Result::ErrorUnsupported; // FMO not supported

    // num_ref_idx_l0_default_active_minus1
    pps.numRefIdxL0Active_ = static_cast<uint8_t>(br.readUev() + 1U);
    if (pps.numRefIdxL0Active_ > 16U)
        return Result::ErrorInvalidParam;

    // num_ref_idx_l1_default_active_minus1
    pps.numRefIdxL1Active_ = static_cast<uint8_t>(br.readUev() + 1U);
    if (pps.numRefIdxL1Active_ > 16U)
        return Result::ErrorInvalidParam;

    // weighted_pred_flag
    pps.weightedPredFlag_ = static_cast<uint8_t>(br.readBit());

    // weighted_bipred_idc
    pps.weightedBipredIdc_ = static_cast<uint8_t>(br.readBits(2U));
    if (pps.weightedBipredIdc_ > 2U)
        return Result::ErrorInvalidParam;

    // pic_init_qp_minus26
    pps.picInitQp_ = static_cast<int8_t>(br.readSev() + cDefaultPicInitQp);
    if (pps.picInitQp_ < 0 || pps.picInitQp_ > 51)
        return Result::ErrorInvalidParam;

    // pic_init_qs_minus26
    pps.picInitQs_ = static_cast<int8_t>(br.readSev() + cDefaultPicInitQp);

    // chroma_qp_index_offset
    pps.chromaQpIndexOffset_ = static_cast<int8_t>(br.readSev());
    if (pps.chromaQpIndexOffset_ < -12 || pps.chromaQpIndexOffset_ > 12)
        return Result::ErrorInvalidParam;

    // deblocking_filter_control_present_flag
    pps.deblockingFilterControlPresent_ = static_cast<uint8_t>(br.readBit());

    // constrained_intra_pred_flag
    pps.constrainedIntraPred_ = static_cast<uint8_t>(br.readBit());

    // redundant_pic_cnt_present_flag
    pps.redundantPicCntPresent_ = static_cast<uint8_t>(br.readBit());

    // §7.3.2.2: if( more_rbsp_data() ) — High profile extension block.
    // Spec uses more_rbsp_data() (profile-agnostic). We approximate with
    // profile==High && hasBits(1). Functionally correct for Baseline/Main/High:
    // Baseline/Main streams don't include this block in practice. [PARTIAL §7.3.2.2]
    if (sps.profileIdc_ == cProfileHigh && br.hasBits(1U))
    {
        pps.transform8x8Mode_ = static_cast<uint8_t>(br.readBit());

        pps.picScalingMatrixPresent_ = br.readBit() != 0U;
        if (pps.picScalingMatrixPresent_)
        {
            uint32_t numLists = 6U + (pps.transform8x8Mode_ ? 2U : 0U);
            for (uint32_t i = 0U; i < numLists; ++i)
            {
                bool present = br.readBit() != 0U;
                if (present)
                {
                    if (i < 6U)
                    {
                        pps.scalingList4x4_[i].present_ = true;
                        parseScalingList(br, pps.scalingList4x4_[i].data_, 16U,
                                         pps.scalingList4x4_[i].useDefault_);
                    }
                    else
                    {
                        pps.scalingList8x8_[i - 6U].present_ = true;
                        parseScalingList(br, pps.scalingList8x8_[i - 6U].data_, 64U,
                                         pps.scalingList8x8_[i - 6U].useDefault_);
                    }
                }
            }
        }

        // second_chroma_qp_index_offset
        pps.secondChromaQpIndexOffset_ = static_cast<int8_t>(br.readSev());
    }
    else
    {
        // §7.4.2.2: When second_chroma_qp_index_offset absent, infer equal to
        // chroma_qp_index_offset. [CHECKED §7.4.2.2]
        pps.secondChromaQpIndexOffset_ = pps.chromaQpIndexOffset_;
    }

    pps.valid_ = true;
    return Result::Ok;
}

} // namespace sub0h264

#endif // CROG_SUB0H264_PPS_HPP
