/** Sub0h264 — CABAC macroblock syntax element parser
 *
 *  Provides a bound interface to CABAC syntax element decode operations.
 *  Holds references to the engine, context set, and neighbor state so that
 *  individual syntax elements can be decoded and tested in isolation without
 *  needing the full H264Decoder.
 *
 *  This class does NOT perform reconstruction (prediction, IDCT, pixel write).
 *  It only handles bitstream parsing of CABAC-coded syntax elements per
 *  ITU-T H.264 §9.3.3.
 *
 *  Usage:
 *    CabacMbParser parser;
 *    parser.bind(engine, ctxSet, neighbor);
 *    uint32_t mbType = parser.decodeMbTypeI(mbX, mbY);
 *    uint8_t cbp = parser.decodeCbp(mbX, mbY);
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_CABAC_MB_PARSER_HPP
#define CROG_SUB0H264_CABAC_MB_PARSER_HPP

#include "cabac.hpp"
#include "cabac_neighbor.hpp"
#include "cabac_parse.hpp"

namespace sub0h264 {

/** CABAC macroblock-level syntax element parser.
 *
 *  Binds to a CabacEngine + CabacContextSet + CabacNeighborCtx and provides
 *  named methods for each syntax element. Each method cites the spec section
 *  that defines the binarization and context assignment.
 *
 *  Spec-annotated review (2026-04-09):
 *    Routing verified: each method calls the correct cabac_parse.hpp function
 *    with the correct neighbor-derived context. [CHECKED §9.3.3]
 *    NOTE: This is a TEST wrapper. The main decoder calls cabac_parse functions
 *    directly with inline context derivation. Bugs in the decoder's context
 *    derivation are NOT caught by reviewing this file.
 *    [PARTIAL] decodeMvd: parameter named 'comp' but passed as absMvdNeighbor
 *    [PARTIAL] decodeResidual4x4: cbfCtxInc always 0 (test-only; decoder computes inline)
 *    [PARTIAL] decodeTransform8x8Flag: ctxIdxInc always 0 (missing neighbor derivation)
 */
class CabacMbParser
{
public:
    CabacMbParser() noexcept = default;

    /** Bind to engine, contexts, and neighbor state.
     *  Must be called before any decode method.
     */
    void bind(CabacEngine& engine, CabacContextSet& ctxSet,
              CabacNeighborCtx& neighbor) noexcept
    {
        engine_ = &engine;
        ctx_ = &ctxSet;
        neighbor_ = &neighbor;
    }

    /** @return True if bind() has been called. */
    bool isBound() const noexcept { return engine_ != nullptr; }

    // ── Slice-level syntax elements ─────────────────────────────────────

    /** Decode mb_skip_flag for P-slices — §9.3.3.1.1.1.
     *  @return 1 if MB is skipped, 0 otherwise.
     */
    uint32_t decodeMbSkipP(uint32_t mbX, uint32_t mbY) noexcept
    {
        bool leftSkip, topSkip;
        neighbor_->skipCtx(mbX, mbY, leftSkip, topSkip);
        return cabacDecodeMbSkipP(*engine_, ctx_->data(), leftSkip, topSkip);
    }

    /** Decode mb_type for I-slices — §9.3.3.1.1.3 Table 9-39.
     *  Also updates the neighbor I_NxN flag.
     *  @return Raw mb_type value (0=I_4x4, 1-24=I_16x16 variants, 25=I_PCM).
     */
    uint32_t decodeMbTypeI(uint32_t mbX, uint32_t mbY) noexcept
    {
        bool leftCondZero, topCondZero;
        neighbor_->mbTypeCtxI(mbX, mbY, leftCondZero, topCondZero);
        uint32_t mbType = cabacDecodeMbTypeI(*engine_, ctx_->data(),
                                              leftCondZero, topCondZero);
        // Update neighbor state
        uint32_t mbIdx = mbY * neighbor_->widthMbs() + mbX;
        (*neighbor_)[mbIdx].setI4x4(mbType == 0U);
        return mbType;
    }

    /** Decode mb_type for P-slices — §9.3.3.1.2.
     *  @return Raw mb_type (0-4 = inter, 5+ = intra in P-slice).
     */
    uint32_t decodeMbTypeP(uint32_t mbX, uint32_t mbY) noexcept
    {
        bool leftCondZero, topCondZero;
        neighbor_->mbTypeCtxI(mbX, mbY, leftCondZero, topCondZero);
        return cabacDecodeMbTypeP(*engine_, ctx_->data(),
                                  leftCondZero, topCondZero);
    }

    // ── MB-level syntax elements ────────────────────────────────────────

