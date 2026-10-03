/** Sub0h264 — CABAC syntax element parsers
 *
 *  Decodes H.264 syntax elements using CABAC for High profile.
 *  Each function maps a syntax element to its binarization scheme
 *  and context model selection per ITU-T H.264 §9.3.2/9.3.3.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_CABAC_PARSE_HPP
#define CROG_SUB0H264_CABAC_PARSE_HPP

#include "cabac.hpp"
#include "tables.hpp"

#include <cstdint>

namespace sub0h264 {

// ── Context offsets for syntax elements — ITU-T H.264 Table 9-11 ────────
// All ctxIdxOffset values verified against Table 9-11/9-39. [CHECKED Table 9-11]

inline constexpr uint32_t cCtxMbTypeI       = 3U;   ///< mb_type I-slice
inline constexpr uint32_t cCtxMbTypeP       = 14U;  ///< mb_type P/SP-slice
inline constexpr uint32_t cCtxMbSkipP       = 11U;  ///< mb_skip_flag P/SP-slice
inline constexpr uint32_t cCtxSubMbTypeP    = 21U;  ///< sub_mb_type P/SP-slice
inline constexpr uint32_t cCtxRefIdx        = 54U;  ///< ref_idx_l0/l1
inline constexpr uint32_t cCtxMvdX          = 40U;  ///< mvd_l0/l1[...][0]
inline constexpr uint32_t cCtxMvdY          = 47U;  ///< mvd_l0/l1[...][1]
inline constexpr uint32_t cCtxMbQpDelta     = 60U;  ///< mb_qp_delta
inline constexpr uint32_t cCtxIntraChroma   = 64U;  ///< intra_chroma_pred_mode
inline constexpr uint32_t cCtxCbpLuma       = 73U;  ///< coded_block_pattern luma
inline constexpr uint32_t cCtxCbpChroma     = 77U;  ///< coded_block_pattern chroma
inline constexpr uint32_t cCtxCbf           = 85U;  ///< coded_block_flag (cat 0-4)
inline constexpr uint32_t cCtxSigCoeff      = 105U; ///< significant_coeff_flag (cat 0-4)
inline constexpr uint32_t cCtxLastSigCoeff  = 166U; ///< last_significant_coeff_flag (cat 0-4)
inline constexpr uint32_t cCtxCoeffAbsLevel = 227U; ///< coeff_abs_level_minus1 (cat 0-4)
inline constexpr uint32_t cCtxTransform8x8  = 399U; ///< transform_size_8x8_flag
inline constexpr uint32_t cCtxPrevIntra4x4  = 68U;  ///< prev_intra4x4_pred_mode_flag
inline constexpr uint32_t cCtxRemIntra4x4   = 69U;  ///< rem_intra4x4_pred_mode

// ── High profile 8x8 block context offsets — ITU-T H.264 Table 9-42 ───
//
// ctxBlockCat 5 (8x8 luma blocks) uses separate context ranges beyond the
// base 460 contexts. Offsets per ITU-T H.264 Table 9-42 (frame coding):

/// significant_coeff_flag for ctxBlockCat=5 — base context index.
/// 63 positions use non-linear mapping via cSigCtxInc8x8Frame.
/// ITU-T H.264 Table 9-42: ctxIdxOffset = 402 for frame coding.
inline constexpr uint32_t cCtxSigCoeff8x8      = 402U;

/// last_significant_coeff_flag for ctxBlockCat=5 — base context index.
/// 63 positions use non-linear mapping via cLastSigCtxInc8x8Frame.
/// ITU-T H.264 Table 9-42: ctxIdxOffset = 417 for frame coding.
inline constexpr uint32_t cCtxLastSigCoeff8x8   = 417U;

/// coeff_abs_level_minus1 for ctxBlockCat=5 — base context index.
/// ITU-T H.264 Table 9-42: ctxIdxOffset = 426.
inline constexpr uint32_t cCtxCoeffAbsLevel8x8  = 426U;

/// coded_block_flag for ctxBlockCat=5 — High profile extension range.
/// ITU-T H.264 Table 9-42: ctxIdxOffset = 1012 for 8x8 luma blocks.
/// Requires context array expanded to 1024 entries.
inline constexpr uint32_t cCtxCbf8x8            = 1012U;

// ── mb_skip_flag (P-slice) — §9.3.3.1.1.1 ──────────────────────────────

/** Decode mb_skip_flag for P-slice.
 *  §9.3.3.1.1.1 Eq 9-7: ctxIdxInc = condTermFlagA + condTermFlagB.
 *  condTermFlagN = 0 when neighbor skipped or unavailable, 1 otherwise.
 *  [CHECKED §9.3.3.1.1.1]
 */
inline uint32_t cabacDecodeMbSkipP(CabacEngine& engine, CabacCtx* ctx,
                                    bool leftSkip, bool topSkip) noexcept
{
    // condTermFlagA = (leftSkip ? 0 : 1), condTermFlagB = (topSkip ? 0 : 1)
    uint32_t ctxInc = (leftSkip ? 0U : 1U) + (topSkip ? 0U : 1U);
    return engine.decodeBin(ctx[cCtxMbSkipP + ctxInc]);
}

// ── mb_type (I-slice) — §9.3.3.1.1.3 Table 9-36 ───────────────────────

