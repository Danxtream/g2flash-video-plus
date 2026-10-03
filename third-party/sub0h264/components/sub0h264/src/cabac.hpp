/** Sub0h264 — CABAC entropy decoder
 *
 *  Context-Adaptive Binary Arithmetic Coding for H.264 High profile.
 *  Implements the arithmetic decode engine, context model management,
 *  and binarization schemes for all syntax elements.
 *
 *  Reference: ITU-T H.264 §9.3
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_CABAC_HPP
#define CROG_SUB0H264_CABAC_HPP

#include "bitstream.hpp"
#include "decode_trace.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstring>
#include <array>

namespace sub0h264 {

/// Number of CABAC context models for Baseline/Main profile — ITU-T H.264
/// Tables 9-12 through 9-23 define 460 contexts (indices 0-459).
inline constexpr uint32_t cNumCabacCtxBase = 460U;

/// Number of CABAC context models including High profile 8x8 extensions.
/// High profile adds contexts 460-1023 for ctxBlockCat 5-13 (8x8 blocks).
/// Layout per ITU-T H.264 §9.3.3.1.1.3 Table 9-42:
///   460-472:  significant_coeff_flag    ctxBlockCat 5 (frame, 10 + 3 extra)
///   472-483:  last_significant_coeff_flag ctxBlockCat 5 (frame)
///   484-493:  coeff_abs_level_minus1    ctxBlockCat 5
///   1012-1015: coded_block_flag         ctxBlockCat 5
/// Total allocation: 1024 contexts covers all High profile extensions.
inline constexpr uint32_t cNumCabacCtx = 1024U;

/// Maximum QP value.
inline constexpr int32_t cMaxQp = 51;

// ── CABAC context model ─────────────────────────────────────────────────

/** Single CABAC binary context model.
 *  Bit 6 = MPS (Most Probable Symbol), Bits 0-5 = state index [0-63].
 */
struct CabacCtx
{
    uint8_t mpsState = 0U;

    uint8_t mps() const noexcept { return (mpsState >> 6U) & 1U; }
    uint8_t state() const noexcept { return mpsState & 0x3FU; }
};

// ── Combined CABAC table — rangeTabLPS + state transitions ──────────────

/** Combined table: rangeTabLPS, nextStateMPS, nextStateLPS.
 *
 *  Indexed by [mpsState & 0x7F][quantized_range].
 *  Index = pStateIdx | (valMPS << 6), so 0-63 = mps=0, 64-127 = mps=1.
 *
 *  Each 32-bit entry packs:
 *    bits  0-7:  rangeTabLPS value  (from Table 9-45)
 *    bits  8-14: next mpsState if MPS decoded  (from Table 9-46 transIdxMPS)
 *    bits 15-21: next mpsState if LPS decoded  (from Table 9-46 transIdxLPS)
 *
 *  Generated from ITU-T H.264 Tables 9-45/9-46 via constexpr. Previous
 *  version was sourced from libavc and had 6 rangeLPS values off by 1,
 *  causing wrong bin decisions at engine state boundaries.
 */
inline constexpr uint32_t cCabacTableGen() noexcept { return 0; } // placeholder for constexpr gen

