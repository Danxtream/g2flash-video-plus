/** Scaling list resolution & application for H.264 High profile.
 *
 *  ITU-T H.264 §7.4.2.1.1.1 / §8.5.12.1: SPS and PPS may signal up to 6 4x4
 *  and 2 8x8 scaling lists per chroma_format_idc=1 stream. Each list is
 *  either signalled flat-16 (no effect), explicitly parsed, or set to the
 *  spec default via useDefaultScalingMatrixFlag.
 *
 *  This header provides:
 *    - Default_4x4_Intra/Inter and Default_8x8_Intra/Inter (Tables 7-3..7-5)
 *      pre-converted to RASTER order at compile time.
 *    - ResolvedScalingLists — slice-level resolved lists in raster order, with
 *      per-list `isFlat` flags so the dequant fast path is preserved when the
 *      stream signals only flat scaling.
 *    - resolveScalingLists(sps, pps) — applies §7.4.2.1.1 fall-back rules.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include "sps.hpp"
#include "pps.hpp"
#include "tables.hpp"  // cZigzag4x4 / cZigzag8x8

#include <array>
#include <cstdint>
#include <cstring>

namespace sub0h264
{

// ── Default scaling matrices (ITU-T H.264 Tables 7-3, 7-4, 7-5) ────────
// Tables specify ZIGZAG order; we store them pre-unscanned to RASTER order
// so dequant call sites can index linearly.

namespace detail
{

inline constexpr std::array<int16_t, 16> cDefault4x4IntraZigzag = {
    6, 13, 13, 20, 20, 20, 28, 28, 28, 28, 32, 32, 32, 37, 37, 42,
};

inline constexpr std::array<int16_t, 16> cDefault4x4InterZigzag = {
    10, 14, 14, 20, 20, 20, 24, 24, 24, 24, 27, 27, 27, 30, 30, 34,
};

// Table 7-4 Default_8x8_Intra (zigzag scan order).
inline constexpr std::array<int16_t, 64> cDefault8x8IntraZigzag = {
     6, 10, 10, 13, 11, 13, 16, 16, 16, 16, 18, 18, 18, 18, 18, 23,
    23, 23, 23, 23, 23, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27,
    27, 27, 27, 27, 29, 29, 29, 29, 29, 29, 29, 31, 31, 31, 31, 31,
    31, 33, 33, 33, 33, 33, 36, 36, 36, 36, 38, 38, 38, 40, 40, 42,
};

// Table 7-5 Default_8x8_Inter (zigzag scan order).
inline constexpr std::array<int16_t, 64> cDefault8x8InterZigzag = {
     9, 13, 13, 15, 13, 15, 17, 17, 17, 17, 19, 19, 19, 19, 19, 21,
    21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 24, 24, 24, 24,
    24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 27, 27, 27, 27, 27,
    27, 28, 28, 28, 28, 28, 30, 30, 30, 30, 32, 32, 32, 33, 33, 35,
};

template <std::size_t N>
constexpr std::array<int16_t, N> unscan(const std::array<int16_t, N>& zig,
                                        const std::array<uint8_t, N>& invScan)
{
    std::array<int16_t, N> out{};
    for (std::size_t k = 0; k < N; ++k)
        out[invScan[k]] = zig[k];
    return out;
}

}  // namespace detail

/// Default 4x4 intra scaling list in RASTER order.
inline constexpr auto cDefault4x4Intra = detail::unscan(
    detail::cDefault4x4IntraZigzag, cZigzag4x4);
/// Default 4x4 inter scaling list in RASTER order.
inline constexpr auto cDefault4x4Inter = detail::unscan(
    detail::cDefault4x4InterZigzag, cZigzag4x4);
/// Default 8x8 intra scaling list in RASTER order.
inline constexpr auto cDefault8x8Intra = detail::unscan(
    detail::cDefault8x8IntraZigzag, cZigzag8x8);
/// Default 8x8 inter scaling list in RASTER order.
inline constexpr auto cDefault8x8Inter = detail::unscan(
    detail::cDefault8x8InterZigzag, cZigzag8x8);

/** Slice-level resolved scaling lists (§7.4.2.1.1).
 *
 *  Lists are stored in RASTER order so dequant call sites index linearly.
 *  `isFlat4x4[i]` / `isFlat8x8[i]` tell the dispatcher whether to take the
 *  scaled-dequant path (when false) or the existing flat-16 fast path
 *  (when true). The latter avoids per-coefficient multiplication overhead
 *  when the stream signals no scaling.
 */
struct ResolvedScalingLists
{
    /// Six 4x4 lists in raster order.
    /// Index: 0=Y intra, 1=Cb intra, 2=Cr intra, 3=Y inter, 4=Cb inter, 5=Cr inter.
    int16_t list4x4[6][16];
    bool isFlat4x4[6];
    /// Two 8x8 lists in raster order.
    /// Index: 0=Y intra, 1=Y inter.
    int16_t list8x8[2][64];
    bool isFlat8x8[2];
    /// True if every list is flat (fast-path entire slice).
    bool allFlat;
};