/** Decode mb_type for I-slice (CABAC).
 *  §9.3.3.1.1.3: ctxIdxInc = condTermFlagA + condTermFlagB (Eq 9-10).
 *  condTermFlagN = 0 when neighbor is I_NxN (I_4x4),
 *  = 1 when neighbor is NOT I_NxN (I_16x16, inter, or unavailable).
 *  Table 9-36: I_NxN→0, I_16x16 suffix→6 bins, I_PCM→terminate.
 *  Table 9-39 ctxIdx verified: bin[0]→3+ctxInc, bin[1]→276,
 *  bin[2]→6, bin[3]→7, bin[4]→8(b3=1), bin[5]→9, bin[6]→10.
 *  [CHECKED §9.3.3.1.1.3] [CHECKED Table 9-36] [CHECKED Table 9-39]
 *  @param leftIsI4x4  true when left neighbor is I_NxN (I_4x4)
 *  @param topIsI4x4   true when top neighbor is I_NxN (I_4x4)
 *  @return mb_type raw value [0=I_4x4, 1-24=I_16x16 variants, 25=I_PCM]
 */
inline uint32_t cabacDecodeMbTypeI(CabacEngine& engine, CabacCtx* ctx,
                                    bool leftIsI4x4, bool topIsI4x4) noexcept
{
    // §9.3.3.1.2: ctxIdxInc = condTermFlagA + condTermFlagB
    // condTermFlagN = 0 when I_NxN, 1 otherwise
    uint32_t ctxInc = (leftIsI4x4 ? 0U : 1U) + (topIsI4x4 ? 0U : 1U);

    // bin[0]: ctxInc
    if (engine.decodeBin(ctx[cCtxMbTypeI + ctxInc]) == 0U)
        return 0U; // I_4x4

    // Terminate check for I_PCM
    if (engine.decodeTerminate() == 1U)
        return 25U; // I_PCM

    // I_16x16 binarization — current implementation uses ctx[6..10].
    // Verified WORKING for wstress_gradient (99 dB bit-exact vs JM).
    //
    // SUB0H264_MBTYPEI_JM_CTX (experimental, 2026-04-17): JM 19.0
    // readMB_typeInfo_CABAC_i_slice (cabac.c L711-730) reads
    // ctx[7..11] (act_ctx 4..8 added to ctxIdxOffset=3). Tested but
    // REGRESSES wstress_gradient from 99 dB to 5 dB. Likely the JM
    // mb_type_contexts[] base is offset by 1 from our ctx[]. Kept under
    // guard so we can revisit when investigating Tapo C110 (which
    // diverges at first I_16x16 MB with mb_type_chroma off by 8).
#ifdef SUB0H264_MBTYPEI_JM_CTX
    constexpr uint32_t base = cCtxMbTypeI + 4U; // = 7 (JM convention)
#else
    constexpr uint32_t base = cCtxMbTypeI + 3U; // = 6 (current)
#endif
    uint32_t cbpLuma = engine.decodeBin(ctx[base]);

    uint32_t cbpChromaFlag = engine.decodeBin(ctx[base + 1U]);
    uint32_t cbpChroma = 0U;
    if (cbpChromaFlag != 0U)
        cbpChroma = engine.decodeBin(ctx[base + 2U]) == 0U ? 1U : 2U;

    uint32_t predMode = (engine.decodeBin(ctx[base + 3U]) << 1U) |
                          engine.decodeBin(ctx[base + 4U]);

    // mb_type = 1 + predMode + cbpChroma*4 + cbpLuma*12
    return 1U + predMode + cbpChroma * 4U + cbpLuma * 12U;
}

// ── mb_type (P-slice, inter) — §9.3.3.1.2 ──────────────────────────────

/** Decode mb_type for P-slice (CABAC).
 *  §9.3.3.1.2 Table 9-37: P-inter prefix is exactly 3 bins (binIdx 0-2).
 *  P_8x8ref0 (mb_type=4) is not allowed in CABAC (Table 9-37: "na").
 *  @param leftIsI4x4  true when left neighbor is I_NxN (for intra suffix context)
 *  @param topIsI4x4   true when top neighbor is I_NxN (for intra suffix context)
 *  @return mb_type raw: 0=P_L0_16x16, 1=P_L0_L0_16x8, 2=P_L0_L0_8x16,
 *          3=P_8x8, 5-30=Intra (offset by 5)
 */