    /** Decode prev_intra4x4_pred_mode_flag + rem_intra4x4_pred_mode — §9.3.3.1.1.3.
     *  @return Packed: bit 3 = flag, bits 0-2 = rem (if flag=0).
     */
    uint8_t decodeIntra4x4PredMode() noexcept
    {
        return cabacDecodeIntra4x4PredMode(*engine_, ctx_->data());
    }

    /** Decode intra_chroma_pred_mode — §9.3.3.1.1.7.
     *  Uses neighbor chroma modes from CabacNeighborCtx for context derivation.
     *  Also stores the decoded mode in the neighbor context.
     *  @return 0=DC, 1=H, 2=V, 3=Plane.
     */
    uint32_t decodeIntraChromaMode(uint32_t mbX, uint32_t mbY) noexcept
    {
        uint32_t ctxInc = neighbor_->chromaModeCtxInc(mbX, mbY);
        uint32_t mode = cabacDecodeIntraChromaMode(*engine_, ctx_->data(), ctxInc);
        uint32_t mbIdx = mbY * neighbor_->widthMbs() + mbX;
        (*neighbor_)[mbIdx].chromaMode = static_cast<uint8_t>(mode);
        return mode;
    }

    /** Decode coded_block_pattern — §9.3.3.1.1.4.
     *  Uses neighbor CBP for context derivation.
     *  @return Packed CBP: bits 0-3 = luma, bits 4-5 = chroma.
     */
    uint8_t decodeCbp(uint32_t mbX, uint32_t mbY) noexcept
    {
        auto cbpN = neighbor_->cbpNeighbors(mbX, mbY);
        return cabacDecodeCbp(*engine_, ctx_->data(),
                              cbpN.left & 0x0FU, cbpN.top & 0x0FU,
                              (cbpN.left >> 4U) & 3U, (cbpN.top >> 4U) & 3U);
    }

    /** Decode mb_qp_delta — §9.3.3.1.1.5.
     *  @param prevHadDelta  True if previous MB had non-zero QP delta.
     *  @return Signed QP delta value.
     */
    int32_t decodeMbQpDelta(bool prevHadDelta) noexcept
    {
        return cabacDecodeMbQpDelta(*engine_, ctx_->data(), prevHadDelta);
    }

    /** Decode motion vector difference — §9.3.3.1.1.6.
     *  @param ctxBase  Context base index (cCtxMvdX or cCtxMvdY).
     *  @param comp     Component (0=x, 1=y).
     *  @return Signed MVD value.
     */
    int16_t decodeMvd(uint32_t ctxBase, uint32_t comp) noexcept
    {
        return cabacDecodeMvd(*engine_, ctx_->data(), ctxBase, comp);
    }

    /** Decode residual 4x4 block — §9.3.3.1.3.
     *  @param[out] coeffs   Output coefficient array (scan order).
     *  @param maxCoeff      Maximum coefficients (15 or 16).
     *  @param ctxBlockCat   Block category (0-4).
     *  @return Number of significant coefficients.
     */
    uint32_t decodeResidual4x4(int16_t* coeffs, uint32_t maxCoeff,
                                uint32_t ctxBlockCat) noexcept
    {
        return cabacDecodeResidual4x4(*engine_, ctx_->data(),
                                      coeffs, maxCoeff, ctxBlockCat);
    }

    /** Decode residual 8x8 block — §9.3.3.1.3 with ctxBlockCat 5.
     *  @param[out] coeffs  Output coefficient array (64 entries, scan order).
     */
    void decodeResidual8x8(int16_t* coeffs) noexcept
    {
        cabacDecodeResidual8x8(*engine_, ctx_->data(), coeffs);
    }

    /** Decode end_of_slice_flag via decodeTerminate() — §7.3.4. */
    uint32_t decodeEndOfSlice() noexcept
    {
        return engine_->decodeTerminate();
    }

    /** Decode transform_size_8x8_flag — §7.3.5. */
    bool decodeTransform8x8Flag() noexcept
    {
        return engine_->decodeBin((*ctx_)[cCtxTransform8x8]) != 0U;
    }

    // ── Direct access for advanced usage ────────────────────────────────

    CabacEngine& engine() noexcept { return *engine_; }
    CabacContextSet& contexts() noexcept { return *ctx_; }
    CabacNeighborCtx& neighbors() noexcept { return *neighbor_; }

private:
    CabacEngine* engine_ = nullptr;
    CabacContextSet* ctx_ = nullptr;
    CabacNeighborCtx* neighbor_ = nullptr;
};

} // namespace sub0h264

#endif // CROG_SUB0H264_CABAC_MB_PARSER_HPP