namespace detail
{

inline bool _isFlat(const int16_t* list, uint32_t n)
{
    for (uint32_t i = 0U; i < n; ++i)
        if (list[i] != 16) return false;
    return true;
}

inline void _setFlat(int16_t* dst, uint32_t n)
{
    for (uint32_t i = 0U; i < n; ++i) dst[i] = 16;
}

inline void _unscan4x4(const int16_t* zigSrc, int16_t* rasterDst)
{
    for (uint32_t k = 0U; k < 16U; ++k)
        rasterDst[cZigzag4x4[k]] = zigSrc[k];
}

inline void _unscan8x8(const int16_t* zigSrc, int16_t* rasterDst)
{
    for (uint32_t k = 0U; k < 64U; ++k)
        rasterDst[cZigzag8x8[k]] = zigSrc[k];
}

}  // namespace detail

/** Resolve scaling lists per §7.4.2.1.1.1 + Table 7-2.
 *
 *  Spec resolution rules per index `i`:
 *
 *    * SPS pass (§7.4.2.1.1.1, Table 7-2 Set B): for each i, if
 *      `seq_scaling_list_present_flag[i]`==0 (list NOT signalled) — use
 *      Default_*_Intra/Inter for "first-of-type" indices (0, 3, 6, 7) or
 *      chain to the predecessor (1←0, 2←1, 4←3, 5←4). If signalled and
 *      `useDefaultScalingMatrixFlag` was true (delta sequence converged to
 *      zero on the first iteration), use the spec default. Otherwise use
 *      the parsed list.
 *
 *    * PPS pass (§7.4.2.1.1.1, Table 7-2 Set A): if PPS scaling matrix is
 *      present, repeat the same rule but the "fall-back" for an unsignalled
 *      list is the corresponding SPS-resolved list (so PPS overrides SPS
 *      only where it explicitly signals a different list).
 *
 *    * If neither SPS nor PPS signals a scaling matrix at all: every list
 *      is flat-16 (no behavioural change vs the dequant fast path).
 *
 *  All lists are returned in RASTER order so dequant can index linearly.
 *  isFlat4x4[i]/isFlat8x8[i] gate the fast/slow dispatch per call site.
 */
