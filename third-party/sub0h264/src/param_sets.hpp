/** Sub0h264 — Parameter set storage
 *
 *  Stores SPS and PPS tables indexed by ID, with active-set tracking.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_PARAM_SETS_HPP
#define CROG_SUB0H264_PARAM_SETS_HPP

#include "sps.hpp"
#include "pps.hpp"

namespace sub0h264 {

/** Parameter set storage — holds all decoded SPS and PPS. */
class ParamSets
{
public:
    /** Store/update an SPS by ID. */
    Result storeSps(const Sps& sps) noexcept
    {
        if (sps.spsId_ >= cMaxSpsCount)
            return Result::ErrorInvalidParam;
        sps_[sps.spsId_] = sps;
        return Result::Ok;
    }

    /** Store/update a PPS by ID. */
    Result storePps(const Pps& pps) noexcept
    {
        if (pps.ppsId_ >= cMaxPpsCount)
            return Result::ErrorInvalidParam;
        pps_[pps.ppsId_] = pps;
        return Result::Ok;
    }

    /** Get SPS by ID (nullptr if not valid). */
    const Sps* getSps(uint8_t id) const noexcept
    {
        if (id >= cMaxSpsCount || !sps_[id].valid_)
            return nullptr;
        return &sps_[id];
    }

    /** Get PPS by ID (nullptr if not valid). */
    const Pps* getPps(uint8_t id) const noexcept
    {
        if (id >= cMaxPpsCount || !pps_[id].valid_)
            return nullptr;
        return &pps_[id];
    }

    /** Get mutable SPS array (for parsePps which needs spsArray). */
    const Sps* spsArray() const noexcept { return sps_; }

private:
    Sps sps_[cMaxSpsCount]{};
    Pps pps_[cMaxPpsCount]{};
};

} // namespace sub0h264

#endif // CROG_SUB0H264_PARAM_SETS_HPP
