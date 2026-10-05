/** Runtime chroma skipping must preserve luma and entropy state.
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "test_fixtures.hpp"
#include "skip_chroma_compare.hpp"

#include <algorithm>
#ifndef ESP_PLATFORM
#include <filesystem>
#endif

using namespace sub0h264;

namespace {
uint32_t gPlaneRequests = 0U;
bool allowOnlyLuma(const AllocationRequest* requests, size_t count,
                   AllocationFailure*) noexcept
{
    gPlaneRequests += static_cast<uint32_t>(count);
    return count == 1U && requests[0].tag == AllocationTag::FrameY;
}
struct PreflightReset
{
    ~PreflightReset() { setAllocationPreflightForTesting(nullptr); }
};

void checkComparison(const test::ChromaComparison& result)
{
    CHECK(result.outcomeDifferences == 0U);
    CHECK(result.yMismatches == 0U);
    CHECK(result.storageErrors == 0U);
    CHECK(result.syntaxDifferences == 0U);
}
} // namespace

TEST_CASE("Skip chroma: construction setting defaults off")
{
    auto defaults = std::make_unique<H264Decoder>();
    auto off = std::make_unique<H264Decoder>(H264Decoder::Settings{false});
    auto on = std::make_unique<H264Decoder>(H264Decoder::Settings{true});
    CHECK_FALSE(defaults->skipChroma());
    CHECK_FALSE(off->skipChroma());
    CHECK(on->skipChroma());
}

TEST_CASE("Skip chroma: explicit off matches default Y/U/V output")
{
    for (const char* name : {"scrolling_texture_baseline.h264", "scrolling_texture_high.h264"})
    {
        INFO(name);
        const auto data = getFixture(name);
        REQUIRE_FALSE(data.empty());
        auto defaults = std::make_unique<H264Decoder>();
        auto off = std::make_unique<H264Decoder>(H264Decoder::Settings{false});
        std::vector<NalBounds> bounds;
        findNalUnits(data.data(), static_cast<uint32_t>(data.size()), bounds);
        for (const auto& b : bounds)
        {
            NalUnit nal;
            REQUIRE(parseNalUnit(data.data() + b.offset, b.size, nal));
            const auto status = defaults->processNal(nal);
            REQUIRE(off->processNal(nal) == status);
            if (status != DecodeStatus::FrameDecoded) continue;
            const Frame* a = defaults->currentFrame();
            const Frame* z = off->currentFrame();
            REQUIRE(a != nullptr);
            REQUIRE(z != nullptr);
            REQUIRE(a->width() == z->width());
            REQUIRE(a->height() == z->height());
            CHECK(std::memcmp(a->yData(), z->yData(), a->yStride() * a->height()) == 0);
            CHECK(std::memcmp(a->uData(), z->uData(), a->uvStride() * a->height() / 2U) == 0);
            CHECK(std::memcmp(a->vData(), z->vData(), a->uvStride() * a->height() / 2U) == 0);
        }
        REQUIRE(defaults->frameCount() > 0U);
    }
}

#ifdef NDEBUG
TEST_CASE("Skip chroma: unsupported CAVLC I_PCM never writes raw samples")
{
    for (bool skip : {false, true})
    {
        auto decoder = std::make_unique<H264Decoder>(H264Decoder::Settings{skip});
        Sps sps;
        sps.valid_ = true;
        sps.profileIdc_ = cProfileBaseline;
        sps.widthInMbs_ = sps.heightInMbs_ = 1U;
        sps.bitsInFrameNum_ = 4U;
        sps.picOrderCntType_ = 2U;
        sps.numRefFrames_ = 1U;
        Pps pps;
        pps.valid_ = true;
        REQUIRE(decoder->paramSets().storeSps(sps) == Result::Ok);
        REQUIRE(decoder->paramSets().storePps(pps) == Result::Ok);
        NalUnit nal;
        nal.type = NalType::SliceIdr;
        nal.refIdc = 3U;
        // first_mb=0, I, pps=0, frame_num=0, idr_pic_id=0,
        // marking flags=0, qp_delta=0, mb_type=25, PCM byte alignment.
        nal.rbspData = {0xB8U, 0x48U, 0x68U};
        nal.rbspData.insert(nal.rbspData.end(), 256U, 73U);
        nal.rbspData.insert(nal.rbspData.end(), 64U, 99U);
        nal.rbspData.insert(nal.rbspData.end(), 64U, 171U);
        nal.rbspData.push_back(0x80U);
        // Existing Release behavior labels this partial frame FrameDecoded.
        // Assert rejection at the MB, rather than claiming valid PCM output.
        CHECK(decoder->processNal(nal) == DecodeStatus::FrameDecoded);
        CHECK(decoder->diagMbEntered() == 0U);
        CHECK(decoder->diagMbCompleted() == UINT32_MAX);
        REQUIRE(decoder->currentFrame() != nullptr);
        const Frame* frame = decoder->currentFrame();
        CHECK(std::all_of(frame->yData(), frame->yData() + 256U,
                          [](uint8_t value) { return value == 0U; }));
        CHECK(frame->hasChroma() == !skip);
    }
}
#endif

TEST_CASE("Skip chroma: frame allocation omits and releases U/V")
{
    Frame frame;
    REQUIRE(frame.allocate(320U, 192U));
    frame.fill(16U, 111U, 222U);
    CHECK(frame.allocatedBytes() == 92160U);
    PreflightReset reset;
    setAllocationPreflightForTesting(allowOnlyLuma);
    gPlaneRequests = 0U;
    REQUIRE(frame.allocate(320U, 192U, true));
    CHECK(gPlaneRequests == 1U);
    CHECK(frame.allocatedBytes() == 61440U);
    CHECK_FALSE(frame.hasChroma());
    CHECK(frame.uData() == nullptr);
    CHECK(frame.vData() == nullptr);
    CHECK(frame.uvStride() == 0U);
    frame.fill(77U, 1U, 2U);
    CHECK(frame.y(0U, 0U) == 77U);
    setAllocationPreflightForTesting(nullptr);
    REQUIRE(frame.allocate(320U, 192U));
    CHECK(frame.hasChroma());
    CHECK(frame.u(0U, 0U) == 0U);
    CHECK(frame.v(0U, 0U) == 0U);
}

TEST_CASE("Skip chroma: two-slot DPB bytes, reuse and reinitialization")
{
    for (const auto dimensions : {std::array<uint16_t, 2>{320U, 192U}, {640U, 480U}})
    {
        const uint32_t pixels = static_cast<uint32_t>(dimensions[0]) * dimensions[1];
        Dpb dpb;
        for (bool skip : {false, true, false})
        {
            REQUIRE(dpb.init(dimensions[0], dimensions[1], 1U, skip));
            CHECK(dpb.allocatedFrameBytes() == 0U);
            Frame* first = dpb.getDecodeTarget(0U, 16U);
            REQUIRE(first != nullptr);
            first->fill(31U, 12U, 13U);
            dpb.markAsReference(0U);
            Frame* second = dpb.getDecodeTarget(1U, 16U);
            REQUIRE(second != nullptr);
            CHECK(second != first);
            CHECK(second->hasChroma() == !skip);
            CHECK(dpb.allocatedFrameCount() == 2U);
            CHECK(dpb.allocatedFrameBytes() == (skip ? pixels * 2U : pixels * 3U));
            dpb.flush();
            REQUIRE(dpb.getDecodeTarget(0U, 16U) != nullptr);
            CHECK(dpb.allocatedFrameCount() == 2U);
            CHECK(dpb.allocatedFrameBytes() == (skip ? pixels * 2U : pixels * 3U));
        }
    }
}

TEST_CASE("Skip chroma: deblocking preserves joint threshold and Y edges")
{
    for (int32_t offset : {0, 40})
    {
        Frame full, luma;
        REQUIRE(full.allocate(32U, 32U));
        REQUIRE(luma.allocate(32U, 32U, true));
        full.fill(128U, 128U, 128U);
        luma.fill(128U, 0U, 0U);
        for (uint32_t y = 0U; y < 32U; ++y)
            for (uint32_t x = 16U; x < 32U; ++x)
                full.y(x, y) = luma.y(x, y) = 132U;
        uint8_t nnz[64] = {}, transforms[4] = {};
        MbMotionInfo motion[64]{};
        for (auto& m : motion) m.refIdx = -1;
        int32_t qps[4] = {51, 0, 51, 0};
        deblockMb(full, 1U, 1U, true, 0, 0, nnz, motion, qps, transforms, offset, 2U, 2U);
        deblockMb(luma, 1U, 1U, true, 0, 0, nnz, motion, qps, transforms, offset, 2U, 2U, true);
        CHECK(std::memcmp(full.yData(), luma.yData(), 1024U) == 0);
        if (offset == 0)
            CHECK(luma.y(15U, 16U) == 128U);
        else
            CHECK(luma.y(15U, 16U) != 128U);
    }
}

#ifndef ESP_PLATFORM
TEST_CASE("Skip chroma: CAVLC and CABAC neighbors and prediction paths")
{
    for (const char* name : {"cavlc_4mb_noisy.h264", "cabac_4mb_noisy.h264",
         "cavlc_1mb_red.h264", "cabac_min_chroma.h264", "baseline_640x480_short.h264",
         "scrolling_texture_high.h264", "bouncing_ball_main.h264", "gradient_pan_high.h264",
         "bench_scroll_high_640x480.h264", "tapo_c110_stream2_high.h264", "g2_10s_700k.h264"})
    {
        INFO(name);
        const auto data = getFixture(name);
        REQUIRE_FALSE(data.empty());
        const auto result = test::compareChromaModes(data);
        REQUIRE(result.frames > 0U);
        checkComparison(result);
    }
}

TEST_CASE("Skip chroma: every desktop fixture preserves every Y frame")
{
    std::vector<std::filesystem::path> clips;
    for (const auto& file : std::filesystem::recursive_directory_iterator(SUB0H264_TEST_FIXTURES_DIR))
        if (file.is_regular_file() && file.path().extension() == ".h264")
            clips.push_back(file.path());
    std::sort(clips.begin(), clips.end());
    REQUIRE_FALSE(clips.empty());
    uint64_t frames = 0U;
    uint32_t failures = 0U;
    for (const auto& clip : clips)
    {
        INFO(clip.filename().string());
        auto relative = std::filesystem::relative(clip, SUB0H264_TEST_FIXTURES_DIR).generic_string();
        const auto data = getFixture(relative.c_str());
        REQUIRE_FALSE(data.empty());
        const auto result = test::compareChromaModes(data);
        checkComparison(result);
        frames += result.frames;
        if (result.parseErrors || result.decodeErrors || result.partialFrames || result.frames == 0U)
        {
            ++failures;
            MESSAGE("Matching baseline limitation: " << relative << ", frames=" << result.frames
                    << ", parse_errors=" << result.parseErrors << ", decode_errors=" << result.decodeErrors
                    << ", partial_frames=" << result.partialFrames);
        }
    }
    MESSAGE("Paired fixtures: clips=" << clips.size() << ", frames=" << frames
            << ", failed clips=" << failures);
}
#endif
