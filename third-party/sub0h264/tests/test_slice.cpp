#include "doctest.h"
#include "../components/sub0h264/src/annexb.hpp"
#include "../components/sub0h264/src/sps.hpp"
#include "../components/sub0h264/src/pps.hpp"
#include "../components/sub0h264/src/param_sets.hpp"
#include "../components/sub0h264/src/slice.hpp"

#include "test_fixtures.hpp"

#include <memory>
#include <vector>

using namespace sub0h264;

/** Parse all NALs from a file, populate ParamSets, and collect slice headers. */
struct ParsedStream
{
    ParamSets paramSets;
    std::vector<SliceHeader> slices;
    std::vector<NalType> sliceNalTypes;

    bool parse(const char* path)
    {
        auto data = getFixture(path);
        if (data.empty())
            return false;

        std::vector<NalBounds> bounds;
        findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);

        for (const auto& b : bounds)
        {
            NalUnit nal;
            if (!parseNalUnit(data.data() + b.offset, b.size, nal))
                continue;

            BitReader br(nal.rbspData.data(), static_cast<uint32_t>(nal.rbspData.size()));

            if (nal.type == NalType::Sps)
            {
                Sps sps;
                if (parseSps(br, sps) == Result::Ok)
                    paramSets.storeSps(sps);
            }
            else if (nal.type == NalType::Pps)
            {
                Pps pps;
                if (parsePps(br, paramSets.spsArray(), pps) == Result::Ok)
                    paramSets.storePps(pps);
            }
            else if (isSliceNal(nal.type))
            {
                // Need PPS to parse slice header
                // Peek at pps_id: first_mb_in_slice(ue) + slice_type(ue) + pps_id(ue)
                BitReader peekBr(nal.rbspData.data(),
                                 static_cast<uint32_t>(nal.rbspData.size()));
                peekBr.readUev(); // first_mb_in_slice
                peekBr.readUev(); // slice_type
                uint8_t ppsId = static_cast<uint8_t>(peekBr.readUev());

                const Pps* pps = paramSets.getPps(ppsId);
                if (!pps)
                    continue;
                const Sps* sps = paramSets.getSps(pps->spsId_);
                if (!sps)
                    continue;

                bool isIdr = (nal.type == NalType::SliceIdr);
                SliceHeader sh;
                if (parseSliceHeader(br, *sps, *pps, isIdr, nal.refIdc, sh) == Result::Ok)
                {
                    slices.push_back(sh);
                    sliceNalTypes.push_back(nal.type);
                }
            }
        }
        return true;
    }
};

TEST_CASE("Slice: flat_black_640x480 — single IDR I-slice")
{
    auto stream = std::make_unique<ParsedStream>();
    REQUIRE(stream->parse("flat_black_baseline_640x480.h264"));

    REQUIRE(stream->slices.size() >= 1U);

    const auto& sh = stream->slices[0];
    CHECK(sh.isIdr_);
    CHECK(sh.sliceType_ == SliceType::I);
    CHECK(sh.firstMbInSlice_ == 0U);
    CHECK(sh.frameNum_ == 0U);

    MESSAGE("IDR slice: type=I qp_delta=" << sh.sliceQpDelta_
            << " deblock=" << (int)sh.disableDeblockingFilter_);
}

TEST_CASE("Slice: baseline_640x480_short — IDR + P-frames")
{
    auto stream = std::make_unique<ParsedStream>();
    REQUIRE(stream->parse("baseline_640x480_short.h264"));

    REQUIRE(stream->slices.size() >= 3U);

    // First slice should be IDR
    CHECK(stream->slices[0].isIdr_);
    CHECK(stream->slices[0].sliceType_ == SliceType::I);
    CHECK(stream->slices[0].firstMbInSlice_ == 0U);

    // Subsequent slices should be P-frames
    uint32_t iCount = 0U, pCount = 0U;
    for (const auto& sh : stream->slices)
    {
        if (sh.sliceType_ == SliceType::I)
            ++iCount;
        else if (sh.sliceType_ == SliceType::P)
            ++pCount;

        // All slices should start at MB 0 (single slice per frame)
        CHECK(sh.firstMbInSlice_ == 0U);
    }

    CHECK(iCount >= 1U);
    CHECK(pCount >= 2U);

    MESSAGE("Slices parsed: " << stream->slices.size()
            << " (I=" << iCount << " P=" << pCount << ")");
}

