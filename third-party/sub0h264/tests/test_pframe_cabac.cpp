/** Sub0h264 — CABAC P-frame decode verification tests
 *
 *  Documents and guards the key behaviors verified during the investigation
 *  of the P-frame CABAC quality bug (6 dB on P-frames, IDR/I-frames correct).
 *
 *  Key confirmed-correct behaviors (each has a corresponding test):
 *
 *  1. ctx[11] init (mb_skip_flag P-slice, ctxInc=0):
 *     At QP=20, cabac_init_idc=0: pStateIdx=2, valMPS=0.
 *     Strongly non-skip biased — probability of decoding skip is low.
 *     Derivation: m=23 n=33 → preCtxState=(23*20>>4)+33=61 ≤ 63 → pStateIdx=63-61=2, mps=0
 *
 *  2. MB0 skip flag arithmetic (scrolling_texture_high.h264 frame 1):
 *     R=510 O=251 → qIdx=3 lps=rangeTabLPS[2][3]=216 → R_new=294 O=251<294 → MPS=0 (non-skip).
 *     Verified independently by pure-Python spec CABAC implementation.
 *
 *  3. CABAC slice init position:
 *     P-NALU slice header is 24 bits; CABAC engine reads 9-bit codIOffset → bp=33.
 *     Confirmed by Python bitstream parse: O=251 at bp=33.
 *
 *  4. MB0 is a coded P-inter MB (P_L0_L0_8x16, mb_type=2):
 *     MvPrediction fires for MB(0,0) of the first P-frame.
 *
 *  Root-cause note (still open):
 *    The P-inter decode path over-consumes bits: by MB28, only 118 bits remain
 *    of the 5128-bit first P-NALU, and the MB decode consumes 203 (85 past end).
 *    This leaves the remaining 272/300 MBs as zero pixels (the 6 dB PSNR symptom).
 *    Tests 3 and 4 will continue to pass even with this bug since they only look at
 *    MB0-MB27. The full-frame PSNR test in test_synthetic_quality.cpp tracks the bug.
 *
 *  Reference: ITU-T H.264 §9.3 (CABAC), §7.3.4 (slice data), §9.3.3.1.1 (ctxInc)
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "test_fixtures.hpp"
#include "../components/sub0h264/src/cabac.hpp"
#include "../components/sub0h264/src/bitstream.hpp"
#include "../components/sub0h264/src/decoder.hpp"
#include "../components/sub0h264/src/annexb.hpp"

#include <cmath>
#include <memory>
#include <vector>

using namespace sub0h264;

// ── §9.3.1.1: Context init exact values for P-slice skip flag ─────────────

TEST_CASE("CABAC §9.3.1.1: P-slice idc=0 QP=20 ctx[11] mb_skip_flag exact init")
{
    // §9.3.1.1: preCtxState = Clip3(1,126, ((m * SliceQPY) >> 4) + n)
    // P-slice idc=0, ctx[11] (mb_skip_flag ctxInc=0): m=23, n=33
    //   preCtxState = (23 * 20 >> 4) + 33 = 28 + 33 = 61
    //   61 <= 63 → pStateIdx = 63 - 61 = 2, valMPS = 0
    //
    // This is a strongly non-skip biased context at QP=20 (low-probability skip).
    // Confirmed empirically: MB0 of scrolling_texture_high.h264 decodes as non-skip.
    CabacCtx ctx[cNumCabacCtx] = {};
    initCabacContexts(ctx, 0U /* P-slice */, 0U /* idc=0 */, 20 /* QP=20 */);

    uint8_t state11 = ctx[11U].mpsState;
    uint32_t pStateIdx = state11 & 0x3FU;
    uint32_t valMPS    = (state11 >> 6U) & 1U;

    MESSAGE("ctx[11] mpsState=0x" << std::hex << (int)state11 << std::dec
            << " pStateIdx=" << pStateIdx << " valMPS=" << valMPS);

    CHECK(pStateIdx == 2U);   // Spec derivation: 63 - 61 = 2
    CHECK(valMPS    == 0U);   // preCtxState <= 63 → valMPS = 0
}

