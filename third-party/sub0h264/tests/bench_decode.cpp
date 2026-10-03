/** Sub0h264 — Decode benchmark
 *
 *  Methodology (from NestNinja decoder-shootout):
 *  1. Validate before measuring (decode once, check frame count)
 *  2. Eliminate I/O from measurement (fixture already in memory)
 *  3. Warm-up pass discarded
 *  4. 3 measured passes, report median
 *  5. Report: fps, total ms, frames
 *
 *  SPDX-License-Identifier: MIT
 */
#include "test_fixtures.hpp"
#include "../components/sub0h264/src/decode_timing.hpp"
#include "../components/sub0h264/src/decoder.hpp"
#include <cstdio>
#include <memory>
#include <algorithm>
#include <array>

using namespace sub0h264;

/// Number of timed passes (after warm-up).
static constexpr uint32_t cMeasuredPasses = 3U;

struct BenchResult
{
    const char* name;
    uint32_t frames;
    std::array<double, cMeasuredPasses> passMs;
    double medianMs;
    double medianFps;
};

static BenchResult runBench(const char* name, const char* fixture)
{
    auto data = getFixture(fixture);
    if (data.empty())
    {
        std::printf("SKIP %s: fixture %s not found\n", name, fixture);
        return { name, 0U, {}, 0.0, 0.0 };
    }

    // Validation pass: decode once, verify frames produced
    {
        auto decoder = std::make_unique<H264Decoder>();
        int32_t frames = decoder->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
        if (frames <= 0)
        {
            std::printf("FAIL %s: decoded 0 frames\n", name);
            return { name, 0U, {}, 0.0, 0.0 };
        }
        std::printf("  %s: validated %d frames\n", name, frames);
    }

    // Warm-up pass (discarded)
    {
        auto decoder = std::make_unique<H264Decoder>();
        decoder->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
    }

    // Measured passes
    BenchResult result = { name, 0U, {}, 0.0, 0.0 };

    for (uint32_t pass = 0U; pass < cMeasuredPasses; ++pass)
    {
        auto decoder = std::make_unique<H264Decoder>();

        int64_t t0 = sub0h264TimerUs();
        int32_t frames = decoder->decodeStream(data.data(), static_cast<uint32_t>(data.size()));
        int64_t elapsed = sub0h264TimerUs() - t0;

        result.frames = static_cast<uint32_t>(frames);
        result.passMs[pass] = elapsed / 1000.0;
    }

    // Compute median
    auto sorted = result.passMs;
    std::sort(sorted.begin(), sorted.end());
    result.medianMs = sorted[cMeasuredPasses / 2U];
    result.medianFps = (result.medianMs > 0.0)
        ? (result.frames * 1000.0 / result.medianMs)
        : 0.0;

    return result;
}

static void printResults(const BenchResult& r)
{
    if (r.frames == 0U)
        return;

    std::printf("  %-30s  %4u frames  ", r.name, r.frames);
    for (uint32_t i = 0U; i < cMeasuredPasses; ++i)
        std::printf("%8.1f ms  ", r.passMs[i]);
    std::printf("  median: %8.1f ms  %6.1f fps\n", r.medianMs, r.medianFps);
}

#ifndef ESP_PLATFORM
// Desktop entry point
int main()
{
    std::printf("Sub0h264 Decode Benchmark\n");
    std::printf("========================\n\n");

    BenchResult results[] = {
        runBench("Baseline CAVLC (short)", "baseline_640x480_short.h264"),
        runBench("Baseline CAVLC (full)",  "baseline_640x480.h264"),
        runBench("High CABAC",             "high_640x480.h264"),
        runBench("Flat black",             "flat_black_baseline_640x480.h264"),
    };

    std::printf("\nResults:\n");
    std::printf("  %-30s  %s  ", "Stream", "Frames");
    for (uint32_t i = 0U; i < cMeasuredPasses; ++i)
        std::printf("  Pass %u    ", i + 1U);
    std::printf("    Median       FPS\n");
    std::printf("  %s\n", std::string(120, '-').c_str());

    for (const auto& r : results)
        printResults(r);

    return 0;
}
#endif
