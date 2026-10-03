/** Sub0h264 — Decode benchmarks as doctest test cases
 *
 *  Tagged with [bench] for CTest label filtering:
 *    ctest --preset default -L bench     (desktop)
 *    ctest --preset esp32p4              (ESP32 — runs all including bench)
 *
 *  Methodology: validate → warm-up → 3 measured passes → report median fps.
 *  Uses doctest MESSAGE() for output so results appear in CTest logs.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "test_fixtures.hpp"
#include "../components/sub0h264/src/decode_timing.hpp"
#include "../components/sub0h264/src/decoder.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>

using namespace sub0h264;

/// Number of timed passes after warm-up.
static constexpr uint32_t cBenchPasses = 3U;

// Version.cmake generates Version.h at build time with VERSION_FULL etc.
#if __has_include("Version.h")
#include "Version.h"
#endif
#ifndef VERSION_FULL
#define VERSION_FULL "unknown"
#endif

/** Run a decode benchmark: validate, warm-up, measure, report. */
static void benchStream(const char* name, const char* fixture)
{
    auto data = getFixture(fixture);
    REQUIRE_FALSE(data.empty());

    // Validate
    int32_t frameCount;
    {
        auto dec = std::make_unique<H264Decoder>();
        frameCount = dec->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
    }
    REQUIRE(frameCount > 0);

    // Warm-up (discarded)
    {
        auto dec = std::make_unique<H264Decoder>();
        dec->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
    }

    // Measured passes
    std::array<double, cBenchPasses> passMs = {};
    for (uint32_t p = 0U; p < cBenchPasses; ++p)
    {
        auto dec = std::make_unique<H264Decoder>();
        int64_t t0 = sub0h264TimerUs();
        dec->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
        int64_t elapsed = sub0h264TimerUs() - t0;
        passMs[p] = elapsed / 1000.0;
    }

    // Median
    auto sorted = passMs;
    std::sort(sorted.begin(), sorted.end());
    double medianMs = sorted[cBenchPasses / 2U];
    double medianFps = (medianMs > 0.0) ? (frameCount * 1000.0 / medianMs) : 0.0;

    // Report via doctest MESSAGE (captured by CTest)
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "BENCH [%s] %s: %ld frames, %.1f ms median (%.1f / %.1f / %.1f), %.1f fps",
        VERSION_FULL, name, (long)frameCount, medianMs,
        passMs[0], passMs[1], passMs[2], medianFps);
    MESSAGE(buf);

    // Machine-readable JSON to stderr for scripts/gen_bench_report.py
    std::fprintf(stderr, "BENCH_JSON {\"id\":\"%s\",\"frames\":%ld,"
        "\"median_ms\":%.1f,\"fps\":%.1f,\"version\":\"%s\"}\n",
        name, (long)frameCount, medianMs, medianFps, VERSION_FULL);
}

TEST_CASE("Bench: Baseline CAVLC 640x480 (short)" * doctest::test_suite("bench"))
{
    benchStream("Baseline CAVLC (short)", "baseline_640x480_short.h264");
}

// high_640x480 excluded from benchmarks: 180/195 frames are B-slices
// (unsupported), producing meaningless fps. Re-add when B-frame support lands.

TEST_CASE("Bench: Flat black 640x480" * doctest::test_suite("bench"))
{
    benchStream("Flat black", "flat_black_baseline_640x480.h264");
}

TEST_CASE("Bench: CAVLC 320x240 I+P" * doctest::test_suite("bench"))
{
    benchStream("CAVLC 320x240 I+P", "scrolling_texture_baseline.h264");
}

TEST_CASE("Bench: CABAC 320x240 I+P" * doctest::test_suite("bench"))
{
    benchStream("CABAC 320x240 I+P", "scrolling_texture_high.h264");
}

// ── 640x480 Benchmark Fixtures ──────────────────────────────────────────

TEST_CASE("Bench: Scroll Baseline 640x480" * doctest::test_suite("bench"))
{
    benchStream("Scroll Baseline 640x480", "bench_scroll_baseline_640x480.h264");
}