TEST_CASE("CABAC §9.3.1.1: P-slice idc=0 QP=20 ctx[12] and ctx[13] skip flag (ctxInc=1,2)")
{
    // §9.3.3.1.1.1: ctxInc = (leftSkip?0:1) + (topSkip?0:1)
    // ctx[12] for ctxInc=1 (one non-skip neighbor): m=23, n=2
    //   preCtxState = (23*20>>4)+2 = 28+2 = 30 ≤ 63 → pStateIdx=33, valMPS=0
    // ctx[13] for ctxInc=2 (both neighbors non-skip): m=21, n=0
    //   preCtxState = (21*20>>4)+0 = 26+0 = 26 ≤ 63 → pStateIdx=37, valMPS=0
    //
    // All three skip-flag contexts are non-skip biased at QP=20.
    CabacCtx ctx[cNumCabacCtx] = {};
    initCabacContexts(ctx, 0U, 0U, 20);

    uint32_t pState12 = ctx[12U].mpsState & 0x3FU;
    uint32_t mps12    = (ctx[12U].mpsState >> 6U) & 1U;
    uint32_t pState13 = ctx[13U].mpsState & 0x3FU;
    uint32_t mps13    = (ctx[13U].mpsState >> 6U) & 1U;

    CHECK(pState12 == 33U);  // 63 - 30 = 33
    CHECK(mps12    == 0U);
    CHECK(pState13 == 37U);  // 63 - 26 = 37
    CHECK(mps13    == 0U);
}

// ── §9.3.2: CABAC arithmetic engine skip-flag decode ──────────────────────

TEST_CASE("CABAC §9.3.2: P-frame skip flag arithmetic with O=251 decodes non-skip")
{
    // This test encodes the exact arithmetic verified for MB0 of
    // scrolling_texture_high.h264 first P-frame (frameNum=1).
    //
    // Confirmed by:
    //   1. Decoder diagnostic: R=510 O=251 after CABAC init (bp=33)
    //   2. Pure-Python spec CABAC trace: skip=0 (non-skip) for MB0
    //
    // Manufacture a byte stream such that the 9-bit CABAC init reads O=251.
    //   251 = 0b011111011 (9 bits)
    //   Byte 0: 0b01111101 = 0x7D  (bits 0-7 of 9-bit O, MSB first)
    //   Byte 1: 0b1xxxxxxx = 0x80  (bit 8 = MSB of second byte = 1)
    //   Remaining bytes: 0x00 (just padding — decodeBin won't read them for MPS)
    //
    // ctx[11]: pStateIdx=2, valMPS=0 (verified by §9.3.1.1 test above)
    // R=510: qIdx = (510>>6) & 3 = 7 & 3 = 3
    // lps  = rangeTabLPS[2][3] = 216  (§9.3.1.2 Table 9-45, row 2 = [128,158,187,216])
    // R_new = 510 - 216 = 294
    // O=251 < R_new=294 → MPS → symbol = valMPS = 0  (non-skip)
    // No renorm (R_new=294 >= 256).
    //
    // §9.3.2 equation 9-9: codIRange_LPS = Table 9-45[pState][qIdx]
    // §9.3.2 equation 9-10: if codIOffset < codIRange_MPS → MPS decision

    uint8_t data[] = {0x7DU, 0x80U, 0x00U, 0x00U, 0x00U};
    BitReader br(data, 5U);

    CabacEngine engine;
    engine.init(br);

    // Verify CABAC init read the expected O=251
    REQUIRE(engine.range()  == 510U);
    REQUIRE(engine.offset() == 251U);
    MESSAGE("After init: R=" << engine.range() << " O=" << engine.offset());

    // Initialize ctx[11] exactly as the P-slice decoder would
    CabacCtx ctx[cNumCabacCtx] = {};
    initCabacContexts(ctx, 0U, 0U, 20);

    // Decode the skip flag
    uint32_t skipFlag = engine.decodeBin(ctx[11U]);

    CHECK(skipFlag == 0U);           // MPS=valMPS=0 → non-skip
    CHECK(engine.range() == 294U);   // 510 - 216 = 294
    CHECK(engine.offset() == 251U);  // O unchanged (MPS path, no bits read — R_new ≥ 256)

    MESSAGE("After skip decode: R=" << engine.range() << " O=" << engine.offset()
            << " skip=" << skipFlag);
}

// ── Integration: first P-frame CABAC state on real fixture ────────────────

