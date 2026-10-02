/** Sub0h264 — CABAC specification reference implementations
 *
 *  Each function implements exactly ONE spec formula/table/algorithm,
 *  cited verbatim. These are used both in production AND as test oracles
 *  to verify the optimized decode path.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_CABAC_SPEC_HPP
#define CROG_SUB0H264_CABAC_SPEC_HPP

#include "cabac.hpp"

#include <cstdint>

namespace sub0h264 {
namespace spec {

// ── §9.3.1.1: Context initialization ───────────────────────────────────

/** Compute initial CABAC context state per §9.3.1.1 Equation 9-5.
 *
 *  preCtxState = Clip3(1, 126, ((m * Clip3(0, 51, SliceQPY)) >> 4) + n)
 *  if (preCtxState <= 63)
 *      pStateIdx = 63 - preCtxState
 *      valMPS = 0
 *  else
 *      pStateIdx = preCtxState - 64
 *      valMPS = 1
 *  [CHECKED §9.3.1.1]
 *
 *  @return Packed mpsState: (pStateIdx & 0x3F) | (valMPS << 6)
 */
inline uint8_t contextInit(int32_t m, int32_t n, int32_t sliceQpY) noexcept
{
    // §9.3.1.1: inner Clip3(0, 51, SliceQPY) before multiplication [CHECKED §9.3.1.1]
    if (sliceQpY < 0) sliceQpY = 0;
    if (sliceQpY > cMaxQp) sliceQpY = cMaxQp;

    int32_t preCtxState = ((m * sliceQpY) >> 4) + n;
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

// ── §9.3.3.1.1.3: ctxIdxInc for mb_type ────────────────────────────────

/** Derive ctxIdxInc for mb_type in I-slices — §9.3.3.1.1.3 Table 9-39.
 *
 *  condTermFlagN:
 *    - If mbAddrN is not available: condTermFlagN = 0
 *    - If mbAddrN is available:
 *        condTermFlagN = (mb_type of mbAddrN != I_NxN) ? 1 : 0
 *        where I_NxN means I_4x4 or I_8x8
 *
 *  ctxIdxInc = condTermFlagA + condTermFlagB
 *
 *  @param leftAvail  True if left neighbor macroblock is available
 *  @param topAvail   True if top neighbor macroblock is available
 *  @param leftIsINxN True if left neighbor mb_type is I_4x4 or I_8x8
 *  @param topIsINxN  True if top neighbor mb_type is I_4x4 or I_8x8
 *  @return ctxIdxInc [0-2]
 */
inline uint32_t ctxIdxIncMbTypeI(bool leftAvail, bool topAvail,
                                  bool leftIsINxN, bool topIsINxN) noexcept
{
    // §9.3.3.1.1.3: condTermFlagN = 0 when not available
    uint32_t condTermA = (leftAvail && !leftIsINxN) ? 1U : 0U;
    uint32_t condTermB = (topAvail && !topIsINxN) ? 1U : 0U;
    return condTermA + condTermB;
}

// ── §9.3.3.1.1.1: ctxIdxInc for mb_skip_flag ──────────────────────────

/** Derive ctxIdxInc for mb_skip_flag in P-slices — §9.3.3.1.1.1.
 *
 *  condTermFlagN:
 *    - If mbAddrN is not available: condTermFlagN = 0
 *    - If mbAddrN is available:
 *        condTermFlagN = (mb_skip_flag of mbAddrN == 1) ? 0 : 1
 *        (neighbor was NOT skipped → condTerm=1)
 *
 *  ctxIdxInc = condTermFlagA + condTermFlagB
 *
 *  Note: condTerm=1 when neighbor is NOT skipped. So more non-skip
 *  neighbors → higher ctxInc → different context model.
 */
inline uint32_t ctxIdxIncMbSkipP(bool leftAvail, bool topAvail,
                                  bool leftSkipped, bool topSkipped) noexcept
{
    uint32_t condTermA = (leftAvail && !leftSkipped) ? 1U : 0U;
    uint32_t condTermB = (topAvail && !topSkipped) ? 1U : 0U;
    return condTermA + condTermB;
}

// ── §9.3.3.1.1.7: ctxIdxInc for intra_chroma_pred_mode ────────────────

/** Derive ctxIdxInc for intra_chroma_pred_mode — §9.3.3.1.1.7.
 *
 *  condTermFlagN:
 *    - If mbAddrN is not available: condTermFlagN = 0
 *    - If mbAddrN is not Intra: condTermFlagN = 0
 *    - Else: condTermFlagN = (IntraChromaPredMode of mbAddrN != 0) ? 1 : 0
 *
 *  ctxIdxInc = condTermFlagA + condTermFlagB
 */
inline uint32_t ctxIdxIncChromaMode(bool leftAvail, bool topAvail,
                                     bool leftIsIntra, bool topIsIntra,
                                     uint8_t leftChromaMode, uint8_t topChromaMode) noexcept
{
    uint32_t condTermA = (leftAvail && leftIsIntra && leftChromaMode != 0U) ? 1U : 0U;
    uint32_t condTermB = (topAvail && topIsIntra && topChromaMode != 0U) ? 1U : 0U;
    return condTermA + condTermB;
}

// ── §9.3.3.1.1.5: ctxIdxInc for mb_qp_delta ───────────────────────────

/** Derive ctxIdxInc for mb_qp_delta bin 0 — §9.3.3.1.1.5.
 *
 *  ctxIdxInc = (prevMbQpDeltaNotZero) ? 1 : 0
 *
 *  where prevMbQpDeltaNotZero is true if the previously decoded
 *  mb_qp_delta was not zero.
 */
inline uint32_t ctxIdxIncQpDelta(bool prevMbQpDeltaNotZero) noexcept
{
    return prevMbQpDeltaNotZero ? 1U : 0U;
}

// ── §9.3.3.1.1.4: ctxIdxInc for coded_block_pattern ───────────────────

/** Derive ctxIdxInc for coded_block_pattern luma — §9.3.3.1.1.4.
 *
 *  For each of the 4 luma 8x8 blocks:
 *  condTermFlagN depends on the CBP of the neighboring 8x8 block.
 *
 *  @param leftLumaCbpBit  0 if left neighbor 8x8 has coded coefficients, 1 if not
 *  @param topLumaCbpBit   0 if top neighbor 8x8 has coded coefficients, 1 if not
 *  @return ctxIdxInc [0-3]
 */
inline uint32_t ctxIdxIncCbpLuma(uint32_t leftLumaCbpBit, uint32_t topLumaCbpBit) noexcept
{
    // §9.3.3.1.1.4: condTermFlagN = (cbp bit of neighboring 8x8 == 0) ? 0 : 1
    // But the actual formula inverts: ctxInc = condTermA + 2*condTermB
    // where condTermN = (neighbor 8x8 block NOT coded) ? 1 : 0
    return leftLumaCbpBit + topLumaCbpBit * 2U;
}

// ── §9.3.1.2: Arithmetic decoding engine initialization ────────────────

/** Verify CABAC engine init state per §9.3.1.2.
 *
 *  codIRange = 510
 *  codIOffset = read_bits(9)
 *
 *  @return true if the engine state matches spec requirements
 */
inline bool verifyEngineInit(uint32_t codIRange, uint32_t codIOffset) noexcept
{
    // §9.3.1.2: codIRange shall be 510 after init
    if (codIRange != 510U) return false;
    // §9.3.1.2: codIOffset shall be < codIRange
    if (codIOffset >= codIRange) return false;
    return true;
}

// ── §9.3.3.2.1: Arithmetic decoding (decodeBin) ───────────────────────

/** Spec-verbatim decodeBin using Table 9-45 rangeTabLPS directly.
 *
 *  This is the reference implementation — NOT using the packed cCabacTable.
 *  Used to cross-check the optimized engine.
 *
 *  @param codIRange   [in/out] Current range
 *  @param codIOffset  [in/out] Current offset
 *  @param pStateIdx   [in/out] Context state [0-63]
 *  @param valMPS      [in/out] Most probable symbol [0-1]
 *  @param br          BitReader for renormalization
 *  @return Decoded bin value (0 or 1)
 */
uint32_t decodeBinSpec(uint32_t& codIRange, uint32_t& codIOffset,
                        uint8_t& pStateIdx, uint8_t& valMPS,
                        BitReader& br) noexcept;

// ── Table 9-45: rangeTabLPS[64][4] ─────────────────────────────────────
// Defined in cabac_spec_tables.cpp or inline below.

/// ITU-T H.264 Table 9-45: rangeTabLPS indexed by [pStateIdx][qCodIRangeIdx].
inline constexpr uint8_t cRangeTabLPS[64][4] = {
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

/// ITU-T H.264 Table 9-46: transIdxLPS[64].
inline constexpr uint8_t cTransIdxLPS[64] = {
     0, 0, 1, 2, 2, 4, 4, 5, 6, 7, 8, 9, 9,11,11,12,
    13,13,15,15,16,16,18,18,19,19,21,21,22,22,23,24,
    24,25,26,26,27,27,28,29,29,30,30,30,31,32,32,33,
    33,33,34,34,35,35,35,36,36,36,37,37,37,38,38,63,
};

/// ITU-T H.264 Table 9-46: transIdxMPS[64].
inline constexpr uint8_t cTransIdxMPS[64] = {
     1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15,16,
    17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
    33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,
    49,50,51,52,53,54,55,56,57,58,59,60,61,62,62,63,
};

// ── Inline spec-verbatim decodeBin ──────────────────────────────────────

inline uint32_t decodeBinSpec(uint32_t& codIRange, uint32_t& codIOffset,
                               uint8_t& pStateIdx, uint8_t& valMPS,
                               BitReader& br) noexcept
{
    // §9.3.3.2.1: Arithmetic decoding process for a binary decision
    uint32_t qCodIRangeIdx = (codIRange >> 6U) & 3U;
    uint32_t codIRangeLPS = cRangeTabLPS[pStateIdx][qCodIRangeIdx];
    codIRange -= codIRangeLPS;

    uint32_t binVal;
    if (codIOffset >= codIRange)
    {
        // LPS path
        binVal = 1U - valMPS;
        codIOffset -= codIRange;
        codIRange = codIRangeLPS;

        if (pStateIdx == 0U)
            valMPS = 1U - valMPS;

        pStateIdx = cTransIdxLPS[pStateIdx];
    }
    else
    {
        // MPS path
        binVal = valMPS;
        pStateIdx = cTransIdxMPS[pStateIdx];
    }

    // §9.3.3.2.2: Renormalization
    while (codIRange < 256U)
    {
        codIRange <<= 1U;
        codIOffset = (codIOffset << 1U) | br.readBit();
    }

    return binVal;
}

/// Spec-verbatim decodeBypass — §9.3.3.2.3.
inline uint32_t decodeBypassSpec(uint32_t& codIRange, uint32_t& codIOffset,
                                  BitReader& br) noexcept
{
    codIOffset = (codIOffset << 1U) | br.readBit();
    if (codIOffset >= codIRange)
    {
        codIOffset -= codIRange;
        return 1U;
    }
    return 0U;
}

/// Spec-verbatim decodeTerminate — §9.3.3.2.4.
inline uint32_t decodeTerminateSpec(uint32_t& codIRange, uint32_t& codIOffset,
                                     BitReader& br) noexcept
{
    codIRange -= 2U;
    if (codIOffset >= codIRange)
        return 1U;
    // Renormalize
    while (codIRange < 256U)
    {
        codIRange <<= 1U;
        codIOffset = (codIOffset << 1U) | br.readBit();
    }
    return 0U;
}

} // namespace spec
} // namespace sub0h264

#endif // CROG_SUB0H264_CABAC_SPEC_HPP