inline uint32_t cabacDecodeMbTypeP(CabacEngine& engine, CabacCtx* ctx,
                                    [[maybe_unused]] bool leftIsI4x4,
                                    [[maybe_unused]] bool topIsI4x4) noexcept
{
    // bin[0]: ctxIdx = 14 (Table 9-39, ctxIdxInc=0) [CHECKED §9.3.3.1.2]
    if (engine.decodeBin(ctx[cCtxMbTypeP + 0U]) == 0U)
    {
        // P-inter: exactly 3 bins — Table 9-37 [CHECKED Table 9-37]
        // bin[1]: ctxIdx = 15 (Table 9-39, ctxIdxInc=1) [CHECKED Table 9-39]
        uint32_t bin1 = engine.decodeBin(ctx[cCtxMbTypeP + 1U]);
        // bin[2]: ctxIdxInc = (b1 != 1) ? 2 : 3 — Table 9-41 [CHECKED Table 9-41]
        uint32_t bin2Ctx = (bin1 != 1U) ? 2U : 3U;
        uint32_t bin2 = engine.decodeBin(ctx[cCtxMbTypeP + bin2Ctx]);

        // Table 9-37 bin string → mb_type mapping: [CHECKED Table 9-37]
        //   000 → P_L0_16x16(0), 001 → P_8x8(3),
        //   010 → P_L0_L0_8x16(2), 011 → P_L0_L0_16x8(1)
        if (bin1 == 0U)
            return bin2 == 0U ? 0U : 3U;
        return bin2 == 0U ? 2U : 1U;
    }

    // Intra MB within P-slice — §9.3.3.1.2, verified against JM reference.
    // I-in-P uses P-slice contexts at ctxIdx 17-20 (NOT I-slice contexts 3-10).
    // JM reference: mb_type_contexts[1][7]=I_NxN, [8]=cbpLuma, [9]=cbpChroma, [10]=predMode.
    {
        // bin: I_NxN check — JM uses fixed context mb_type_contexts[1][7] = ctxIdx 17
        if (engine.decodeBin(ctx[17U]) == 0U)
            return 5U + 0U; // I_4x4 (I_NxN) in P-slice

        // Terminate check for I_PCM
        if (engine.decodeTerminate() == 1U)
            return 5U + 25U; // I_PCM in P-slice

        // I_16x16 suffix — JM: mb_type_contexts[1][8,9,10]
        uint32_t cbpLuma = engine.decodeBin(ctx[18U]);       // AC/no-AC

        uint32_t cbpChroma = 0U;
        if (engine.decodeBin(ctx[19U]) != 0U)                // cbpChroma > 0
        {
            cbpChroma = engine.decodeBin(ctx[19U]) == 0U ? 1U : 2U; // cbpChroma 1 or 2
        }

        uint32_t predMode = (engine.decodeBin(ctx[20U]) << 1U) |
                              engine.decodeBin(ctx[20U]);     // 2-bit pred mode

        return 5U + 1U + predMode + cbpChroma * 4U + cbpLuma * 12U;
    }
}

// ── coded_block_pattern — §9.3.3.1.1.4 ─────────────────────────────────

/** Decode coded_block_pattern for CABAC — §9.3.3.1.1.4.
 *
 *  Each of the 4 luma bins uses condTermFlag derived from a SPECIFIC 8x8
 *  block of the left/top neighbor (not just "has any luma CBP").
 *
 *  Block layout: 0=TL, 1=TR, 2=BL, 3=BR.
 *  Block 0: left=leftCbp bit 1, top=topCbp bit 2
 *  Block 1: left=current bit 0,  top=topCbp bit 3
 *  Block 2: left=leftCbp bit 3, top=current bit 0
 *  Block 3: left=current bit 2,  top=current bit 1
 *
 *  condTermFlagN = 1 when neighbor block is NOT coded (cbp bit=0),
 *                  0 when coded (cbp bit=1) or unavailable (treat as coded).
 *
 *  @param leftLumaCbp  Left MB's 4-bit luma CBP (bits 0-3), or 0x0F if unavailable
 *  @param topLumaCbp   Top MB's 4-bit luma CBP (bits 0-3), or 0x0F if unavailable
 *  @param leftChromaCbp Left MB's chroma CBP (0-2), or 2 if unavailable
 *  @param topChromaCbp  Top MB's chroma CBP (0-2), or 2 if unavailable
 */
inline uint8_t cabacDecodeCbp(CabacEngine& engine, CabacCtx* ctx,
                               uint8_t leftLumaCbp, uint8_t topLumaCbp,
                               uint8_t leftChromaCbp, uint8_t topChromaCbp) noexcept
{
    // Luma CBP: 4 bins, one per 8x8 block — §9.3.3.1.1.4
    // condTermFlagN = (neighbor_cbp_bit == 0) ? 1 : 0
    // ctxIdxInc = condTermFlagA + 2*condTermFlagB (Eq 9-12)
    // Block neighbor mapping verified: TL←(left.TR, top.BL), etc. [CHECKED §9.3.3.1.1.4]
    uint8_t cbpLuma = 0U;

    // Block 0 (TL): A=left's bit 1, B=top's bit 2
    {
        uint32_t cA = (leftLumaCbp & 0x02U) ? 0U : 1U;
        uint32_t cB = (topLumaCbp  & 0x04U) ? 0U : 2U;
        if (engine.decodeBin(ctx[cCtxCbpLuma + cA + cB]) == 1U) cbpLuma |= 1U;
    }
    // Block 1 (TR): A=current's bit 0, B=top's bit 3
    {
        uint32_t cA = (cbpLuma     & 0x01U) ? 0U : 1U;
        uint32_t cB = (topLumaCbp  & 0x08U) ? 0U : 2U;
        if (engine.decodeBin(ctx[cCtxCbpLuma + cA + cB]) == 1U) cbpLuma |= 2U;
    }
    // Block 2 (BL): A=left's bit 3, B=current's bit 0
    {
        uint32_t cA = (leftLumaCbp & 0x08U) ? 0U : 1U;
        uint32_t cB = (cbpLuma     & 0x01U) ? 0U : 2U;
        if (engine.decodeBin(ctx[cCtxCbpLuma + cA + cB]) == 1U) cbpLuma |= 4U;
    }
    // Block 3 (BR): A=current's bit 2, B=current's bit 1
    {
        uint32_t cA = (cbpLuma & 0x04U) ? 0U : 1U;
        uint32_t cB = (cbpLuma & 0x02U) ? 0U : 2U;
        if (engine.decodeBin(ctx[cCtxCbpLuma + cA + cB]) == 1U) cbpLuma |= 8U;
    }

    // Chroma CBP: 2 bins — §9.3.3.1.1.4, §9.3.2.6
    // §9.3.3.1.1.4 for ctxIdxOffset=77: condTermFlagN for CHROMA has OPPOSITE
    // sense from luma. The spec says:
    //   condTermFlag=0 when: unavailable/skip OR chromaCbp=0 (bin0) OR chromaCbp!=2 (bin1)
    //   condTermFlag=1 otherwise (available AND chromaCbp>0/==2)
    // Use sentinel value 0xFF to mark unavailable (condTermFlag=0 regardless).
    // [FM-23 fixed: was using luma sense (coded→0), spec says chroma coded→1]
    uint8_t cbpChroma = 0U;
    {
        // condTermFlag=1 when available AND chromaCbp matches condition, 0 otherwise.
        // Unavailable passed as 0xFF → fails both >0 and >1 checks correctly
        // IF we fix the default to NOT look like coded chroma.
        // Actually: unavailable → leftChromaCbp is from default cbp 0x2F → chromaCbp=2.
        // We need to detect unavailable separately. Use 3 as "unavailable" sentinel
        // (valid chroma cbp is 0, 1, or 2).
        uint32_t cA = (leftChromaCbp <= 2U && leftChromaCbp > 0U) ? 1U : 0U;
        uint32_t cB = (topChromaCbp  <= 2U && topChromaCbp  > 0U) ? 2U : 0U;
        if (engine.decodeBin(ctx[cCtxCbpChroma + cA + cB]) == 1U)
        {
            cA = (leftChromaCbp <= 2U && leftChromaCbp > 1U) ? 1U : 0U;
            cB = (topChromaCbp  <= 2U && topChromaCbp  > 1U) ? 2U : 0U;
            cbpChroma = engine.decodeBin(ctx[cCtxCbpChroma + cA + cB + 4U]) == 0U ? 1U : 2U;
        }
    }

    return cbpLuma | (cbpChroma << 4U);
}