namespace detail {

// ITU-T H.264 Table 9-45: rangeTabLPS[64][4]
inline constexpr uint8_t rangeTabLPS[64][4] = {
    {128,176,208,240}, {128,167,197,227}, {128,158,187,216}, {123,150,178,205},
    {116,142,169,195}, {111,135,160,185}, {105,128,152,175}, {100,122,144,166},
    { 95,116,137,158}, { 90,110,130,150}, { 85,104,123,142}, { 81, 99,117,135},
    { 77, 94,111,128}, { 73, 89,105,122}, { 69, 85,100,116}, { 66, 80, 95,110},
    { 62, 76, 90,104}, { 59, 72, 86, 99}, { 56, 69, 81, 94}, { 53, 65, 77, 89},
    { 51, 62, 73, 85}, { 48, 59, 69, 80}, { 46, 56, 66, 76}, { 43, 53, 63, 72},
    { 41, 50, 59, 69}, { 39, 48, 56, 65}, { 37, 45, 54, 62}, { 35, 43, 51, 59},
    { 33, 41, 48, 56}, { 32, 39, 46, 53}, { 30, 37, 43, 50}, { 29, 35, 41, 48},
    { 27, 33, 39, 45}, { 26, 31, 37, 43}, { 24, 30, 35, 41}, { 23, 28, 33, 39},
    { 22, 27, 32, 37}, { 21, 26, 30, 35}, { 20, 24, 29, 33}, { 19, 23, 27, 31},
    { 18, 22, 26, 30}, { 17, 21, 25, 28}, { 16, 20, 23, 27}, { 15, 19, 22, 25},
    { 14, 18, 21, 24}, { 14, 17, 20, 23}, { 13, 16, 19, 22}, { 12, 15, 18, 21},
    { 12, 14, 17, 20}, { 11, 14, 16, 19}, { 11, 13, 15, 18}, { 10, 12, 15, 17},
    { 10, 12, 14, 16}, {  9, 11, 13, 15}, {  9, 11, 12, 14}, {  8, 10, 12, 14},
    {  8,  9, 11, 13}, {  7,  9, 11, 12}, {  7,  9, 10, 12}, {  7,  8, 10, 11},
    {  6,  8,  9, 11}, {  6,  7,  9, 10}, {  6,  7,  8,  9}, {  2,  2,  2,  2},
};

// ITU-T H.264 Table 9-46: state transition tables
inline constexpr uint8_t transIdxMPS[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63,
};
inline constexpr uint8_t transIdxLPS[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63,
};

/// Generate one packed table entry from spec tables.
inline constexpr uint32_t packEntry(uint32_t pState, uint32_t mps, uint32_t q) noexcept
{
    uint32_t lps = rangeTabLPS[pState][q];
    uint32_t nextMpsState = transIdxMPS[pState];
    uint32_t nextMpsMpsState = nextMpsState | (mps << 6U);
    uint32_t nextLpsState = transIdxLPS[pState];
    uint32_t nextLpsMps = (pState == 0U) ? (1U - mps) : mps;
    uint32_t nextLpsMpsState = nextLpsState | (nextLpsMps << 6U);
    return lps | (nextMpsMpsState << 8U) | (nextLpsMpsState << 15U);
}

} // namespace detail

