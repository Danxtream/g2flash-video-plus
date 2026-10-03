/** Sub0h264 — Bitstream trace test
 *
 *  Uses our decoder's low-level CAVLC functions to independently parse
 *  MB(9,0) residual blocks from the raw RBSP, comparing per-block bit
 *  offsets against the full decoder's consumption.
 *
 *  NOTE: These tests validate against libavc reference values, not
 *  directly against the spec. They serve as cross-decoder consistency
 *  checks. The coefficient values and bit offsets are fixture-specific.
 *
 *  Reference: ITU-T H.264 §9.2, libavc ih264d_parse_cavlc.c
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "test_fixtures.hpp"
#include "../components/sub0h264/src/decoder.hpp"

#include <memory>
#include <vector>

using namespace sub0h264;

/** Decode one CAVLC 4x4 block using the public decodeResidualBlock4x4,
 *  and return the bit consumption. */
static uint32_t decodeBlockBits(BitReader& br, int32_t nC,
                                uint32_t maxCoeff, uint32_t startIdx,
                                ResidualBlock4x4& block)
{
    uint32_t before = br.bitOffset();
    decodeResidualBlock4x4(br, nC, maxCoeff, startIdx, block);
    return br.bitOffset() - before;
}

TEST_CASE("MB(9,0) full residual trace: find 3-bit over-consumption"
          * doctest::skip(true))  // Skip: diagnostic test with wrong nC, reads past RBSP on ESP32
{
    // MB(9,0) is the first I_4x4 in baseline_640x480_short IDR.
    // Full decoder consumes 303 bits (240→543).
    // MB(10,0) should start at bit 540 per RBSP analysis.
    // Total should be 300 bits → 3-bit over-consumption.
    //
    // This test decodes each block independently using our CAVLC functions,
    // starting from the known-good residual start at bit 284.
    //
    // If the standalone decode matches the full decoder (303 bits),
    // the bug is in CAVLC. If it produces 300 bits, the bug is in how
    // the full decoder calls CAVLC (e.g., wrong nC context).

    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    // Extract IDR RBSP
    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);
    NalUnit idrNal;
    for (const auto& b : bounds)
        if (parseNalUnit(data.data() + b.offset, b.size, idrNal)
            && idrNal.type == NalType::SliceIdr)
            break;
    REQUIRE(idrNal.type == NalType::SliceIdr);

    BitReader br(idrNal.rbspData.data(),
                 static_cast<uint32_t>(idrNal.rbspData.size()));

    // ── Header (bits 240→284) — verified correct ──────────────────────
    // Skip to bit 240 (MB(9,0) start, after mb_type already consumed by caller)
    br.skipBits(241U); // Past mb_type (1 bit at 240)

    // Skip 16 prediction modes (22 bits: 241→263)
    br.skipBits(22U);
    CHECK(br.bitOffset() == 263U);

    // Skip chroma pred mode (1 bit: 263→264)
    br.skipBits(1U);

    // Skip cbp code (11 bits: 264→275)
    br.skipBits(11U);

    // Skip qp delta (9 bits: 275→284)
    br.skipBits(9U);
    CHECK(br.bitOffset() == 284U);

    // ── Luma residual (cbpLuma=8 → group 3 only, scans 12-15) ────────
    MESSAGE("=== MB(9,0) residual decode from bit 284 ===");

    // scans 0-11 are uncoded (groups 0-2 have cbpLuma=0)
    // scan12 → raster10 (row=2, col=2), nC=0
    ResidualBlock4x4 blk12;
    uint32_t bits12 = decodeBlockBits(br, 0, cMaxCoeff4x4, 0U, blk12);
    MESSAGE("scan12: tc=" << blk12.totalCoeff << " " << bits12
            << "b (284->" << br.bitOffset() << ")");

    // scan13 → raster11 (row=2, col=3), nC = avg(left=raster10, top=0)
    // left=raster10 tc=0, top neighbor from group1 (uncoded)=0 → nC=0
    int32_t nc13 = (blk12.totalCoeff + 0 + 1) >> 1; // (left=tc12, top=uncoded=0)
    ResidualBlock4x4 blk13;
    uint32_t bits13 = decodeBlockBits(br, nc13, cMaxCoeff4x4, 0U, blk13);
    MESSAGE("scan13: nC=" << nc13 << " tc=" << blk13.totalCoeff << " "
            << bits13 << "b (" << (br.bitOffset()-bits13) << "->" << br.bitOffset() << ")");

    // scan14 → raster14 (row=3, col=2), nC = avg(left=raster13[uncoded]=0, top=raster10=tc12)
    // raster13 is in group 2 (uncoded), raster10 was scan12 (tc=0)
    int32_t nc14 = (0 + blk12.totalCoeff + 1) >> 1;
    ResidualBlock4x4 blk14;
    uint32_t bits14 = decodeBlockBits(br, nc14, cMaxCoeff4x4, 0U, blk14);
    MESSAGE("scan14: nC=" << nc14 << " tc=" << blk14.totalCoeff << " "
            << bits14 << "b (" << (br.bitOffset()-bits14) << "->" << br.bitOffset() << ")");

    // scan15 → raster15 (row=3, col=3), nC = avg(left=raster14=tc14, top=raster11=tc13)
    int32_t nc15 = (blk14.totalCoeff + blk13.totalCoeff + 1) >> 1;
    ResidualBlock4x4 blk15;
    uint32_t bits15 = decodeBlockBits(br, nc15, cMaxCoeff4x4, 0U, blk15);
    MESSAGE("scan15: nC=" << nc15 << " tc=" << blk15.totalCoeff << " "
            << bits15 << "b (" << (br.bitOffset()-bits15) << "->" << br.bitOffset() << ")");

    uint32_t lumaTotal = bits12 + bits13 + bits14 + bits15;
    MESSAGE("Luma total: " << lumaTotal << "b (284->" << br.bitOffset() << ")");

    // ── Chroma DC (cbpChroma=2 → DC present) ─────────────────────────
    // Cb DC: nC=-1 (chroma DC table)
    ResidualBlock4x4 dcCb;
    uint32_t bitsDcCb = decodeBlockBits(br, -1, 4U, 0U, dcCb);
    MESSAGE("chromaDC Cb: tc=" << dcCb.totalCoeff << " " << bitsDcCb
            << "b (" << (br.bitOffset()-bitsDcCb) << "->" << br.bitOffset() << ")");

    // Cr DC: nC=-1
    ResidualBlock4x4 dcCr;
    uint32_t bitsDcCr = decodeBlockBits(br, -1, 4U, 0U, dcCr);
    MESSAGE("chromaDC Cr: tc=" << dcCr.totalCoeff << " " << bitsDcCr
            << "b (" << (br.bitOffset()-bitsDcCr) << "->" << br.bitOffset() << ")");

    // ── Chroma AC (cbpChroma=2 → AC present, all Cb then all Cr) ─────
    // All 4 Cb AC blocks (nC from chroma neighbors, all 0 for first I_4x4)
    uint32_t chromaAcTotal = 0U;
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        ResidualBlock4x4 acBlk;
        uint32_t acBits = decodeBlockBits(br, 0, 15U, 1U, acBlk);
        chromaAcTotal += acBits;
        if (acBlk.totalCoeff > 0U)
            MESSAGE("chromaAC Cb[" << i << "]: nC=0 tc=" << acBlk.totalCoeff
                    << " " << acBits << "b");
    }

    // All 4 Cr AC blocks — track NNZ for correct nC context
    // Chroma 2x2 block layout: [0]=TL [1]=TR [2]=BL [3]=BR
    uint8_t crNnz[4] = {};
    for (uint32_t i = 0U; i < 4U; ++i)
    {
        // Compute nC from chroma neighbors (same logic as getChromaNc)
        uint32_t blkX = i & 1U;
        uint32_t blkY = i >> 1U;
        int32_t leftNnz = 0, topNnz = 0;
        bool hasLeft = false, hasTop = false;

        if (blkX > 0U) { leftNnz = crNnz[i - 1U]; hasLeft = true; }
        // For MB(9,0), left MB Cr AC nnz = 0 (MB(8,0) is I_16x16, cbpC=0)
        else { leftNnz = 0; hasLeft = true; }

        if (blkY > 0U) { topNnz = crNnz[i - 2U]; hasTop = true; }
        // For MB(9,0), top MB = none (mbY=0)
        else if (false) { /* mbY > 0 */ }

        int32_t ncCr = 0;
        if (hasLeft && hasTop) ncCr = (leftNnz + topNnz + 1) >> 1;
        else if (hasLeft) ncCr = leftNnz;
        else if (hasTop) ncCr = topNnz;

        ResidualBlock4x4 acBlk;
        uint32_t acBits = decodeBlockBits(br, ncCr, 15U, 1U, acBlk);
        crNnz[i] = acBlk.totalCoeff;
        chromaAcTotal += acBits;
        MESSAGE("chromaAC Cr[" << i << "]: nC=" << ncCr << " tc="
                << acBlk.totalCoeff << " " << acBits << "b ("
                << (br.bitOffset()-acBits) << "->" << br.bitOffset() << ")");
    }
    MESSAGE("chromaAC total: " << chromaAcTotal << "b");

    uint32_t chromaTotal = bitsDcCb + bitsDcCr + chromaAcTotal;
    MESSAGE("Chroma total: " << chromaTotal << "b");

    uint32_t residualTotal = lumaTotal + chromaTotal;
    uint32_t mbTotal = 44U + residualTotal; // 44 = header bits

    MESSAGE("=== TOTAL: header(44) + residual(" << residualTotal
            << ") = " << mbTotal << " bits ===");
    MESSAGE("Full decoder reports: 303 bits");
    MESSAGE("Expected (MB10 at bit 540): 300 bits");
    MESSAGE("Standalone: " << mbTotal << " bits → "
            << (mbTotal == 300U ? "MATCHES expected"
                : (mbTotal == 303U ? "matches full decoder (bug in CAVLC)"
                   : "DIFFERENT from both!")));

    // If this is 300, the bug is in the full decoder's nC context.
    // If this is 303, the bug is in our CAVLC functions.
    // If something else, we have a different issue.

    // The Cr AC blocks might have wrong nC (we used 0 for all, but the
    // full decoder computes nC from actual Cr neighbors).
    // The nC difference could cause different coeff_token decoding → different bits.
}