TEST_CASE("CABAC P-frame: scrolling_texture_high CABAC init at bit 33 for P-slice")
{
    // The first P-NALU (frameNum=1) of scrolling_texture_high.h264 has a
    // 24-bit slice header followed by 9-bit CABAC init → bp=33 at MB(0,0) start.
    //
    // Confirmed by Python bitstream parse of raw RBSP bytes:
    //   bits[24..32] = 011111011 = 251 = O  (after emulation prevention removal)
    //
    // This test catches any regression that shifts the slice header parse or
    // changes the byte alignment before CABAC init.
    //
    // The P-slice CABAC loop emits MbStart(a=200, b=bitPosition) at the start of
    // each MB (added alongside the equivalent I-slice event — see decoder.hpp).
    //
    // ITU-T H.264 §9.3.1.2: CABAC initialisation process — reads 9-bit codIOffset.
    auto h264 = getFixture("scrolling_texture_high.h264");
    if (h264.empty())
    {
        MESSAGE("scrolling_texture_high.h264 not found — skipping");
        return;
    }

    // Capture the CABAC bit position at MB(0,0) for each frame.
    // MbStart(a=200, b=bitPos) fires at the very start of each MB's CABAC decode.
    // For MB(0,0), this equals the initial CABAC engine bitPosition after init.
    struct CabacStart { uint32_t frameIdx; uint32_t bitPos; };
    std::vector<CabacStart> cabacStarts;
    uint32_t frameCount = 0U;

    auto decoder = std::make_unique<H264Decoder>();
    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.type == TraceEventType::MbStart && e.a == 200U
            && e.mbX == 0U && e.mbY == 0U)
        {
            cabacStarts.push_back({frameCount, e.b});
        }
    });

    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal))
            continue;
        if (decoder->processNal(nal) == DecodeStatus::FrameDecoded)
        {
            ++frameCount;
            if (frameCount >= 2U)
                break; // Only need IDR (frame 0) + first P-frame (frame 1)
        }
    }

    // frameCount increments AFTER the trace fires, so:
    //   IDR fires when frameCount=0, then frameCount→1
    //   P-frame fires when frameCount=1, then frameCount→2
    REQUIRE(cabacStarts.size() >= 2U); // IDR + at least one P-frame

    MESSAGE("IDR CABAC start bit: "     << cabacStarts[0].bitPos
            << " (frame=" << cabacStarts[0].frameIdx << ")");
    MESSAGE("P-frame CABAC start bit: " << cabacStarts[1].bitPos
            << " (frame=" << cabacStarts[1].frameIdx << ")");

    // IDR (I-slice): slice header ends at byte boundary; 9-bit init follows.
    // Exact value depends on IDR header length.
    // P-frame CABAC starts at bit 33 for this fixture:
    //   Slice header: first_mb_in_slice=0 [1b], slice_type=5 [5b], pps_id=0 [1b],
    //   frame_num=1 [4b per SPS log2_max_frame_num=4], byte-align-to-24, then 9-bit init.
    CHECK(cabacStarts[1].bitPos == 33U);
}