// ── mb_qp_delta — §9.3.3.1.1.5 / §9.3.2.7 ─────────────────────────────

/** Decode mb_qp_delta for CABAC.
 *  §9.3.3.1.1.5: bin[0] ctxIdxInc = 0 or 1 based on previous MB qp_delta.
 *  §9.3.2.7 Table 9-3: U binarization mapped to signed via
 *    delta = (-1)^(k+1) * ceil(k/2) for coded value k.
 *  Table 9-39: bin[0]→60+{0,1}, bin[1]→62, bins[2+]→63. [CHECKED §9.3.3.1.1.5]
 */
inline int32_t cabacDecodeMbQpDelta(CabacEngine& engine, CabacCtx* ctx,
                                     bool prevMbHadQpDelta) noexcept
{
    uint32_t ctxInc0 = prevMbHadQpDelta ? 1U : 0U;
    if (engine.decodeBin(ctx[cCtxMbQpDelta + ctxInc0]) == 0U)
        return 0;

    // Unary with sign: decode absolute value then sign
    uint32_t absVal = 1U;
    while (absVal < 52U)
    {
        if (engine.decodeBin(ctx[cCtxMbQpDelta + 2U + (absVal > 1U ? 1U : 0U)]) == 0U)
            break;
        ++absVal;
    }

    // Map to signed: odd→positive, even→negative
    // 1→+1, 2→-1, 3→+2, 4→-2, ...
    int32_t delta;
    if (absVal & 1U)
        delta = static_cast<int32_t>((absVal + 1U) >> 1U);
    else
        delta = -static_cast<int32_t>(absVal >> 1U);

    return delta;
}

// ── MVD (motion vector difference) — §9.3.3.1.1.7 ──────────────────────

/** Decode one MVD component (x or y) for CABAC.
 *  §9.3.3.1.1.7 Eqs 9-15/16/17: bin[0] ctxIdxInc from |mvdA|+|mvdB|.
 *    absMvd < 3 → ctxInc=0, 3..32 → ctxInc=1, >32 → ctxInc=2.
 *  Table 9-39: bin[0]→ctxOff+{0,1,2}, bin[1]→+3, bin[2]→+4, bin[3]→+5,
 *    bin[4]→+6, bins[5+]→+6. [CHECKED §9.3.3.1.1.7] [CHECKED Table 9-39]
 *  @param ctxOffset  cCtxMvdX or cCtxMvdY
 *  @param absMvdNeighbor  Sum of |mvd| of left + top neighbors
 */
inline int16_t cabacDecodeMvd(CabacEngine& engine, CabacCtx* ctx,
                               uint32_t ctxOffset, int32_t absMvdNeighbor) noexcept
{
    // Context selection for bin 0 based on neighbor magnitude
    uint32_t ctxInc;
    if (absMvdNeighbor < 3)       ctxInc = 0U;
    else if (absMvdNeighbor <= 32) ctxInc = 1U;
    else                           ctxInc = 2U;

    // Prefix: truncated unary (max 9)
    uint32_t prefix = 0U;
    if (engine.decodeBin(ctx[ctxOffset + ctxInc]) == 1U)
    {
        ++prefix;
        while (prefix < 9U)
        {
            uint32_t binCtx = (prefix < 4U) ? (prefix + 2U) : 6U;
            if (engine.decodeBin(ctx[ctxOffset + binCtx]) == 0U)
                break;
            ++prefix;
        }
    }

    int32_t absMvd;
    if (prefix < 9U)
    {
        absMvd = static_cast<int32_t>(prefix);
    }
    else
    {
        // Suffix: 3rd-order Exp-Golomb (k=3) via bypass bins — §9.3.2.3 Table 9-37.
        // UEG3 decode: start with k=3, each leading 1 adds (1<<k) and increments k,
        // then a 0 stop bit, then k bits of the remainder.
        constexpr uint32_t cEgOrder = 3U;
        uint32_t k = cEgOrder;
        uint32_t suffix = 0U;
        while (engine.decodeBypass() == 1U && k < 16U)
        {
            suffix += (1U << k);
            ++k;
        }
        suffix += engine.decodeBypassBins(k);
        absMvd = static_cast<int32_t>(9U + suffix);
    }

    if (absMvd == 0)
        return 0;

    // Sign bit (bypass)
    uint32_t sign = engine.decodeBypass();
    return sign ? static_cast<int16_t>(-absMvd) : static_cast<int16_t>(absMvd);
}