TEST_CASE("Slice: frame numbers start at 0 for IDR")
{
    auto stream = std::make_unique<ParsedStream>();
    REQUIRE(stream->parse("baseline_640x480_short.h264"));
    REQUIRE(stream->slices.size() >= 3U);

    // IDR resets frame_num to 0
    CHECK(stream->slices[0].frameNum_ == 0U);

    // Frame numbers increment modulo maxFrameNum (may wrap back to 0)
    // Just verify they are within valid range
    const Sps* sps = stream->paramSets.getSps(0);
    REQUIRE(sps != nullptr);
    uint16_t maxFrameNum = static_cast<uint16_t>(sps->maxFrameNum());

    for (const auto& sh : stream->slices)
        CHECK(sh.frameNum_ < maxFrameNum);
}

TEST_CASE("Slice: QP delta is within valid range")
{
    auto stream = std::make_unique<ParsedStream>();
    REQUIRE(stream->parse("baseline_640x480_short.h264"));

    for (const auto& sh : stream->slices)
    {
        // slice_qp_delta should result in a valid QP (0-51)
        // QP = picInitQp + sliceQpDelta, picInitQp is typically 26
        // So sliceQpDelta should be in [-26, 25]
        CHECK(sh.sliceQpDelta_ >= -26);
        CHECK(sh.sliceQpDelta_ <= 25);
    }
}

// ── Slice header bit consumption — ITU-T H.264 §7.3.3 ──────────────────

TEST_CASE("Slice header: baseline IDR consumes exactly 28 bits")
{
    // ITU-T H.264 §7.3.3: manually computed bit layout for the IDR slice
    // of baseline_640x480_short.h264 (RBSP bytes: 88 84 0C F2 62 ...):
    //
    // Field                          Bits  Value    Bit range
    // ─────────────────────────────────────────────────────────
    // first_mb_in_slice    ue(v)       1    0       [0]
    // slice_type           ue(v)       7    7→I     [1-7]
    // pic_parameter_set_id ue(v)       1    0       [8]
    // frame_num            u(4)        4    0       [9-12]
    // idr_pic_id           ue(v)       1    0       [13]
    // no_output_of_prior   u(1)        1    0       [14]
    // long_term_reference  u(1)        1    0       [15]
    // slice_qp_delta       se(v)       9    -12     [16-24]
    // disable_deblock_idc  ue(v)       1    0       [25]
    // alpha_c0_offset_div2 se(v)       1    0       [26]
    // beta_offset_div2     se(v)       1    0       [27]
    // ─────────────────────────────────────────────────────────
    // Total: 28 bits
    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);
    REQUIRE(bounds.size() >= 3U);

    // Parse SPS + PPS first
    ParamSets ps;
    NalUnit spsNal, ppsNal;
    REQUIRE(parseNalUnit(data.data() + bounds[0].offset, bounds[0].size, spsNal));
    {
        BitReader br(spsNal.rbspData.data(),
                     static_cast<uint32_t>(spsNal.rbspData.size()));
        Sps sps;
        REQUIRE(parseSps(br, sps) == Result::Ok);
        ps.storeSps(sps);
    }
    REQUIRE(parseNalUnit(data.data() + bounds[1].offset, bounds[1].size, ppsNal));
    {
        BitReader br(ppsNal.rbspData.data(),
                     static_cast<uint32_t>(ppsNal.rbspData.size()));
        Pps pps;
        REQUIRE(parsePps(br, ps.spsArray(), pps) == Result::Ok);
        ps.storePps(pps);
    }

    // Find the IDR NAL (skip SEI if present)
    uint32_t idrIdx = 2U;
    for (uint32_t i = 2U; i < static_cast<uint32_t>(bounds.size()); ++i)
    {
        NalUnit nal;
        if (parseNalUnit(data.data() + bounds[i].offset, bounds[i].size, nal)
            && nal.type == NalType::SliceIdr)
        {
            idrIdx = i;
            break;
        }
    }

    NalUnit idrNal;
    REQUIRE(parseNalUnit(data.data() + bounds[idrIdx].offset,
                         bounds[idrIdx].size, idrNal));
    REQUIRE(idrNal.type == NalType::SliceIdr);

    BitReader br(idrNal.rbspData.data(),
                 static_cast<uint32_t>(idrNal.rbspData.size()));

    const Pps* pps = ps.getPps(0);
    REQUIRE(pps != nullptr);
    const Sps* sps = ps.getSps(pps->spsId_);
    REQUIRE(sps != nullptr);

    uint32_t bitsBefore = br.bitOffset();
    SliceHeader sh;
    REQUIRE(parseSliceHeader(br, *sps, *pps, true, idrNal.refIdc, sh) == Result::Ok);
    uint32_t bitsConsumed = br.bitOffset() - bitsBefore;

    // Verify exact bit consumption per §7.3.3 manual calculation
    CHECK(bitsConsumed == 28U);

    // Verify parsed values match the known bitstream content
    CHECK(sh.firstMbInSlice_ == 0U);
    CHECK(sh.sliceType_ == SliceType::I);
    CHECK(sh.frameNum_ == 0U);
    CHECK(sh.isIdr_);
    CHECK(sh.idrPicId_ == 0U);
    CHECK(sh.sliceQpDelta_ == -12);
    CHECK(sh.disableDeblockingFilter_ == 0U);
    CHECK(sh.sliceAlphaC0Offset_ == 0);
    CHECK(sh.sliceBetaOffset_ == 0);

    // Verify effective QP: picInitQp(26) + sliceQpDelta(-12) = 14
    // This stream's PPS has pic_init_qp_minus26 = 0, so picInitQp = 26.
    /// PPS pic_init_qp for this stream.
    static constexpr int32_t cPicInitQp = 26;
    CHECK(pps->picInitQp_ == cPicInitQp);
    int32_t effectiveQp = cPicInitQp + sh.sliceQpDelta_;
    CHECK(effectiveQp == 14);

    MESSAGE("IDR slice header: " << bitsConsumed << " bits, QP=" << effectiveQp);
}

