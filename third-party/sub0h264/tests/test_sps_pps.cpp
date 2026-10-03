#include "doctest.h"
#include "../components/sub0h264/src/annexb.hpp"
#include "../components/sub0h264/src/sps.hpp"
#include "../components/sub0h264/src/pps.hpp"
#include "../components/sub0h264/src/param_sets.hpp"

#include "test_fixtures.hpp"

#include <memory>
#include <vector>

using namespace sub0h264;

/** Helper: find and parse the first SPS NAL from a file. */
static bool findAndParseSps(const char* path, Sps& sps)
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
        if (nal.type != NalType::Sps)
            continue;

        BitReader br(nal.rbspData.data(), static_cast<uint32_t>(nal.rbspData.size()));
        return parseSps(br, sps) == Result::Ok;
    }
    return false;
}

/** Helper: parse SPS + PPS from a file into ParamSets. */
static bool parseParamSets(const char* path, ParamSets& ps)
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
                ps.storeSps(sps);
        }
        else if (nal.type == NalType::Pps)
        {
            Pps pps;
            if (parsePps(br, ps.spsArray(), pps) == Result::Ok)
                ps.storePps(pps);
        }
    }
    return true;
}

TEST_CASE("SPS: flat_black_640x480 — Baseline profile")
{
    Sps sps;
    REQUIRE(findAndParseSps("flat_black_baseline_640x480.h264", sps));

    CHECK(sps.valid_);
    CHECK(sps.profileIdc_ == cProfileBaseline);
    CHECK(sps.width() == 640U);
    CHECK(sps.height() == 480U);
    CHECK(sps.widthInMbs_ == 40U);   // 640 / 16
    CHECK(sps.heightInMbs_ == 30U);  // 480 / 16
    CHECK(sps.frameMbsOnly_ == 1U);
    CHECK(sps.numRefFrames_ <= cMaxRefFrames);

    MESSAGE("SPS: profile=" << (int)sps.profileIdc_
            << " level=" << (int)sps.levelIdc_
            << " " << sps.width() << "x" << sps.height()
            << " refs=" << (int)sps.numRefFrames_
            << " poc_type=" << (int)sps.picOrderCntType_);
}

TEST_CASE("SPS: baseline_640x480_short — Baseline profile")
{
    Sps sps;
    REQUIRE(findAndParseSps("baseline_640x480_short.h264", sps));

    CHECK(sps.valid_);
    CHECK(sps.profileIdc_ == cProfileBaseline);
    CHECK(sps.width() == 640U);
    CHECK(sps.height() == 480U);
}

TEST_CASE("PPS: flat_black_640x480 — parse PPS after SPS")
{
    auto ps = std::make_unique<ParamSets>();
    REQUIRE(parseParamSets("flat_black_baseline_640x480.h264", *ps));

    // Should have at least one SPS and one PPS
    const Sps* sps = ps->getSps(0);
    REQUIRE(sps != nullptr);
    CHECK(sps->valid_);

    // Find the first valid PPS
    const Pps* pps = nullptr;
    for (uint8_t i = 0U; i < 255U; ++i)
    {
        pps = ps->getPps(i);
        if (pps)
            break;
    }
    REQUIRE(pps != nullptr);
    CHECK(pps->valid_);

    // Baseline uses CAVLC
    CHECK(pps->entropyCodingMode_ == 0U);
    CHECK_FALSE(pps->isCabac());

    MESSAGE("PPS: id=" << (int)pps->ppsId_
            << " sps_id=" << (int)pps->spsId_
            << " entropy=" << (pps->isCabac() ? "CABAC" : "CAVLC")
            << " qp=" << (int)pps->picInitQp_
            << " deblock=" << (int)pps->deblockingFilterControlPresent_);
}

TEST_CASE("PPS: baseline_640x480_short — CAVLC entropy")
{
    auto ps = std::make_unique<ParamSets>();
    REQUIRE(parseParamSets("baseline_640x480_short.h264", *ps));

    const Pps* pps = nullptr;
    for (uint8_t i = 0U; i < 255U; ++i)
    {
        pps = ps->getPps(i);
        if (pps) break;
    }
    REQUIRE(pps != nullptr);
    CHECK_FALSE(pps->isCabac());
}

TEST_CASE("ParamSets: invalid SPS ID rejected")
{
    Sps sps;
    sps.spsId_ = 32U; // Out of range
    sps.valid_ = true;

    auto ps = std::make_unique<ParamSets>();
    CHECK(ps->storeSps(sps) == Result::ErrorInvalidParam);
}