TEST_CASE("CABAC P-frame: scrolling_texture_high MB(0,0) produces non-trivial output")
{
    // MB(0,0) of the first P-frame of scrolling_texture_high.h264 is a coded
    // P-inter MB of type P_L0_L0_8x16 (mb_type=2 per CABAC Table 9-37).
    //
    // Confirmed by decoder diagnostics AND pure-Python CABAC trace:
    //   skip=0 (non-skip), mbType=2, mvdX=0 mvdY=4, cbp=0x0b, luma_cbp=0xb
    //
    // This test verifies that MB(0,0) output is non-trivial (not zeroed out),
    // which would happen if skip detection was broken or the CABAC decode
    // for the mb_type/MVD/residual steps produced garbage.
    //
    // The scrolling texture P-frame is motion-compensated from the IDR frame,
    // so MB(0,0) output should be visually similar to IDR MB(0,0) (< 30 luma
    // difference on average) but not identical (MVD = (0,4) = 1-pixel shift down).
    //
    // Note: This test passes even with the current over-consumption bug at MB28,
    // because MB0 decodes correctly before the overrun. The full-frame PSNR test
    // in test_synthetic_quality.cpp catches the 6 dB symptom.
    //
    // ITU-T H.264 §9.3.3.1.1.1: ctxInc for mb_skip_flag
    // ITU-T H.264 §9.3.3.1.2 Table 9-37: mb_type P-slice binarization
    auto h264 = getFixture("scrolling_texture_high.h264");
    if (h264.empty())
    {
        MESSAGE("scrolling_texture_high.h264 not found — skipping");
        return;
    }

    auto decoder = std::make_unique<H264Decoder>();
    uint32_t frameCount = 0U;
    const Frame* idrFrame  = nullptr;
    const Frame* pFrame    = nullptr;

    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal))
            continue;
        if (decoder->processNal(nal) == DecodeStatus::FrameDecoded)
        {
            if (frameCount == 0U) idrFrame = decoder->currentFrame();
            if (frameCount == 1U) { pFrame = decoder->currentFrame(); break; }
            ++frameCount;
        }
    }

    REQUIRE(idrFrame != nullptr);
    REQUIRE(pFrame != nullptr);

    // MB(0,0): first 16x16 luma block (pixels [0..15, 0..15])
    // Compute mean of first row to verify non-zero, non-garbage output
    double mb0Sum = 0.0;
    for (uint32_t c = 0U; c < 16U; ++c)
        mb0Sum += pFrame->y(c, 0U);
    double mb0Mean = mb0Sum / 16.0;

    MESSAGE("P-frame MB(0,0) row-0 Y mean = " << mb0Mean);
    MESSAGE("IDR frame MB(0,0) row-0 pixel[0] = " << (int)idrFrame->y(0U, 0U));
    MESSAGE("P-frame  MB(0,0) row-0 pixel[0] = " << (int)pFrame->y(0U, 0U));

    // Output must be visible content (not zeroed out), and not clipped to 255
    CHECK(mb0Mean >= 20.0);   // Not zero / black
    CHECK(mb0Mean <= 240.0);  // Not clipped white

    // P-frame is motion-compensated from IDR: should be close to IDR output
    // (MVD=(0,4) = 1 pel down shift; residual cbp=0x0b adds correction)
    double maxDiff = 0.0;
    for (uint32_t c = 0U; c < 16U; ++c)
    {
        double d = std::abs(static_cast<double>(pFrame->y(c, 0U))
                            - idrFrame->y(c, 0U));
        maxDiff = std::max(maxDiff, d);
    }
    MESSAGE("MB(0,0) row-0 max pixel diff IDR vs P-frame = " << maxDiff);
    // Must be within 60 luma levels of IDR (1-pel motion + small residual)
    CHECK(maxDiff <= 60.0);
}

TEST_CASE("CABAC P-frame: scrolling_texture_high decodes at least 2 frames without crash")
{
    // Smoke test: stream can be started and produces at least the IDR + first P-frame.
    // Crashes or assertion failures here indicate a fundamental parser regression.
    auto h264 = getFixture("scrolling_texture_high.h264");
    if (h264.empty())
    {
        MESSAGE("scrolling_texture_high.h264 not found — skipping");
        return;
    }

    auto decoder = std::make_unique<H264Decoder>();
    uint32_t frameCount = 0U;

    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal))
            continue;
        if (decoder->processNal(nal) == DecodeStatus::FrameDecoded)
        {
            ++frameCount;
            if (frameCount >= 2U)
                break;
        }
    }

    CHECK(frameCount >= 2U);
}