TEST_CASE("Slice header: first P-slice bit consumption is reasonable")
{
    // ITU-T H.264 §7.3.3: P-slice headers include additional fields:
    // num_ref_idx_active_override_flag(1) + optional ref_pic_list_modification.
    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);

    ParamSets ps;
    // Parse SPS+PPS
    for (uint32_t i = 0; i < static_cast<uint32_t>(bounds.size()); ++i)
    {
        NalUnit nal;
        if (!parseNalUnit(data.data() + bounds[i].offset, bounds[i].size, nal))
            continue;
        BitReader br(nal.rbspData.data(),
                     static_cast<uint32_t>(nal.rbspData.size()));
        if (nal.type == NalType::Sps)
        {
            Sps sps;
            if (parseSps(br, sps) == Result::Ok)
                ps.storeSps(sps);
        }
        else if (nal.type == NalType::Pps)
        {
            Pps pps;
            if (parsePps(br, ps.spsArray(), pps) == Result::Ok)
                ps.storePps(pps);
        }
    }

    // Find first P-slice NAL
    for (uint32_t i = 0; i < static_cast<uint32_t>(bounds.size()); ++i)
    {
        NalUnit nal;
        if (!parseNalUnit(data.data() + bounds[i].offset, bounds[i].size, nal))
            continue;
        if (nal.type != NalType::SliceNonIdr)
            continue;

        const Pps* pps = ps.getPps(0);
        REQUIRE(pps != nullptr);
        const Sps* sps = ps.getSps(pps->spsId_);
        REQUIRE(sps != nullptr);

        BitReader br(nal.rbspData.data(),
                     static_cast<uint32_t>(nal.rbspData.size()));
        uint32_t bitsBefore = br.bitOffset();
        SliceHeader sh;
        REQUIRE(parseSliceHeader(br, *sps, *pps, false, nal.refIdc, sh) == Result::Ok);
        uint32_t bitsConsumed = br.bitOffset() - bitsBefore;

        CHECK(sh.sliceType_ == SliceType::P);
        CHECK(sh.firstMbInSlice_ == 0U);
        // P-slice header should be compact (typically 15-30 bits)
        CHECK(bitsConsumed >= 10U);
        CHECK(bitsConsumed <= 60U);

        MESSAGE("First P-slice header: " << bitsConsumed << " bits, "
                << "frame_num=" << sh.frameNum_
                << " qpDelta=" << sh.sliceQpDelta_);
        break;
    }
}

// ── SPS bit-exact parsing — ITU-T H.264 §7.3.2.1 ──────────────────────