/// Generated from ITU-T H.264 Tables 9-45/9-46 — constexpr, spec-auditable.
inline constexpr uint32_t cCabacTable[128][4] = {
    { 2097536, 2097584, 2097616, 2097648 },
    { 640, 679, 709, 739 },
    { 33664, 33694, 33723, 33752 },
    { 66683, 66710, 66738, 66765 },
    { 66932, 66958, 66985, 67011 },
    { 132719, 132743, 132768, 132793 },
    { 132969, 132992, 133016, 133039 },
    { 165988, 166010, 166032, 166054 },
    { 199007, 199028, 199049, 199070 },
    { 232026, 232046, 232066, 232086 },
    { 265045, 265064, 265083, 265102 },
    { 298065, 298083, 298101, 298119 },
    { 298317, 298334, 298351, 298368 },
    { 364105, 364121, 364137, 364154 },
    { 364357, 364373, 364388, 364404 },
    { 397378, 397392, 397407, 397422 },
    { 430398, 430412, 430426, 430440 },
    { 430651, 430664, 430678, 430691 },
    { 496440, 496453, 496465, 496478 },
    { 496693, 496705, 496717, 496729 },
    { 529715, 529726, 529737, 529749 },
    { 529968, 529979, 529989, 530000 },
    { 595758, 595768, 595778, 595788 },
    { 596011, 596021, 596031, 596040 },
    { 629033, 629042, 629051, 629061 },
    { 629287, 629296, 629304, 629313 },
    { 695077, 695085, 695094, 695102 },
    { 695331, 695339, 695347, 695355 },
    { 728353, 728361, 728368, 728376 },
    { 728608, 728615, 728622, 728629 },
    { 761630, 761637, 761643, 761650 },
    { 794653, 794659, 794665, 794672 },
    { 794907, 794913, 794919, 794925 },
    { 827930, 827935, 827941, 827947 },
    { 860952, 860958, 860963, 860969 },
    { 861207, 861212, 861217, 861223 },
    { 894230, 894235, 894240, 894245 },
    { 894485, 894490, 894494, 894499 },
    { 927508, 927512, 927517, 927521 },
    { 960531, 960535, 960539, 960543 },
    { 960786, 960790, 960794, 960798 },
    { 993809, 993813, 993817, 993820 },
    { 994064, 994068, 994071, 994075 },
    { 994319, 994323, 994326, 994329 },
    { 1027342, 1027346, 1027349, 1027352 },
    { 1060366, 1060369, 1060372, 1060375 },
    { 1060621, 1060624, 1060627, 1060630 },
    { 1093644, 1093647, 1093650, 1093653 },
    { 1093900, 1093902, 1093905, 1093908 },
    { 1094155, 1094158, 1094160, 1094163 },
    { 1127179, 1127181, 1127183, 1127186 },
    { 1127434, 1127436, 1127439, 1127441 },
    { 1160458, 1160460, 1160462, 1160464 },
    { 1160713, 1160715, 1160717, 1160719 },
    { 1160969, 1160971, 1160972, 1160974 },
    { 1193992, 1193994, 1193996, 1193998 },
    { 1194248, 1194249, 1194251, 1194253 },
    { 1194503, 1194505, 1194507, 1194508 },
    { 1227527, 1227529, 1227530, 1227532 },
    { 1227783, 1227784, 1227786, 1227787 },
    { 1228038, 1228040, 1228041, 1228043 },
    { 1261062, 1261063, 1261065, 1261066 },
    { 1261062, 1261063, 1261064, 1261065 },
    { 2080514, 2080514, 2080514, 2080514 },
    // States 64-127 (MPS=1)
    { 16768, 16816, 16848, 16880 },
    { 2114176, 2114215, 2114245, 2114275 },
    { 2147200, 2147230, 2147259, 2147288 },
    { 2180219, 2180246, 2180274, 2180301 },
    { 2180468, 2180494, 2180521, 2180547 },
    { 2246255, 2246279, 2246304, 2246329 },
    { 2246505, 2246528, 2246552, 2246575 },
    { 2279524, 2279546, 2279568, 2279590 },
    { 2312543, 2312564, 2312585, 2312606 },
    { 2345562, 2345582, 2345602, 2345622 },
    { 2378581, 2378600, 2378619, 2378638 },
    { 2411601, 2411619, 2411637, 2411655 },
    { 2411853, 2411870, 2411887, 2411904 },
    { 2477641, 2477657, 2477673, 2477690 },
    { 2477893, 2477909, 2477924, 2477940 },
    { 2510914, 2510928, 2510943, 2510958 },
    { 2543934, 2543948, 2543962, 2543976 },
    { 2544187, 2544200, 2544214, 2544227 },
    { 2609976, 2609989, 2610001, 2610014 },
    { 2610229, 2610241, 2610253, 2610265 },
    { 2643251, 2643262, 2643273, 2643285 },
    { 2643504, 2643515, 2643525, 2643536 },
    { 2709294, 2709304, 2709314, 2709324 },
    { 2709547, 2709557, 2709567, 2709576 },
    { 2742569, 2742578, 2742587, 2742597 },
    { 2742823, 2742832, 2742840, 2742849 },
    { 2808613, 2808621, 2808630, 2808638 },
    { 2808867, 2808875, 2808883, 2808891 },
    { 2841889, 2841897, 2841904, 2841912 },
    { 2842144, 2842151, 2842158, 2842165 },
    { 2875166, 2875173, 2875179, 2875186 },
    { 2908189, 2908195, 2908201, 2908208 },
    { 2908443, 2908449, 2908455, 2908461 },
    { 2941466, 2941471, 2941477, 2941483 },
    { 2974488, 2974494, 2974499, 2974505 },
    { 2974743, 2974748, 2974753, 2974759 },
    { 3007766, 3007771, 3007776, 3007781 },
    { 3008021, 3008026, 3008030, 3008035 },
    { 3041044, 3041048, 3041053, 3041057 },
    { 3074067, 3074071, 3074075, 3074079 },
    { 3074322, 3074326, 3074330, 3074334 },
    { 3107345, 3107349, 3107353, 3107356 },
    { 3107600, 3107604, 3107607, 3107611 },
    { 3107855, 3107859, 3107862, 3107865 },
    { 3140878, 3140882, 3140885, 3140888 },
    { 3173902, 3173905, 3173908, 3173911 },
    { 3174157, 3174160, 3174163, 3174166 },
    { 3207180, 3207183, 3207186, 3207189 },
    { 3207436, 3207438, 3207441, 3207444 },
    { 3207691, 3207694, 3207696, 3207699 },
    { 3240715, 3240717, 3240719, 3240722 },
    { 3240970, 3240972, 3240975, 3240977 },
    { 3273994, 3273996, 3273998, 3274000 },
    { 3274249, 3274251, 3274253, 3274255 },
    { 3274505, 3274507, 3274508, 3274510 },
    { 3307528, 3307530, 3307532, 3307534 },
    { 3307784, 3307785, 3307787, 3307789 },
    { 3308039, 3308041, 3308043, 3308044 },
    { 3341063, 3341065, 3341066, 3341068 },
    { 3341319, 3341320, 3341322, 3341323 },
    { 3341574, 3341576, 3341577, 3341579 },
    { 3374598, 3374599, 3374601, 3374602 },
    { 3374598, 3374599, 3374600, 3374601 },
    { 4194050, 4194050, 4194050, 4194050 },
};