TEST_CASE("Bench: Scroll High 640x480" * doctest::test_suite("bench"))
{
    benchStream("Scroll High 640x480", "bench_scroll_high_640x480.h264");
}

TEST_CASE("Bench: Ball Baseline 640x480" * doctest::test_suite("bench"))
{
    benchStream("Ball Baseline 640x480", "bench_ball_baseline_640x480.h264");
}

TEST_CASE("Bench: Ball High 640x480" * doctest::test_suite("bench"))
{
    benchStream("Ball High 640x480", "bench_ball_high_640x480.h264");
}

TEST_CASE("Bench: Still Baseline 640x480" * doctest::test_suite("bench"))
{
    benchStream("Still Baseline 640x480", "bench_still_baseline_640x480.h264");
}

TEST_CASE("Bench: Still High 640x480" * doctest::test_suite("bench"))
{
    benchStream("Still High 640x480", "bench_still_high_640x480.h264");
}

TEST_CASE("Bench: Tapo C110 stream2" * doctest::test_suite("bench"))
{
    auto data = getFixture("tapo_c110_stream2_high.h264");
    if (data.empty()) { MESSAGE("tapo fixture not available — skipping"); return; }
    benchStream("Tapo C110 stream2", "tapo_c110_stream2_high.h264");
}

/** Run a profiled decode: measures per-section timing breakdown. */
static void profileStream(const char* name, const char* fixture)
{
    auto data = getFixture(fixture);
    REQUIRE_FALSE(data.empty());

    SectionProfile profile = {};
    auto dec = std::make_unique<H264Decoder>();
    dec->setProfile(&profile);

    int64_t t0 = sub0h264TimerUs();
    int32_t frames = dec->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
    int64_t totalUs = sub0h264TimerUs() - t0;

    REQUIRE(frames > 0);

    int64_t profiledUs = profile.deblockUs + profile.overheadUs
                       + profile.intraPredUs + profile.interPredUs;
    int64_t entropyAndTransform = totalUs - profiledUs;

    char buf[768];
    std::snprintf(buf, sizeof(buf),
        "PROFILE [%s] %s: %ld frames in %lld us (%.1f fps)\n"
        "    Intra MB:   %8lld us  (%5.1f%%)\n"
        "    Inter MB:   %8lld us  (%5.1f%%)\n"
        "    Deblock:    %8lld us  (%5.1f%%)\n"
        "    FrameSync:  %8lld us  (%5.1f%%)\n"
        "    Other:      %8lld us  (%5.1f%%)",
        VERSION_FULL, name, (long)frames, (long long)totalUs,
        (totalUs > 0) ? (frames * 1e6 / totalUs) : 0.0,
        (long long)profile.intraPredUs,
        (totalUs > 0) ? (100.0 * profile.intraPredUs / totalUs) : 0.0,
        (long long)profile.interPredUs,
        (totalUs > 0) ? (100.0 * profile.interPredUs / totalUs) : 0.0,
        (long long)profile.deblockUs,
        (totalUs > 0) ? (100.0 * profile.deblockUs / totalUs) : 0.0,
        (long long)profile.overheadUs,
        (totalUs > 0) ? (100.0 * profile.overheadUs / totalUs) : 0.0,
        (long long)entropyAndTransform,
        (totalUs > 0) ? (100.0 * entropyAndTransform / totalUs) : 0.0);
    MESSAGE(buf);
}

TEST_CASE("Profile: Baseline CAVLC 640x480" * doctest::test_suite("bench"))
{
    profileStream("Baseline CAVLC", "baseline_640x480_short.h264");
}

TEST_CASE("Profile: Scrolling texture 320x240" * doctest::test_suite("bench"))
{
    auto data = getFixture("scrolling_texture_baseline.h264");
    if (data.empty()) { MESSAGE("scrolling_texture_baseline.h264 not embedded — skipping"); return; }
    profileStream("Scrolling texture", "scrolling_texture_baseline.h264");
}

TEST_CASE("Profile: CABAC 320x240 I+P" * doctest::test_suite("bench"))
{
    profileStream("CABAC I+P 320x240", "scrolling_texture_high.h264");
}