// FM-13: Default values when conditional fields absent — §7.4.2.1, §7.4.2.2
TEST_CASE("SPS: frameCropping absent → croppedWidth == width (FM-13 §7.3.2.1.1)")
{
    // Baseline stream has no frame_cropping_flag; croppedWidth must equal width.
    // §7.3.2.1.1: frame_crop_* fields absent → defaults to full frame. [CHECKED §7.4.2.1]
    Sps sps;
    REQUIRE(findAndParseSps("flat_black_baseline_640x480.h264", sps));

    CHECK_FALSE(sps.frameCropping_);
    CHECK(sps.croppedWidth() == sps.width());
    CHECK(sps.croppedHeight() == sps.height());
}

TEST_CASE("SPS: derived variables from parsed fields (FM-1 §7.4.2.1)")
{
    // §7.4.2.1: MaxFrameNum = 2^(log2_max_frame_num_minus4 + 4)
    // PicWidthInMbs = pic_width_in_mbs_minus1 + 1, etc. [CHECKED §7.4.2.1]
    Sps sps;
    REQUIRE(findAndParseSps("flat_black_baseline_640x480.h264", sps));

    // widthInMbs_ = pic_width_in_mbs_minus1 + 1 = 640/16 = 40
    CHECK(sps.widthInMbs_ == 40U);
    CHECK(sps.heightInMbs_ == 30U);
    CHECK(sps.totalMbs() == 1200U);           // 40 * 30
    // maxFrameNum = 1 << bitsInFrameNum_; bitsInFrameNum_ >= 4 (log2_max_frame_num_minus4 + 4)
    CHECK(sps.bitsInFrameNum_ >= 4U);
    CHECK(sps.maxFrameNum() == (1U << sps.bitsInFrameNum_));
}

TEST_CASE("PPS: secondChromaQpIndexOffset defaults to chromaQpIndexOffset when absent (FM-13 §7.4.2.2)")
{
    // §7.4.2.2: When second_chroma_qp_index_offset not present (non-High profile),
    // it shall be inferred equal to chroma_qp_index_offset. [CHECKED §7.4.2.2]
    auto ps = std::make_unique<ParamSets>();
    REQUIRE(parseParamSets("baseline_640x480_short.h264", *ps));

    const Pps* pps = nullptr;
    for (uint8_t i = 0U; i < 255U; ++i)
    {
        pps = ps->getPps(i);
        if (pps) break;
    }
    REQUIRE(pps != nullptr);
    // Baseline profile → more_rbsp_data() == false → second offset inherits from first
    CHECK(pps->secondChromaQpIndexOffset_ == pps->chromaQpIndexOffset_);
}

TEST_CASE("PPS: CABAC fixture parses correctly — entropy mode and validity (FM-2 §7.3.2.2)")
{
    // CABAC fixture — more_rbsp_data() behavior depends on profile.
    // §7.3.2.2: PPS must parse without error and entropy_coding_mode_flag=1 (CABAC).
    // [CHECKED §7.3.2.2]
    auto ps = std::make_unique<ParamSets>();
    REQUIRE(parseParamSets("bouncing_ball_main.h264", *ps));

    const Pps* pps = nullptr;
    for (uint8_t i = 0U; i < 255U; ++i)
    {
        pps = ps->getPps(i);
        if (pps) break;
    }
    REQUIRE(pps != nullptr);
    CHECK(pps->isCabac());
    CHECK(pps->valid_);
    // secondChromaQpIndexOffset: valid for both High (parsed) and non-High (inferred).
    // Regardless of profile, the value must be in [-12, 12].
    CHECK(pps->secondChromaQpIndexOffset_ >= -12);
    CHECK(pps->secondChromaQpIndexOffset_ <= 12);
}

TEST_CASE("ParamSets: PPS with missing SPS rejected")
{
    // Create an RBSP for a minimal PPS: pps_id=0, sps_id=5 (doesn't exist)
    // ue(0)=1, ue(5)=00110 → binary: 1_00110 = 0b1001100 = 0x4C shifted
    const uint8_t rbsp[] = { 0b10011000, 0x00 }; // ue(0) + ue(5) + ...

    BitReader br(rbsp, sizeof(rbsp));
    auto ps = std::make_unique<ParamSets>(); // No SPS stored
    Pps pps;
    CHECK(parsePps(br, ps->spsArray(), pps) == Result::ErrorInvalidParam);
}