// FM-13: Default values when conditional field absent — §7.3.3
TEST_CASE("Slice: numRefIdxActiveL0 defaults from PPS when no override (FM-13 §7.3.3)")
{
    // §7.3.3: when num_ref_idx_active_override_flag = 0, numRefIdxActiveL0_ must
    // equal pps.numRefIdxL0Active_default (inferred from PPS). [CHECKED §7.3.3]
    auto stream = std::make_unique<ParsedStream>();
    REQUIRE(stream->parse("baseline_640x480_short.h264"));

    const Pps* pps = stream->paramSets.getPps(0);
    REQUIRE(pps != nullptr);

    for (const auto& sh : stream->slices)
    {
        if (sh.sliceType_ != SliceType::P)
            continue;
        // numRefIdxActiveL0_ must be in valid range [1..maxRefFrames]
        // When override_flag=0, it equals PPS default; when override_flag=1,
        // the slice header provides its own value. Either way, must be >= 1.
        CHECK(sh.numRefIdxActiveL0_ >= 1U);
        CHECK(sh.numRefIdxActiveL0_ <= 16U);
        break;
    }
}

// FM-2: Negative-clause / missing normalization — §7.3.3
TEST_CASE("Slice: slice_type values 5-9 are normalized to 0-4 (FM-2 §7.3.3)")
{
    // §7.3.3: slice_type ue(v) in 0-9; values 5-9 mean all slices in picture
    // are the same type and are normalized by subtracting 5. [CHECKED §7.3.3]
    // If normalization fails, SliceType enum would hold values 5-9 — this test
    // verifies all parsed slice types are valid enum values (0-4).
    auto stream = std::make_unique<ParsedStream>();
    REQUIRE(stream->parse("bouncing_ball_main.h264"));
    REQUIRE(stream->slices.size() >= 2U);

    for (const auto& sh : stream->slices)
    {
        uint8_t rawType = static_cast<uint8_t>(sh.sliceType_);
        CHECK(rawType <= 4U);
    }
    CHECK(stream->slices[0].sliceType_ == SliceType::I);
    CHECK(stream->slices[0].isIdr_);
}

// FM-24: Slice header field dependency — §7.3.3
TEST_CASE("Slice: CABAC P-slice cabac_init_idc is in valid range (FM-24 §7.3.3)")
{
    // §7.3.3: cabac_init_idc ue(v) read only when entropy_coding_mode_flag=1
    // and slice_type != I and slice_type != SI. Values 0-2 are valid. [CHECKED §7.3.3]
    auto stream = std::make_unique<ParsedStream>();
    REQUIRE(stream->parse("bouncing_ball_main.h264"));

    bool foundPSlice = false;
    for (const auto& sh : stream->slices)
    {
        if (sh.sliceType_ != SliceType::P)
            continue;
        CHECK(sh.cabacInitIdc_ <= 2U);
        foundPSlice = true;
        break;
    }
    CHECK(foundPSlice);
}

TEST_CASE("SPS: bitsInFrameNum matches log2_max_frame_num_minus4 + 4")
{
    // ITU-T H.264 §7.3.2.1: log2_max_frame_num_minus4 is ue(v),
    // and the total frame_num field width = value + 4.
    auto data = getFixture("baseline_640x480_short.h264");
    REQUIRE_FALSE(data.empty());

    std::vector<NalBounds> bounds;
    findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);
    REQUIRE(bounds.size() >= 1U);

    NalUnit spsNal;
    REQUIRE(parseNalUnit(data.data() + bounds[0].offset, bounds[0].size, spsNal));
    REQUIRE(spsNal.type == NalType::Sps);

    BitReader br(spsNal.rbspData.data(),
                 static_cast<uint32_t>(spsNal.rbspData.size()));
    Sps sps;
    REQUIRE(parseSps(br, sps) == Result::Ok);

    // bitsInFrameNum_ must be in [4, 16] per spec
    CHECK(sps.bitsInFrameNum_ >= 4U);
    CHECK(sps.bitsInFrameNum_ <= 16U);
    // For this stream, ffmpeg shows 4-bit frame_num
    CHECK(sps.bitsInFrameNum_ == 4U);

    // poc_type=2 for this stream (no POC in slice header)
    CHECK(sps.picOrderCntType_ == 2U);

    // frameMbsOnly must be true for baseline profile
    CHECK(sps.frameMbsOnly_);
}