TEST_CASE("Chroma DC dequant: MB(5,0) matches libavc values via trace callback")
{
    // Use the trace callback to capture chroma DC dequant values from
    // MB(5,0) and compare against libavc's known-good output.
    //
    // libavc trace for MB(5,0):
    //   Cb raw=[-121,0,0,0] scale_u=640 → dequant=[-2420,-2420,-2420,-2420]
    //   (Cr not traced but follows same formula)
    //
    // Reference: libavc ih264d_cavlc_parse_chroma_dc, line 2678+1397.

    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    auto decoder = std::make_unique<H264Decoder>();

    // Capture chroma DC events for MB(5,0)
    int16_t capturedCb[4] = {};
    int16_t capturedCr[4] = {};
    bool cbCaptured = false, crCaptured = false;

    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.type == TraceEventType::ChromaDcDequant &&
            e.mbX == 5 && e.mbY == 0 && e.data)
        {
            if (e.a == 0) { // Cb
                for (uint32_t i = 0; i < 4; ++i) capturedCb[i] = e.data[i];
                cbCaptured = true;
            } else { // Cr
                for (uint32_t i = 0; i < 4; ++i) capturedCr[i] = e.data[i];
                crCaptured = true;
            }
        }
    });

    decoder->decodeStream(data.data(), static_cast<uint32_t>(data.size()));

    REQUIRE(cbCaptured);
    REQUIRE(crCaptured);

    // Verify against libavc's output — ITU-T H.264 §8.5.12.1 chroma DC dequant.
    // libavc: Cb dequant = [-2420, -2420, -2420, -2420]
    // Formula: (f * LevelScale * 16 << qpDiv6) >> 5 = (f * 10 * 16 << 2) >> 5 = f * 20
    // Hadamard of [-121,0,0,0] = [-121,-121,-121,-121] → dequant = -121 * 20 = -2420
    CHECK(capturedCb[0] == static_cast<int16_t>(-2420));
    CHECK(capturedCb[1] == static_cast<int16_t>(-2420));
    CHECK(capturedCb[2] == static_cast<int16_t>(-2420));
    CHECK(capturedCb[3] == static_cast<int16_t>(-2420));

    // Cr: raw=[357,0,0,0] → Hadamard=[357,357,357,357] → dequant = 357 * 20 = 7140
    CHECK(capturedCr[0] == static_cast<int16_t>(7140));

    MESSAGE("Chroma DC dequant values match libavc reference");
}