// ── Residual coefficient decoding — §9.3.3.1.9 ─────────────────────────

/** Decode significant_coeff_flag + last_significant_coeff_flag + levels
 *  for one 4x4 block using CABAC.
 *
 *  @param engine      CABAC engine
 *  @param ctx         Context array
 *  @param[out] coeffs Output coefficients in scan order
 *  @param maxCoeff    Maximum number of coefficients (15 for AC, 16 for 4x4)
 *  @param ctxBlockCat Block category for context offset selection
 *  @return Number of non-zero coefficients
 */
/** Decode a 4x4 residual block via CABAC — §9.3.3.1.1.
 *  @param cbfCtxInc  Context increment for coded_block_flag (0-3).
 *                    Derived from neighbor NNZ per §9.3.3.1.1.3:
 *                    ctxInc = (nA != 0) + 2*(nB != 0) where nA=left, nB=top.
 *                    Pass 0 for DC blocks (cat 0,3) where cbf is not decoded.
 */
inline uint32_t cabacDecodeResidual4x4(CabacEngine& engine, CabacCtx* ctx,
                                        int16_t* coeffs, uint32_t maxCoeff,
                                        uint32_t ctxBlockCat,
                                        uint32_t cbfCtxInc = 0U) noexcept
{
    // coded_block_flag — §7.3.5.3.3 + §9.3.3.1.1.1 Table 9-34.
    {
        uint32_t cbfCtx = cCtxCbf + ctxBlockCat * 4U + cbfCtxInc;
        if (engine.decodeBin(ctx[cbfCtx]) == 0U)
            return 0U;
    }

    // Context offsets per block category — ITU-T H.264 Table 9-40.
    // ctxIdx = ctxIdxOffset + ctxIdxBlockCatOffset(cat) + ctxIdxInc.
    // NOT uniformly spaced: chroma DC has only 3 sig contexts, chroma AC starts at 152.
    // [CHECKED Table 9-40]
    static constexpr uint32_t cSigOffsets[5]   = {105, 120, 134, 149, 152};
    static constexpr uint32_t cLastOffsets[5]  = {166, 181, 195, 210, 213};
    // §9.3.3.1.3: coeff_abs_level_minus1 ctxIdxBlockCatOffset per Table 9-40.
    // Cat 3 (chroma DC) has only 9 contexts (max increment=8), not 10.
    // So cat 4 offset = 30+9 = 39 (not 40). [CHECKED Table 9-40]
    static constexpr uint32_t cLevelOffsets[5] = {227, 237, 247, 257, 266};
    uint32_t sigOffset   = cSigOffsets[ctxBlockCat];
    uint32_t lastOffset  = cLastOffsets[ctxBlockCat];
    uint32_t levelOffset = cLevelOffsets[ctxBlockCat];

    // Decode significant coefficient map — §9.3.3.1.3 residual_block_cabac()
    uint8_t sigMap[16] = {};
    uint32_t numSig = 0U;
    int32_t lastSigIdx = -1;
    bool foundLast = false;

    for (uint32_t i = 0U; i < maxCoeff - 1U; ++i)
    {
        if (engine.decodeBin(ctx[sigOffset + i]) == 1U)
        {
            sigMap[i] = 1U;
            lastSigIdx = static_cast<int32_t>(i);
            ++numSig;

            // Check last_significant_coeff_flag
            if (engine.decodeBin(ctx[lastOffset + i]) == 1U)
            {
                foundLast = true;
                break;
            }
        }
    }

    // §9.3.3.1.3: Position maxCoeff-1 is implicitly significant when the
    // loop exhausted without last_significant_coeff_flag=1. This applies even
    // when numSig==0 (only the last scan position has a coefficient).
    // [CHECKED §9.3.3.1.3]
    if (!foundLast)
    {
        // Loop exhausted: position maxCoeff-1 is implicitly significant
        sigMap[maxCoeff - 1U] = 1U;
        lastSigIdx = static_cast<int32_t>(maxCoeff - 1U);
        ++numSig;
    }

    if (numSig == 0U)
        return 0U; // Malformed: coded_block_flag=1 but foundLast with no coefficients

    // Decode coefficient levels (reverse scan order)
    uint32_t numT1 = 0U;
    uint32_t numLarger = 0U;

    for (int32_t i = lastSigIdx; i >= 0; --i)
    {
        if (!sigMap[i])
            continue;

        // coeff_abs_level_minus1: prefix (truncated unary)
        // §9.3.3.1.3: bin[0] ctxIdxInc:
        //   numDecodAbsLevelGt1 > 0: ctxIdxInc = 0
        //   numDecodAbsLevelGt1 == 0: ctxIdxInc = Min(numDecodAbsLevelEq1 + 1, 4)
        // [CHECKED §9.3.3.1.3]
        uint32_t ctxInc;
        if (numLarger > 0U)
            ctxInc = 0U;
        else
            ctxInc = (numT1 < 4U) ? (numT1 + 1U) : 4U;

        uint32_t prefix = 0U;
        if (engine.decodeBin(ctx[levelOffset + ctxInc]) == 1U)
        {
            ++prefix;
            // §9.3.3.1.3: bins k>=1 ctxIdxInc = 5 + Min(numDecodAbsLevelGt1, maxInc)
            // maxInc = 3 for ctxBlockCat==3 (chroma DC, 9 contexts), 4 otherwise.
            // [CHECKED §9.3.3.1.3]
            uint32_t maxInc = (ctxBlockCat == 3U) ? 3U : 4U;
            uint32_t nextCtx;
            if (numLarger > 0U)
                nextCtx = 5U + (numLarger < maxInc ? numLarger : maxInc);
            else
                nextCtx = 5U;
            while (prefix < 14U)
            {
                if (engine.decodeBin(ctx[levelOffset + nextCtx]) == 0U)
                    break;
                ++prefix;
            }
        }

        int32_t absLevel;
        if (prefix < 14U)
        {
            absLevel = static_cast<int32_t>(prefix) + 1;
        }
        else
        {
            // Suffix: Exp-Golomb k=0 via bypass
            uint32_t k = 0U;
            while (engine.decodeBypass() == 1U && k < 16U)
                ++k;
            uint32_t suffix = ((1U << k) - 1U) + engine.decodeBypassBins(k);
            absLevel = static_cast<int32_t>(14U + suffix) + 1;
        }

        // Sign (bypass)
        uint32_t sign = engine.decodeBypass();
        // Output in SCAN ORDER — caller applies zigzag reordering.
        coeffs[i] = sign ? static_cast<int16_t>(-absLevel) : static_cast<int16_t>(absLevel);

        // Update context tracking
        if (absLevel == 1)
            ++numT1;
        else
            ++numLarger;
    }

    return numSig;
}

