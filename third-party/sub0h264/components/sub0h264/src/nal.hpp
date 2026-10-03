/** Sub0h264 — NAL unit types and structures
 *
 *  H.264 NAL (Network Abstraction Layer) unit definitions.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_NAL_HPP
#define CROG_SUB0H264_NAL_HPP

#include <cstdint>
#include <vector>

namespace sub0h264 {

/** H.264 NAL unit types (nal_unit_type field, 5 bits). */
enum class NalType : uint8_t
{
    Unspecified        = 0,
    SliceNonIdr        = 1,   ///< Non-IDR slice (P, B, or non-IDR I)
    SliceDataPartA     = 2,
    SliceDataPartB     = 3,
    SliceDataPartC     = 4,
    SliceIdr           = 5,   ///< IDR slice (key frame)
    Sei                = 6,   ///< Supplemental Enhancement Information
    Sps                = 7,   ///< Sequence Parameter Set
    Pps                = 8,   ///< Picture Parameter Set
    Aud                = 9,   ///< Access Unit Delimiter
    EndOfSequence      = 10,
    EndOfStream        = 11,
    FillerData         = 12,
    SpsExtension       = 13,
    PrefixNal          = 14,
    SubsetSps          = 15,
};

/** Parsed NAL unit header + RBSP data. */
struct NalUnit
{
    NalType  type    = NalType::Unspecified;
    uint8_t  refIdc  = 0U;   ///< nal_ref_idc (2 bits): reference priority
    std::vector<uint8_t> rbspData;  ///< RBSP bytes (emulation prevention removed)
};

/** Check if a NAL type is a slice (contains macroblock data). */
inline bool isSliceNal(NalType type) noexcept
{
    return type == NalType::SliceNonIdr
        || type == NalType::SliceIdr
        || type == NalType::SliceDataPartA;
}

/** Check if a NAL type is a parameter set. */
inline bool isParamSetNal(NalType type) noexcept
{
    return type == NalType::Sps || type == NalType::Pps;
}

} // namespace sub0h264

#endif // CROG_SUB0H264_NAL_HPP