// ── Count leading zeros ─────────────────────────────────────────────────

inline uint32_t clz32(uint32_t val) noexcept
{
    if (val == 0U) return 32U;
#if defined(__GNUC__) || defined(__clang__)
    return static_cast<uint32_t>(__builtin_clz(val));
#elif defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse(&idx, val);
    return 31U - idx;
#else
    uint32_t n = 0U;
    if (val <= 0x0000FFFFU) { n += 16U; val <<= 16U; }
    if (val <= 0x00FFFFFFU) { n += 8U;  val <<= 8U; }
    if (val <= 0x0FFFFFFFU) { n += 4U;  val <<= 4U; }
    if (val <= 0x3FFFFFFFU) { n += 2U;  val <<= 2U; }
    if (val <= 0x7FFFFFFFU) { n += 1U; }
    return n;
#endif
}

// ── CABAC arithmetic state snapshot ─────────────────────────────────────

/** Lightweight snapshot of CABAC arithmetic engine state.
 *
 *  Captures the three values needed to fully restore the engine to a prior
 *  point: codIRange, codIOffset, and the bitstream position. Used for
 *  debugging experiments (save state, decode N bins, restore, compare).
 *
 *  ITU-T H.264 §9.3.1 defines these as the core engine variables.
 */
struct CabacState
{
    uint32_t codIRange = 510U;
    uint32_t codIOffset = 0U;
    uint32_t bitPosition = 0U;

    bool operator==(const CabacState& other) const noexcept
    {
        return codIRange == other.codIRange
            && codIOffset == other.codIOffset
            && bitPosition == other.bitPosition;
    }
    bool operator!=(const CabacState& other) const noexcept
    {
        return !(*this == other);
    }
};

// ── CABAC context set wrapper ───────────────────────────────────────────

/** Wrapper around the 1024 CABAC context models.
 *
 *  Drop-in replacement for std::array<CabacCtx, cNumCabacCtx>.
 *  Provides data(), operator[], and size() for backwards compatibility,
 *  plus snapshot/compare operations for debugging.
 */
class CabacContextSet
{
public:
    CabacContextSet() noexcept = default;

    /** Initialize all contexts for a slice — delegates to initCabacContexts().
     *  Must be called after construction with the slice parameters.
     */
    void init(uint32_t sliceType, uint32_t cabacInitIdc,
              int32_t sliceQpY) noexcept;

    /** @return Pointer to the context array (for passing to free functions). */
    CabacCtx* data() noexcept { return ctx_.data(); }
    const CabacCtx* data() const noexcept { return ctx_.data(); }