TEST_CASE("Chroma DC raw: MB(9,0) CAVLC coefficients match libavc")
{
    // libavc trace for MB(9,0) (chromaQp=22, second chroma DC call):
    //   Cb raw=[5,-5,-5,5] scale_u=2048
    //   Cr raw=[-15,15,15,-15] scale_v=2048

    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    auto decoder = std::make_unique<H264Decoder>();

    int16_t rawCb9[4] = {}, rawCr9[4] = {};
    bool cbRaw = false, crRaw = false;

    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.type == TraceEventType::ChromaDcRaw &&
            e.mbX == 9 && e.mbY == 0 && e.data)
        {
            if (e.a == 0) { for (int i=0;i<4;i++) rawCb9[i]=e.data[i]; cbRaw=true; }
            if (e.a == 1) { for (int i=0;i<4;i++) rawCr9[i]=e.data[i]; crRaw=true; }
        }
    });

    decoder->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
    REQUIRE(cbRaw);
    REQUIRE(crRaw);

    // Compare against libavc's raw CAVLC decode
    CHECK(rawCb9[0] == 5);
    CHECK(rawCb9[1] == -5);
    CHECK(rawCb9[2] == -5);
    CHECK(rawCb9[3] == 5);

    CHECK(rawCr9[0] == -15);
    CHECK(rawCr9[1] == 15);
    CHECK(rawCr9[2] == 15);
    CHECK(rawCr9[3] == -15);

    MESSAGE("Cb raw: [" << rawCb9[0] << "," << rawCb9[1] << "," << rawCb9[2] << "," << rawCb9[3] << "]");
    MESSAGE("Cr raw: [" << rawCr9[0] << "," << rawCr9[1] << "," << rawCr9[2] << "," << rawCr9[3] << "]");
}