TEST_CASE("CABAC P-frame 1: PSNR >= 40 dB vs raw source (regression guard)")
{
    // Guards the fixes for:
    //   - MVD 3rd-order Exp-Golomb (k=3 not k=0) — §9.3.2.3 Table 9-37
    //   - P_8x8 per-sub-partition motion/MVD storage — §6.4.2.1
    //   - Skip MB CABAC neighbor CBP = 0 — §9.3.3.1.1.4
    // Frame 1 (first P-frame, numRefL0=1) must decode at 40+ dB vs raw source.
    auto h264 = getFixture("scrolling_texture_high.h264");
    auto raw  = getFixture("scrolling_texture_high_raw.yuv");
    if (h264.empty() || raw.empty())
    {
        MESSAGE("scrolling_texture_high fixture not found - skipping");
        return;
    }

    auto decoder = std::make_unique<H264Decoder>();
    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    uint32_t frameCount = 0U;
    double frame1Psnr = 0.0;
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal))
            continue;
        if (decoder->processNal(nal) == DecodeStatus::FrameDecoded)
        {
            if (frameCount == 1U) // First P-frame
            {
                const Frame* frame = decoder->currentFrame();
                REQUIRE(frame != nullptr);
                uint32_t w = frame->width(), h = frame->height();
                uint32_t lumaSize = w * h;
                uint32_t frameSize = lumaSize + 2U * (w / 2U) * (h / 2U);
                REQUIRE(raw.size() >= 2U * frameSize);

                // Compute luma PSNR vs raw source frame 1
                const uint8_t* refY = raw.data() + frameSize; // frame 1 raw
                double sse = 0.0;
                for (uint32_t r = 0U; r < h; ++r)
                {
                    const uint8_t* decRow = frame->yRow(r);
                    const uint8_t* refRow = refY + r * w;
                    for (uint32_t c = 0U; c < w; ++c)
                    {
                        double d = static_cast<double>(decRow[c]) - refRow[c];
                        sse += d * d;
                    }
                }
                double mse = sse / lumaSize;
                frame1Psnr = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
                MESSAGE("CABAC P-frame 1 PSNR: " << frame1Psnr << " dB");
            }
            ++frameCount;
            if (frameCount >= 2U)
                break;
        }
    }

    REQUIRE(frameCount >= 2U);
    // Frame 1 should decode near-perfectly (52+ dB with current fixes).
    // Use 40 dB as a conservative threshold to catch regressions without
    // being brittle to minor IDCT rounding changes.
    CHECK(frame1Psnr >= 40.0);
}

TEST_CASE("CABAC P-frame multi-ref: frames 2-4 PSNR >= 40 dB (regression guard)")
{
    // Guards multi-ref CABAC fixes:
    //   - ref_idx unbounded unary decode (§9.3.3.1.2)
    //   - Immediate refIdx storage for all partition types (§9.3.3.1.1.6)
    //   - I-in-P mb_type contexts 17-20 (§9.3.3.1.2)
    //   - Per-partition reference lookup via DPB L0 list
    // Frames 2-4 use numRefIdxActiveL0 > 1 with ref_pic_list_modification.
    auto h264 = getFixture("scrolling_texture_high.h264");
    auto raw  = getFixture("scrolling_texture_high_raw.yuv");
    if (h264.empty() || raw.empty())
    {
        MESSAGE("scrolling_texture_high fixture not found - skipping");
        return;
    }

    auto decoder = std::make_unique<H264Decoder>();
    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    uint32_t frameCount = 0U;
    double minMultiRefPsnr = 999.0;
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal))
            continue;
        if (decoder->processNal(nal) == DecodeStatus::FrameDecoded)
        {
            // Check frames 2-4 (multi-ref P-frames)
            if (frameCount >= 2U && frameCount <= 4U)
            {
                const Frame* frame = decoder->currentFrame();
                REQUIRE(frame != nullptr);
                uint32_t w = frame->width(), h = frame->height();
                uint32_t lumaSize = w * h;
                uint32_t frameSize = lumaSize + 2U * (w / 2U) * (h / 2U);
                REQUIRE(raw.size() >= (frameCount + 1U) * frameSize);

                const uint8_t* refY = raw.data() + frameCount * frameSize;
                double sse = 0.0;
                for (uint32_t r = 0U; r < h; ++r)
                {
                    const uint8_t* decRow = frame->yRow(r);
                    const uint8_t* refRow = refY + r * w;
                    for (uint32_t c = 0U; c < w; ++c)
                    {
                        double d = static_cast<double>(decRow[c]) - refRow[c];
                        sse += d * d;
                    }
                }
                double mse = sse / lumaSize;
                double psnr = (mse > 0.0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
                MESSAGE("CABAC multi-ref frame " << frameCount << " PSNR: " << psnr << " dB");
                if (psnr < minMultiRefPsnr) minMultiRefPsnr = psnr;
            }
            ++frameCount;
            if (frameCount > 4U)
                break;
        }
    }

    REQUIRE(frameCount > 4U);
    // Frames 2-4 should decode well with multi-ref fixes (currently 52+ dB).
    // Conservative 40 dB threshold catches regressions.
    CHECK(minMultiRefPsnr >= 40.0);
}