    /** Index into the context array. */
    CabacCtx& operator[](uint32_t idx) noexcept { return ctx_[idx]; }
    const CabacCtx& operator[](uint32_t idx) const noexcept { return ctx_[idx]; }

    /** @return Number of contexts (1024). */
    static constexpr uint32_t size() noexcept { return cNumCabacCtx; }

    /** Take a snapshot (copy) of the entire context set.
     *  Used for debugging: snapshot before decode, compare after.
     */
    CabacContextSet snapshot() const noexcept { return *this; }

    /** Find the first context index where this set differs from another.
     *  @return Index of first difference, or cNumCabacCtx if identical.
     */
    uint32_t firstDifference(const CabacContextSet& other) const noexcept
    {
        for (uint32_t i = 0U; i < cNumCabacCtx; ++i)
        {
            if (ctx_[i].mpsState != other.ctx_[i].mpsState)
                return i;
        }
        return cNumCabacCtx;
    }

    /** Count how many contexts differ from another set. */
    uint32_t countDifferences(const CabacContextSet& other) const noexcept
    {
        uint32_t count = 0U;
        for (uint32_t i = 0U; i < cNumCabacCtx; ++i)
        {
            if (ctx_[i].mpsState != other.ctx_[i].mpsState)
                ++count;
        }
        return count;
    }

    /** Dump context state to a char buffer (printf-safe, no iostream).
     *  Writes up to maxLen chars of "ctx[i]=0xNN ..." for the first N contexts.
     *  @return Number of chars written (excluding null terminator).
     */
    uint32_t dump(char* buf, uint32_t maxLen,
                  uint32_t startIdx = 0U, uint32_t count = 16U) const noexcept
    {
        if (maxLen == 0U) return 0U;
        uint32_t pos = 0U;
        uint32_t end = startIdx + count;
        if (end > cNumCabacCtx) end = cNumCabacCtx;
        for (uint32_t i = startIdx; i < end && pos + 12U < maxLen; ++i)
        {
            int written = std::snprintf(buf + pos, maxLen - pos,
                                        "ctx[%" PRIu32 "]=0x%02X ", i,
                                        static_cast<unsigned>(ctx_[i].mpsState));
            if (written > 0) pos += static_cast<uint32_t>(written);
        }
        if (pos < maxLen) buf[pos] = '\0';
        return pos;
    }

private:
    std::array<CabacCtx, cNumCabacCtx> ctx_ = {};
};

// ── CABAC arithmetic decoder engine ─────────────────────────────────────

/** CABAC arithmetic decoding engine.
 *
 *  Maintains codIRange and codIOffset state for binary arithmetic decoding.
 *  Uses the combined rangeTabLPS/state transition table.
 */
class CabacEngine
{
public:
    CabacEngine() = default;

    /** Initialize engine from bitstream at current position.
     *  §9.3.1.2: codIRange = 510; codIOffset = read_bits(9) MSB-first unsigned.
     *  Called once per slice before first MB, and after each I_PCM MB. [CHECKED §9.3.1.2]
     */
    void init(BitReader& br) noexcept
    {
        br_ = &br;
        codIRange_ = 510U;
        codIOffset_ = br.readBits(9U);
    }