TEST_CASE("Chroma DC: MB(30,1) raw coefficients via trace callback")
{
    // MB(30,1) has a -56 chroma U error at interior pixels.
    // Capture raw and dequanted DC to check for reconstruction bugs.
    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    auto decoder = std::make_unique<H264Decoder>();

    int16_t rawCb[4] = {}, rawCr[4] = {}, dqCb[4] = {}, dqCr[4] = {};
    bool gotRaw = false, gotDq = false;

    decoder->trace().setCallback([&](const TraceEvent& e) {
        if (e.mbX == 30 && e.mbY == 1 && e.data) {
            if (e.type == TraceEventType::ChromaDcRaw && e.a == 0) {
                for (int i=0;i<4;i++) rawCb[i]=e.data[i];
                gotRaw = true;
            }
            if (e.type == TraceEventType::ChromaDcRaw && e.a == 1) {
                for (int i=0;i<4;i++) rawCr[i]=e.data[i];
            }
            if (e.type == TraceEventType::ChromaDcDequant && e.a == 0) {
                for (int i=0;i<4;i++) dqCb[i]=e.data[i];
                gotDq = true;
            }
            if (e.type == TraceEventType::ChromaDcDequant && e.a == 1) {
                for (int i=0;i<4;i++) dqCr[i]=e.data[i];
            }
        }
    });

    decoder->decodeStream(data.data(), static_cast<uint32_t>(data.size()));

    if (gotRaw)
        MESSAGE("MB(30,1) Cb raw: [" << rawCb[0] << "," << rawCb[1] << "," << rawCb[2] << "," << rawCb[3] << "]"
                << "  Cr raw: [" << rawCr[0] << "," << rawCr[1] << "," << rawCr[2] << "," << rawCr[3] << "]");
    if (gotDq)
        MESSAGE("MB(30,1) Cb dq:  [" << dqCb[0] << "," << dqCb[1] << "," << dqCb[2] << "," << dqCb[3] << "]"
                << "  Cr dq:  [" << dqCr[0] << "," << dqCr[1] << "," << dqCr[2] << "," << dqCr[3] << "]");
}