// ── 8x8 block context increment mapping — ITU-T H.264 Table 9-43 ──────

/// Context index increment mapping for significant_coeff_flag ctxBlockCat=5.
/// Non-linear mapping per ITU-T H.264 Table 9-43 (frame coded blocks).
/// Index i (0..62) maps to the ctxIdxInc added to cCtxSigCoeff8x8.
inline constexpr std::array<uint8_t, 63> cSigCtxInc8x8Frame = {
     0,  1,  2,  3,  4,  5,  5,  4,
     4,  3,  3,  4,  4,  4,  5,  5,
     4,  4,  4,  4,  3,  3,  6,  7,
     7,  7,  8,  9, 10,  9,  8,  7,
     7,  6, 11, 12, 13, 11,  6,  7,
     8,  9, 14, 10,  9,  8,  6, 11,
    12, 13, 11,  6,  9, 14, 10,  9,
    11, 12, 13, 11, 14, 10, 12,
};

/// Context index increment mapping for last_significant_coeff_flag ctxBlockCat=5.
/// Non-linear mapping per ITU-T H.264 Table 9-43 (frame coded blocks).
/// Index i (0..62) maps to the ctxIdxInc added to cCtxLastSigCoeff8x8.
inline constexpr std::array<uint8_t, 63> cLastSigCtxInc8x8Frame = {
    0, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1,
    2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 3,
    4, 4, 4, 4, 4, 4, 4, 4,
    5, 5, 5, 5, 6, 6, 6, 6,
    7, 7, 7, 7, 8, 8, 8,
};

// Compile-time spot-checks — ITU-T H.264 Table 9-43.
static_assert(cSigCtxInc8x8Frame[0]  ==  0U, "sig[0]=0 per Table 9-43");
static_assert(cSigCtxInc8x8Frame[6]  ==  5U, "sig[6]=5 per Table 9-43");
static_assert(cSigCtxInc8x8Frame[22] ==  6U, "sig[22]=6 per Table 9-43");
static_assert(cSigCtxInc8x8Frame[42] == 14U, "sig[42]=14 per Table 9-43");
static_assert(cSigCtxInc8x8Frame[62] == 12U, "sig[62]=12 per Table 9-43");
static_assert(cLastSigCtxInc8x8Frame[0]  == 0U, "last[0]=0 per Table 9-43");
static_assert(cLastSigCtxInc8x8Frame[16] == 2U, "last[16]=2 per Table 9-43");
static_assert(cLastSigCtxInc8x8Frame[40] == 4U, "last[40]=4 per Table 9-43");
static_assert(cLastSigCtxInc8x8Frame[62] == 8U, "last[62]=8 per Table 9-43");

// ── 8x8 CABAC residual decode — ITU-T H.264 §9.3.3.1.1 ───────────────

/** Decode a 8x8 residual block via CABAC — High profile §9.3.3.1.1.
 *
 *  Uses ctxBlockCat=5 with non-linear context mapping per Table 9-43.
 *  Decodes coded_block_flag, 63 significant/last positions, and levels
 *  for a full 64-coefficient 8x8 block.
 *
 *  @param engine     CABAC engine
 *  @param ctx        Context array (1024 entries for High profile)
 *  @param[out] coeffs Output 64 coefficients in scan order (caller applies
 *                     zigzag reordering via cZigzag8x8)
 *  @param cbfCtxInc  Context increment for coded_block_flag (0-3).
 *                    Derived from neighbor NNZ per §9.3.3.1.1.3.
 *  @return Number of non-zero coefficients
 */