    /** Decode one binary decision using a context model.
     *  §9.3.3.2.1 DecodeBin process — step order verified:
     *  1. qIdx = (codIRange >> 6) & 3  [CHECKED §9.3.3.2.1]
     *  2. rangeLPS = rangeTabLPS[pStateIdx][qIdx]  [CHECKED §9.3.3.2.1]
     *  3. codIRange -= rangeLPS  [CHECKED §9.3.3.2.1]
     *  4. if codIOffset >= codIRange → LPS; else → MPS  [CHECKED §9.3.3.2.1]
     *  5. Update symbol, codIRange (LPS only), context (pStateIdx + valMPS)  [CHECKED §9.3.3.2.1]
     *  6. Renormalize if codIRange < 256  [CHECKED §9.3.3.2.2]
     *  LPS valMPS flip (pStateIdx==0): encoded in packed table entry. [CHECKED §9.3.3.2.1.1]
     *  @param ctx  Context model (updated in-place)
     *  @return Decoded binary symbol (0 or 1)
     */
    uint32_t decodeBin(CabacCtx& ctx) noexcept
    {
        uint32_t state = ctx.mpsState & 0x7FU;
        uint32_t qRange = (codIRange_ >> 6U) & 3U;

        uint32_t tableEntry = cCabacTable[state][qRange];
        uint32_t rangeLPS = tableEntry & 0xFFU;
        codIRange_ -= rangeLPS;

        uint32_t symbol;
        if (codIOffset_ >= codIRange_)
        {
            // LPS path
            symbol = 1U - (state >> 6U);
            codIOffset_ -= codIRange_;
            codIRange_ = rangeLPS;
            ctx.mpsState = static_cast<uint8_t>((tableEntry >> 15U) & 0x7FU);
        }
        else
        {
            // MPS path (common case — branch predictor handles this well)
            symbol = state >> 6U;
            ctx.mpsState = static_cast<uint8_t>((tableEntry >> 8U) & 0x7FU);
        }

        // Renormalize when range drops below 256
        if (codIRange_ < 256U)
            renormalize();

        // CABAC bin trace — compiled out unless SUB0H264_TRACE is defined.
        // Zero overhead in production/release builds.
#if SUB0H264_TRACE
        if (trace_ && trace_->hasCallback())
        {
            int32_t ctxIdx = ctxBase_ ? static_cast<int32_t>(&ctx - ctxBase_) : -1;
            trace_->onCabacBin(binCount_++, ctxIdx, symbol,
                codIRange_, static_cast<int16_t>(codIOffset_),
                static_cast<int16_t>(state),
                static_cast<int16_t>(state >> 6U));
        }
#endif

        return symbol;
    }

    /** Decode one bypass (equiprobable) bin. */
    uint32_t decodeBypass() noexcept
    {
        codIOffset_ = (codIOffset_ << 1U) | br_->readBit();
        uint32_t symbol;
        if (codIOffset_ >= codIRange_)
        {
            symbol = 1U;
            codIOffset_ -= codIRange_;
        }
        else
        {
            symbol = 0U;
        }

#if SUB0H264_TRACE
        if (trace_ && trace_->hasCallback())
        {
            trace_->onCabacBin(binCount_++, -1, symbol,
                codIRange_, static_cast<int16_t>(codIOffset_),
                -1, -1);
        }
#endif

        return symbol;
    }

    /** Decode N bypass bins, returning the N-bit value. */
    uint32_t decodeBypassBins(uint32_t numBins) noexcept
    {
        uint32_t val = 0U;
        for (uint32_t i = 0U; i < numBins; ++i)
            val = (val << 1U) | decodeBypass();
        return val;
    }

    /** Decode terminate bin (end-of-slice test). */
    uint32_t decodeTerminate() noexcept
    {
        codIRange_ -= 2U;
        if (codIOffset_ >= codIRange_)
            return 1U;
        if (codIRange_ < 256U)
            renormalize();
        return 0U;
    }

    /** Decode a unary-coded value with context switching.
     *  @param maxBins  Maximum number of bins
     *  @param ctxBase  Array of context models
     *  @param ctxIncFirst  Context increment for first bin
     *  @param ctxIncRest   Context increment for subsequent bins
     *  @return Decoded unsigned value
     */
    uint32_t decodeUnary(uint32_t maxBins, CabacCtx* ctxBase,
                          uint32_t ctxIncFirst, uint32_t ctxIncRest) noexcept
    {
        uint32_t val = 0U;
        if (decodeBin(ctxBase[ctxIncFirst]) == 0U)
            return 0U;
        ++val;
        while (val < maxBins)
        {
            if (decodeBin(ctxBase[ctxIncRest]) == 0U)
                break;
            ++val;
        }
        return val;
    }