TEST_CASE("CAVLC coeff_token table: no ambiguous VLC codes per Table 9-5")
{
    // ITU-T H.264 Table 9-5: each (totalCoeff, trailingOnes) pair has a
    // unique VLC code at its specific length. If two entries have the same
    // code AND length, matchCoeffTokenTable picks the first match, which
    // could be wrong.
    //
    // Reference: libavc ih264d_parse_cavlc.c uses pre-built decode trees.

    for (uint32_t tableIdx = 0U; tableIdx < 3U; ++tableIdx)
    {
        struct Entry { uint32_t tc; uint32_t t1; uint8_t size; uint16_t code; };
        std::vector<Entry> entries;

        for (uint32_t t1 = 0U; t1 < 4U; ++t1)
            for (uint32_t tc = 0U; tc <= 16U; ++tc)
            {
                if (t1 > tc) continue;
                uint8_t sz = cCoeffTokenSize[tableIdx][t1][tc];
                if (sz == 0U || sz > 16U) continue;
                entries.push_back({tc, t1, sz, cCoeffTokenCode[tableIdx][t1][tc]});
            }

        // Check for duplicate (code, size) pairs
        uint32_t dupes = 0U;
        for (size_t i = 0; i < entries.size(); ++i)
            for (size_t j = i + 1; j < entries.size(); ++j)
                if (entries[i].size == entries[j].size &&
                    entries[i].code == entries[j].code)
                {
                    ++dupes;
                    MESSAGE("Table " << tableIdx << " DUPE: tc=" << entries[i].tc
                            << ",t1=" << entries[i].t1 << " vs tc=" << entries[j].tc
                            << ",t1=" << entries[j].t1 << " code=" << entries[i].code
                            << " size=" << (int)entries[i].size);
                }
        CHECK(dupes == 0U);

        // Check prefix-free: no shorter code is a prefix of a longer one
        uint32_t prefixViolations = 0U;
        for (size_t i = 0; i < entries.size(); ++i)
            for (size_t j = 0; j < entries.size(); ++j)
            {
                if (i == j || entries[i].size >= entries[j].size) continue;
                uint32_t shift = entries[j].size - entries[i].size;
                if ((entries[j].code >> shift) == entries[i].code)
                    ++prefixViolations;
            }
        CHECK(prefixViolations == 0U);

        MESSAGE("Table " << tableIdx << ": " << entries.size()
                << " entries, " << dupes << " dupes, "
                << prefixViolations << " prefix violations");
    }
}