inline uint32_t cabacDecodeResidual8x8(CabacEngine& engine, CabacCtx* ctx,
                                        int16_t* coeffs, uint32_t cbfCtxInc = 0U) noexcept
{
    /// Maximum number of coefficients in an 8x8 block.
    static constexpr uint32_t cMaxCoeff8x8 = 64U;

    // §7.3.5.3.3 residual_block_cabac(): coded_block_flag is read only when
    // `maxNumCoeff != 64 || ChromaArrayType == 3`. For 8x8 luma (maxNumCoeff=64,
    // ChromaArrayType=1 for 4:2:0): condition is FALSE → cbf NOT read. [CHECKED §7.3.5.3.3]
    // Caller gates entry via CBP (CodedBlockPatternLuma bit); if CBP says residual
    // is present, we proceed directly into the significant coefficient map decode.
    (void)cbfCtxInc;

    // Decode significant coefficient map — §9.3.3.1.3 residual_block_cabac()
    // 8x8 blocks use non-linear context mapping per Table 9-43.
    uint8_t sigMap[cMaxCoeff8x8] = {};
    uint32_t numSig = 0U;
    int32_t lastSigIdx = -1;
    bool foundLast = false;

    for (uint32_t i = 0U; i < cMaxCoeff8x8 - 1U; ++i)
    {
        uint32_t sigCtx = cCtxSigCoeff8x8 + cSigCtxInc8x8Frame[i];
        if (engine.decodeBin(ctx[sigCtx]) == 1U)
        {
            sigMap[i] = 1U;
            lastSigIdx = static_cast<int32_t>(i);
            ++numSig;

            // Check last_significant_coeff_flag
            uint32_t lastCtx = cCtxLastSigCoeff8x8 + cLastSigCtxInc8x8Frame[i];
            if (engine.decodeBin(ctx[lastCtx]) == 1U)
            {
                foundLast = true;
                break;
            }
        }
    }

    // §9.3.3.1.3: Position maxCoeff-1 (63) is implicitly significant when
    // the loop exhausted without last_significant_coeff_flag=1. This applies
    // even when numSig==0 (only position 63 has a coefficient). [CHECKED §9.3.3.1.3]
    if (!foundLast)
    {
        sigMap[cMaxCoeff8x8 - 1U] = 1U;
        lastSigIdx = static_cast<int32_t>(cMaxCoeff8x8 - 1U);
        ++numSig;
    }

    if (numSig == 0U)
        return 0U; // Malformed: CBP set but foundLast with no coefficients

    // Decode coefficient levels (reverse scan order) — §9.3.3.1.3.
    // coeff_abs_level_minus1 uses ctxBlockCat=5 offsets at cCtxCoeffAbsLevel8x8.
    uint32_t numT1 = 0U;
    uint32_t numLarger = 0U;

    for (int32_t i = lastSigIdx; i >= 0; --i)
    {
        if (!sigMap[i])
            continue;

        // coeff_abs_level_minus1: prefix (truncated unary)
        // §9.3.3.1.3: bin 0 context depends on trailing ones / larger counts.
        uint32_t ctxInc;
        if (numLarger > 0U)
            ctxInc = 0U;
        else
            ctxInc = (numT1 < 4U) ? (numT1 + 1U) : 4U;

        uint32_t prefix = 0U;
        if (engine.decodeBin(ctx[cCtxCoeffAbsLevel8x8 + ctxInc]) == 1U)
        {
            ++prefix;
            // §9.3.3.1.3: bins k>=1 use ctxIdxInc = 5 + min(numDecodAbsLevelGt1, 4)
            /// Maximum additional context increment for level suffix bins.
            static constexpr uint32_t cMaxLevelCtxInc8x8 = 4U;
            uint32_t nextCtx = 5U + (numLarger < cMaxLevelCtxInc8x8
                                     ? numLarger : cMaxLevelCtxInc8x8);
            while (prefix < 14U)
            {
                if (engine.decodeBin(ctx[cCtxCoeffAbsLevel8x8 + nextCtx]) == 0U)
                    break;
                ++prefix;
            }
        }

        int32_t absLevel;
        if (prefix < 14U)
        {
            absLevel = static_cast<int32_t>(prefix) + 1;
        }
        else
        {
            // Suffix: Exp-Golomb k=0 via bypass bins — §9.3.3.1.3.
            uint32_t k = 0U;
            while (engine.decodeBypass() == 1U && k < 16U)
                ++k;
            uint32_t suffix = ((1U << k) - 1U) + engine.decodeBypassBins(k);
            absLevel = static_cast<int32_t>(14U + suffix) + 1;
        }

        // Sign (bypass)
        uint32_t sign = engine.decodeBypass();
        // Output in SCAN ORDER — caller applies zigzag reordering via cZigzag8x8.
        coeffs[i] = sign ? static_cast<int16_t>(-absLevel)
                         : static_cast<int16_t>(absLevel);

        // Update context tracking
        if (absLevel == 1)
            ++numT1;
        else
            ++numLarger;
    }

    return numSig;
}

// ── Intra prediction mode (CABAC) — Table 9-34/9-39 ───────────────────

/** Decode prev_intra4x4_pred_mode_flag + rem_intra4x4_pred_mode.
 *  Table 9-34: prev_intra4x4_pred_mode_flag: FL, maxBinIdxCtx=0, ctxIdxOffset=68.
 *  Table 9-34: rem_intra4x4_pred_mode: FL cMax=7, ctxIdxOffset=69.
 *  Table 9-39 row 69: ctxIdxInc=0 for binIdx 0,1,2 → all context-coded at ctx[69].
 *  Bit ordering: LSB-first (value = b0 | b1<<1 | b2<<2) — verified against x264
 *  encoded bitstreams. Both bypass and MSB-first produce wrong output.
 *  [CHECKED Table 9-34] [CHECKED Table 9-39]
 */