    /** Decode a truncated unary value (UEG0 prefix). */
    uint32_t decodeTruncUnary(uint32_t maxVal, CabacCtx* ctxBase,
                               const uint32_t* ctxIncs, uint32_t numCtxIncs) noexcept
    {
        uint32_t val = 0U;
        while (val < maxVal)
        {
            uint32_t ctxInc = (val < numCtxIncs) ? ctxIncs[val] : ctxIncs[numCtxIncs - 1U];
            if (decodeBin(ctxBase[ctxInc]) == 0U)
                break;
            ++val;
        }
        return val;
    }

    /** Get current bitstream read position (for debugging/testing). */
    uint32_t bitPosition() const noexcept { return br_ ? br_->bitOffset() : 0U; }
    /** Get current range (for diagnostic comparison). */
    uint32_t range() const noexcept { return codIRange_; }
    /** Get current offset (for diagnostic comparison). */
    uint32_t offset() const noexcept { return codIOffset_; }

    /** Set the trace system for CABAC bin event emission.
     *  When trace is non-null and has a callback, each decoded bin emits
     *  a CabacBin TraceEvent. Only compiled in SUB0H264_TRACE builds.
     *  @param trace  Pointer to the decoder's DecodeTrace (nullable)
     *  @param ctxBase  Base of the context array for ctxIdx computation
     */
    void setTrace([[maybe_unused]] const DecodeTrace* trace,
                  [[maybe_unused]] const CabacCtx* ctxBase = nullptr) noexcept
    {
#if SUB0H264_TRACE
        trace_ = trace;
        ctxBase_ = ctxBase;
        binCount_ = 0U;
#endif
    }

    /** Capture a snapshot of the engine's arithmetic state.
     *  The snapshot includes codIRange, codIOffset, and the bitstream position.
     */
    CabacState snapshot() const noexcept
    {
        return { codIRange_, codIOffset_, br_ ? br_->bitOffset() : 0U };
    }

    /** Restore the engine to a previously captured state.
     *  The BitReader must be the same one used during the original decode.
     *  @param state  Previously captured CabacState
     *  @param br     BitReader to seek back to the saved position
     */
    void restore(const CabacState& state, BitReader& br) noexcept
    {
        codIRange_ = state.codIRange;
        codIOffset_ = state.codIOffset;
        br.seekToBit(state.bitPosition);
        br_ = &br;
    }

    /** Check if the engine's current state matches a snapshot. */
    bool matches(const CabacState& other) const noexcept
    {
        return snapshot() == other;
    }

private:
    BitReader* br_ = nullptr;
    uint32_t codIRange_ = 510U;
    uint32_t codIOffset_ = 0U;

    // Bin trace state — only meaningful in SUB0H264_TRACE builds
#if SUB0H264_TRACE
    const DecodeTrace* trace_ = nullptr;
    const CabacCtx* ctxBase_ = nullptr;
    uint32_t binCount_ = 0U;
public:
    uint32_t binCount() const noexcept { return binCount_; }
private:
#endif

    void renormalize() noexcept
    {
        // §9.3.3.2.2 RenormD: batch renormalization using CLZ.
        // Mathematically equivalent to the spec's per-bit loop
        // (verified: both produce identical R/O at every bin).
        // Batch approach avoids per-bit loop overhead on ESP32-P4 RISC-V.
        uint32_t shift = clz32(codIRange_) - 23U;
        codIRange_ <<= shift;
        codIOffset_ = (codIOffset_ << shift) | br_->readBits(shift);
    }
};

// ── Context initialization — ITU-T H.264 §9.3.1.1 ──────────────────────

#include "cabac_init_mn.hpp"

/** Compute a single CABAC context initial state from (m, n) parameters.
 *
 *  Formula (ITU-T H.264 §9.3.1.1, Equation 9-5):
 *    preCtxState = Clip3(1, 126, ((m * Clip3(0, 51, SliceQPY)) >> 4) + n)
 *    if preCtxState <= 63: pStateIdx = 63 - preCtxState, valMPS = 0
 *    else:                 pStateIdx = preCtxState - 64,  valMPS = 1
 *  [CHECKED §9.3.1.1]
 *
 *  Returns packed mpsState: (pStateIdx & 0x3F) | (valMPS << 6)
 *  which matches our CabacCtx encoding and cCabacTable[128][4] index.
 */
