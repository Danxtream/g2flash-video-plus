/* No-profile timing shim used for the glasses speed tests.
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef CROG_SUB0H264_DECODE_TIMING_HPP
#define CROG_SUB0H264_DECODE_TIMING_HPP
#include <cstdint>

/* Supply the vendor guard before decoder.hpp to avoid a chrono/clock runtime.
 * This disables measurement hooks, not the structured DecodeTrace machinery. */
inline constexpr int64_t sub0h264TimerUs() noexcept { return 0; }
namespace sub0h264 {
struct SectionProfile {
    int64_t entropyUs = 0, intraPredUs = 0, interPredUs = 0;
    int64_t transformUs = 0, deblockUs = 0, overheadUs = 0;
    uint32_t frameCount = 0;
};
}
#endif