inline ResolvedScalingLists resolveScalingLists(const Sps& sps, const Pps& pps) noexcept
{
    ResolvedScalingLists r{};
    const bool anyMatrix = sps.seqScalingMatrixPresent_ || pps.picScalingMatrixPresent_;

    if (!anyMatrix)
    {
        for (uint32_t i = 0U; i < 6U; ++i)
        {
            detail::_setFlat(r.list4x4[i], 16U);
            r.isFlat4x4[i] = true;
        }
        for (uint32_t i = 0U; i < 2U; ++i)
        {
            detail::_setFlat(r.list8x8[i], 64U);
            r.isFlat8x8[i] = true;
        }
        r.allFlat = true;
        return r;
    }

    auto defaultFor4x4 = [](uint32_t i) -> const int16_t* {
        return (i < 3U) ? cDefault4x4Intra.data() : cDefault4x4Inter.data();
    };
    auto defaultFor8x8 = [](uint32_t i) -> const int16_t* {
        return (i == 0U) ? cDefault8x8Intra.data() : cDefault8x8Inter.data();
    };
    // First-of-type indices for the chain rule: list 0 (Y intra),
    // list 3 (Y inter) for 4x4; list 0 and 1 for 8x8 (no chaining there).
    auto isFirstOfType4x4 = [](uint32_t i) { return (i == 0U) || (i == 3U); };

    // ── SPS resolution (lists that the PPS will consult as fall-back) ──
    int16_t spsList4x4[6][16];
    bool    spsList4x4Flat[6];
    int16_t spsList8x8[2][64];
    bool    spsList8x8Flat[2];

    if (sps.seqScalingMatrixPresent_)
    {
        for (uint32_t i = 0U; i < 6U; ++i)
        {
            if (!sps.scalingList4x4_[i].present_)
            {
                if (isFirstOfType4x4(i))
                    std::memcpy(spsList4x4[i], defaultFor4x4(i), 16U * sizeof(int16_t));
                else
                    std::memcpy(spsList4x4[i], spsList4x4[i - 1U], 16U * sizeof(int16_t));
            }
            else if (sps.scalingList4x4_[i].useDefault_)
                std::memcpy(spsList4x4[i], defaultFor4x4(i), 16U * sizeof(int16_t));
            else
                detail::_unscan4x4(sps.scalingList4x4_[i].data_, spsList4x4[i]);
            spsList4x4Flat[i] = detail::_isFlat(spsList4x4[i], 16U);
        }
        for (uint32_t i = 0U; i < 2U; ++i)
        {
            if (!sps.scalingList8x8_[i].present_)
                std::memcpy(spsList8x8[i], defaultFor8x8(i), 64U * sizeof(int16_t));
            else if (sps.scalingList8x8_[i].useDefault_)
                std::memcpy(spsList8x8[i], defaultFor8x8(i), 64U * sizeof(int16_t));
            else
                detail::_unscan8x8(sps.scalingList8x8_[i].data_, spsList8x8[i]);
            spsList8x8Flat[i] = detail::_isFlat(spsList8x8[i], 64U);
        }
    }
    else
    {
        // No SPS matrix → SPS-resolved view is flat-16; PPS Table 7-2 Set A
        // chain rule still uses defaults for first-of-type indices.
        for (uint32_t i = 0U; i < 6U; ++i)
        {
            detail::_setFlat(spsList4x4[i], 16U);
            spsList4x4Flat[i] = true;
        }
        for (uint32_t i = 0U; i < 2U; ++i)
        {
            detail::_setFlat(spsList8x8[i], 64U);
            spsList8x8Flat[i] = true;
        }
    }

    // ── PPS resolution (final lists used by dequant) ──
    if (pps.picScalingMatrixPresent_)
    {
        for (uint32_t i = 0U; i < 6U; ++i)
        {
            if (!pps.scalingList4x4_[i].present_)
            {
                // Table 7-2 Set A: fall back to default-of-type for first
                // index of type, else chain to predecessor (PPS-resolved).
                // When SPS matrix is present, fall back to SPS-resolved
                // list instead (Table 7-2 Set C).
                if (sps.seqScalingMatrixPresent_)
                    std::memcpy(r.list4x4[i], spsList4x4[i], 16U * sizeof(int16_t));
                else if (isFirstOfType4x4(i))
                    std::memcpy(r.list4x4[i], defaultFor4x4(i), 16U * sizeof(int16_t));
                else
                    std::memcpy(r.list4x4[i], r.list4x4[i - 1U], 16U * sizeof(int16_t));
            }
            else if (pps.scalingList4x4_[i].useDefault_)
                std::memcpy(r.list4x4[i], defaultFor4x4(i), 16U * sizeof(int16_t));
            else
                detail::_unscan4x4(pps.scalingList4x4_[i].data_, r.list4x4[i]);
            r.isFlat4x4[i] = detail::_isFlat(r.list4x4[i], 16U);
        }
        for (uint32_t i = 0U; i < 2U; ++i)
        {
            if (!pps.scalingList8x8_[i].present_)
            {
                if (sps.seqScalingMatrixPresent_)
                    std::memcpy(r.list8x8[i], spsList8x8[i], 64U * sizeof(int16_t));
                else
                    std::memcpy(r.list8x8[i], defaultFor8x8(i), 64U * sizeof(int16_t));
            }
            else if (pps.scalingList8x8_[i].useDefault_)
                std::memcpy(r.list8x8[i], defaultFor8x8(i), 64U * sizeof(int16_t));
            else
                detail::_unscan8x8(pps.scalingList8x8_[i].data_, r.list8x8[i]);
            r.isFlat8x8[i] = detail::_isFlat(r.list8x8[i], 64U);
        }
    }
    else
    {
        // PPS doesn't override — copy SPS-resolved view through.
        for (uint32_t i = 0U; i < 6U; ++i)
        {
            std::memcpy(r.list4x4[i], spsList4x4[i], 16U * sizeof(int16_t));
            r.isFlat4x4[i] = spsList4x4Flat[i];
        }
        for (uint32_t i = 0U; i < 2U; ++i)
        {
            std::memcpy(r.list8x8[i], spsList8x8[i], 64U * sizeof(int16_t));
            r.isFlat8x8[i] = spsList8x8Flat[i];
        }
    }

    r.allFlat = true;
    for (uint32_t i = 0U; i < 6U; ++i) r.allFlat &= r.isFlat4x4[i];
    for (uint32_t i = 0U; i < 2U; ++i) r.allFlat &= r.isFlat8x8[i];
    return r;
}

/// 4x4 scaling list selector — block type → list index 0..5.
/// Index: 0=Y intra, 1=Cb intra, 2=Cr intra, 3=Y inter, 4=Cb inter, 5=Cr inter.
enum class Block4x4Plane : uint8_t { LumaIntra = 0, CbIntra = 1, CrIntra = 2,
                                     LumaInter = 3, CbInter = 4, CrInter = 5 };

inline const int16_t* select4x4(const ResolvedScalingLists& sl, Block4x4Plane p) noexcept
{
    return sl.list4x4[static_cast<uint8_t>(p)];
}

inline bool isFlat4x4(const ResolvedScalingLists& sl, Block4x4Plane p) noexcept
{
    return sl.isFlat4x4[static_cast<uint8_t>(p)];
}

}  // namespace sub0h264