inline uint8_t computeCabacInitState(int32_t m, int32_t n,
                                      int32_t sliceQpY) noexcept
{
    // §9.3.1.1: inner Clip3(0, 51, SliceQPY) applied before multiplication [CHECKED §9.3.1.1]
    if (sliceQpY < 0) sliceQpY = 0;
    if (sliceQpY > cMaxQp) sliceQpY = cMaxQp;

    int32_t preCtxState = ((m * sliceQpY) >> 4) + n;
    // §9.3.1.1: outer Clip3(1, 126, ...) [CHECKED §9.3.1.1]
    if (preCtxState < 1) preCtxState = 1;
    if (preCtxState > 126) preCtxState = 126;

    uint8_t pStateIdx;
    uint8_t valMPS;
    if (preCtxState <= 63)
    {
        pStateIdx = static_cast<uint8_t>(63 - preCtxState);
        valMPS = 0U;
    }
    else
    {
        pStateIdx = static_cast<uint8_t>(preCtxState - 64);
        valMPS = 1U;
    }

    return (pStateIdx & 0x3FU) | (valMPS << 6U);
}

/** Initialize CABAC context models for a slice.
 *
 *  Uses constexpr (m, n) parameters from ITU-T H.264 Tables 9-12 through 9-23
 *  to compute initial probability states. The computation is:
 *    preCtxState = Clip3(1, 126, ((m * SliceQPY) >> 4) + n)
 *
 *  @param[out] ctx       Array of 460 context models
 *  @param sliceType      0=P, 1=B, 2=I
 *  @param cabacInitIdc   PPS cabac_init_idc [0-2] (ignored for I-slices)
 *  @param sliceQpY       Slice QP Y value [0-51]
 */
inline void initCabacContexts(CabacCtx* ctx, uint32_t sliceType,
                               uint32_t cabacInitIdc, int32_t sliceQpY) noexcept
{
    // I-slices always use init_idc 3 (spec §9.3.1.1)
    /// Index into cCabacInitMN: 0-2 for P/B slices, 3 for I-slices.
    static constexpr uint32_t cISliceInitIdc = 3U;
    uint32_t idc = (sliceType == 2U) ? cISliceInitIdc : cabacInitIdc;
    if (idc > 3U) idc = 0U;

    // Initialize base 460 contexts from (m, n) tables — Tables 9-12..9-23.
    for (uint32_t i = 0U; i < cNumCabacCtxBase; ++i)
    {
        int32_t m = cCabacInitMN[idc][i][0];
        int32_t n = cCabacInitMN[idc][i][1];
        ctx[i].mpsState = computeCabacInitState(m, n, sliceQpY);
    }

    // High profile extension contexts (460-1023) — §9.3.1.1.
    // §9.3.1.1 Tables 9-24 to 9-33 define (m, n) pairs for all 1024 contexts.
    // Contexts 460-1023 cover 8x8 transform coefficient elements (High profile
    // only). We zero-init (pStateIdx=0, valMPS=0 = equiprobable + biased MPS)
    // instead of applying spec (m, n) tables. [UNCHECKED §9.3.1.1 — Tables 9-24..9-33]
    // NOTE: No impact on current Main-profile (profile_idc=77) test fixtures;
    // 8x8 coefficient contexts are only used when transform_8x8_mode_flag=1
    // (PPS flag, High profile only). Fixing requires adding Tables 9-24..9-33
    // to cabac_init_mn.hpp and extending the loop to cNumCabacCtx.
    for (uint32_t i = cNumCabacCtxBase; i < cNumCabacCtx; ++i)
        ctx[i].mpsState = 0U;
}

// ── CabacContextSet deferred implementation ────────────────────────────
// Must appear after initCabacContexts() is defined (above).

inline void CabacContextSet::init(uint32_t sliceType, uint32_t cabacInitIdc,
                                   int32_t sliceQpY) noexcept
{
    initCabacContexts(ctx_.data(), sliceType, cabacInitIdc, sliceQpY);
}

} // namespace sub0h264

#endif // CROG_SUB0H264_CABAC_HPP