inline uint8_t cabacDecodeIntra4x4PredMode(CabacEngine& engine, CabacCtx* ctx) noexcept
{
    if (engine.decodeBin(ctx[cCtxPrevIntra4x4]) == 1U)
        return 0xFFU; // Use most probable mode

    // 3 context-coded bins at ctx[69], LSB-first — verified matches ffmpeg:
    //   mode += 1 * get_cabac(&sl->cabac, &sl->cabac_state[69]);
    //   mode += 2 * get_cabac(&sl->cabac, &sl->cabac_state[69]);
    //   mode += 4 * get_cabac(&sl->cabac, &sl->cabac_state[69]);
    uint32_t b0 = engine.decodeBin(ctx[cCtxRemIntra4x4]);
    uint32_t b1 = engine.decodeBin(ctx[cCtxRemIntra4x4]);
    uint32_t b2 = engine.decodeBin(ctx[cCtxRemIntra4x4]);
    return static_cast<uint8_t>(b0 | (b1 << 1U) | (b2 << 2U));
}

/** Decode intra_chroma_pred_mode (CABAC) — §9.3.3.1.1.8.
 *  §9.3.3.1.1.8: ctxIdxInc = condTermFlagA + condTermFlagB.
 *  condTermFlagN = (neighbor_chroma_mode != 0) ? 1 : 0 (or 0 if unavailable).
 *  Table 9-39: bin[0]→64+{0,1,2}, bins[1,2]→64+3=67. [CHECKED §9.3.3.1.1.8]
 *  TU binarization cMax=3: DC=0, H=10, V=110, Plane=111.
 *  @param ctxInc  Pre-computed context increment [0-2] from neighbor chroma modes.
 */
inline uint32_t cabacDecodeIntraChromaMode(CabacEngine& engine, CabacCtx* ctx,
                                            uint32_t ctxInc) noexcept
{
    if (engine.decodeBin(ctx[cCtxIntraChroma + ctxInc]) == 0U)
        return 0U; // DC

    if (engine.decodeBin(ctx[cCtxIntraChroma + 3U]) == 0U)
        return 1U; // Horizontal

    return engine.decodeBin(ctx[cCtxIntraChroma + 3U]) == 0U ? 2U : 3U;
}

// ── ref_idx_l0 (CABAC) — §9.3.3.1.1.6 / Table 9-39 ───────────────────

/** Decode ref_idx_l0 for CABAC — truncated unary binarization.
 *  §9.3.3.1.1.6: bin[0] ctxInc from neighbor ref_idx (condTermFlagA + condTermFlagB).
 *  Table 9-39: ctxIdxOffset=54, bin[0]→ctxInc from §9.3.3.1.1.6,
 *    bin[1]→ctxInc=4, bins[2+]→ctxInc=5. [CHECKED §9.3.3.1.1.6]
 *  @param ctxInc0  Pre-computed ctxIdxInc for bin[0] (0-3, from neighbor ref_idx)
 *  @param maxRefIdx  Maximum ref_idx value (num_ref_idx_l0_active - 1)
 *  @return Decoded ref_idx value [0..maxRefIdx]
 */
inline uint8_t cabacDecodeRefIdx(CabacEngine& engine, CabacCtx* ctx,
                                  uint32_t ctxInc0, uint32_t maxRefIdx) noexcept
{
    if (maxRefIdx == 0U)
        return 0U; // Only one reference — no decode needed

    // bin[0]: ctxIdx = 54 + ctxInc0  [CHECKED §9.3.3.1.1.6]
    if (engine.decodeBin(ctx[cCtxRefIdx + ctxInc0]) == 0U)
        return 0U;

    // bins[1+]: unary decode with ctx_offset=1 — §9.3.3.1.2 Table 9-39.
    // JM reference decoder uses unbounded unary (not truncated at maxRefIdx).
    // bin[1]: ctxIdx = 54 + 4, bins[2+]: ctxIdx = 54 + 5.
    uint8_t refIdx = 1U;
    uint32_t ctxOff = 4U;
    while (engine.decodeBin(ctx[cCtxRefIdx + ctxOff]) != 0U)
    {
        ++refIdx;
        ctxOff = 5U; // bins 2+ all use ctxInc=5
    }
    return refIdx;
}

// ── sub_mb_type (P-slice, CABAC) — Table 9-38 ────────────────────────

/** Decode sub_mb_type for P-slice CABAC.
 *  Table 9-38 binarization for P-slice sub_mb_type:
 *    P_L0_8x8:  1
 *    P_L0_8x4:  00
 *    P_L0_4x8:  011
 *    P_L0_4x4:  010
 *  Table 9-39: ctxIdxOffset=21, bin[0]→ctxInc=0, bin[1]→ctxInc=1, bin[2]→ctxInc=2.
 *  [CHECKED Table 9-38] [CHECKED Table 9-39]
 *  @return sub_mb_type: 0=P_L0_8x8, 1=P_L0_8x4, 2=P_L0_4x8, 3=P_L0_4x4
 */
inline uint32_t cabacDecodeSubMbTypeP(CabacEngine& engine, CabacCtx* ctx) noexcept
{
    if (engine.decodeBin(ctx[cCtxSubMbTypeP + 0U]) != 0U)
        return 0U; // P_L0_8x8

    if (engine.decodeBin(ctx[cCtxSubMbTypeP + 1U]) == 0U)
        return 1U; // P_L0_8x4

    if (engine.decodeBin(ctx[cCtxSubMbTypeP + 2U]) != 0U)
        return 2U; // P_L0_4x8

    return 3U; // P_L0_4x4
}

} // namespace sub0h264

#endif // CROG_SUB0H264_CABAC_PARSE_HPP
