/** Sub0h264 — Decode pipeline timing instrumentation
 *
 *  Optional per-frame timing of parse vs reconstruction phases.
 *  Enable with SUB0H264_TIMING=1 (compile-time).
 *
 *  Used for Phase 12 dual-core pipeline research: measures the
 *  parse/recon time split to evaluate pipelining potential.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_DECODE_TIMING_HPP
#define CROG_SUB0H264_DECODE_TIMING_HPP

#include <cstdint>
#include <cstdio>

#ifndef SUB0H264_TIMING
#define SUB0H264_TIMING 0
#endif

#if defined(ESP_PLATFORM) && __has_include("esp_timer.h")
#include "esp_timer.h"
inline int64_t sub0h264TimerUs() { return esp_timer_get_time(); }
#elif defined(ESP_PLATFORM)
// esp_timer not available — use FreeRTOS tick count as fallback
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
inline int64_t sub0h264TimerUs()
{
    return static_cast<int64_t>(xTaskGetTickCount()) * (1000000LL / configTICK_RATE_HZ);
}
#else
#include <chrono>
inline int64_t sub0h264TimerUs()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}
#endif

namespace sub0h264 {

/// Per-frame timing accumulator.
struct FrameTiming
{
    int64_t parseUs = 0;     ///< Entropy decode (CAVLC/CABAC) time
    int64_t reconUs = 0;     ///< Prediction + transform + deblock time
    int64_t totalUs = 0;     ///< Total frame time (includes overhead)
    uint32_t frameIdx = 0U;
};

/// Per-section timing for profiling decode hot paths.
/// Updated by the decoder when enabled; zero-cost when not used.
struct SectionProfile
{
    int64_t entropyUs = 0;   ///< CAVLC/CABAC bin decode + coefficient extraction
    int64_t intraPredUs = 0; ///< Intra prediction (4x4, 8x8, 16x16, chroma)
    int64_t interPredUs = 0; ///< Motion compensation (luma + chroma MC)
    int64_t transformUs = 0; ///< IDCT + dequant (4x4 and 8x8)
    int64_t deblockUs = 0;   ///< Deblocking filter
    int64_t overheadUs = 0;  ///< NAL parsing, slice header, DPB management
    uint32_t frameCount = 0U;

    void print() const noexcept
    {
        int64_t total = entropyUs + intraPredUs + interPredUs + transformUs
                      + deblockUs + overheadUs;
        if (total <= 0) return;
        std::printf("  Section profile (%lu frames, %lld us total):\n",
                    (unsigned long)frameCount, (long long)total);
        std::printf("    Entropy:    %8lld us  (%5.1f%%)\n",
                    (long long)entropyUs, 100.0 * entropyUs / total);
        std::printf("    Intra pred: %8lld us  (%5.1f%%)\n",
                    (long long)intraPredUs, 100.0 * intraPredUs / total);
        std::printf("    Inter pred: %8lld us  (%5.1f%%)\n",
                    (long long)interPredUs, 100.0 * interPredUs / total);
        std::printf("    Transform:  %8lld us  (%5.1f%%)\n",
                    (long long)transformUs, 100.0 * transformUs / total);
        std::printf("    Deblock:    %8lld us  (%5.1f%%)\n",
                    (long long)deblockUs, 100.0 * deblockUs / total);
        std::printf("    Overhead:   %8lld us  (%5.1f%%)\n",
                    (long long)overheadUs, 100.0 * overheadUs / total);
        double fps = (frameCount > 0 && total > 0)
            ? (frameCount * 1e6 / total) : 0.0;
        std::printf("    Effective:  %.1f fps\n", fps);
    }
};

/// Accumulated timing across all frames in a stream.
struct StreamTiming
{
    int64_t totalParseUs = 0;
    int64_t totalReconUs = 0;
    int64_t totalFrameUs = 0;
    uint32_t frameCount = 0U;

    void addFrame(const FrameTiming& ft) noexcept
    {
        totalParseUs += ft.parseUs;
        totalReconUs += ft.reconUs;
        totalFrameUs += ft.totalUs;
        ++frameCount;
    }

    void print() const noexcept
    {
        if (frameCount == 0U) return;

        double parsePct = (totalFrameUs > 0)
            ? (100.0 * totalParseUs / totalFrameUs) : 0.0;
        double reconPct = (totalFrameUs > 0)
            ? (100.0 * totalReconUs / totalFrameUs) : 0.0;
        double overheadPct = 100.0 - parsePct - reconPct;

        std::printf("  Parse/Recon split (%lu frames):\n", (unsigned long)frameCount);
        std::printf("    Parse:    %8lld us  (%5.1f%%)\n",
                    (long long)totalParseUs, parsePct);
        std::printf("    Recon:    %8lld us  (%5.1f%%)\n",
                    (long long)totalReconUs, reconPct);
        std::printf("    Overhead: %8lld us  (%5.1f%%)\n",
                    (long long)(totalFrameUs - totalParseUs - totalReconUs), overheadPct);
        std::printf("    Total:    %8lld us\n", (long long)totalFrameUs);

        /// Pipeline scaling estimate (Amdahl's law):
        /// If parse and recon are pipelined across 2 cores:
        /// Speedup = total / max(parse, recon)
        double maxPhase = (totalParseUs > totalReconUs)
            ? static_cast<double>(totalParseUs)
            : static_cast<double>(totalReconUs);
        double pipelineSpeedup = (maxPhase > 0.0)
            ? (static_cast<double>(totalFrameUs) / maxPhase) : 1.0;
        std::printf("    Pipeline potential: %.2fx (Amdahl)\n", pipelineSpeedup);
    }
};

/// RAII timer for a code section. Adds elapsed time to a target accumulator.
struct ScopedTimer
{
#if SUB0H264_TIMING
    int64_t* target_;
    int64_t start_;

    explicit ScopedTimer(int64_t& target) noexcept
        : target_(&target), start_(sub0h264TimerUs()) {}

    ~ScopedTimer() noexcept
    {
        *target_ += (sub0h264TimerUs() - start_);
    }
#else
    explicit ScopedTimer(int64_t&) noexcept {}
#endif

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;
};

} // namespace sub0h264

#endif // CROG_SUB0H264_DECODE_TIMING_HPP
