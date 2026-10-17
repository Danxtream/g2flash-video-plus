/* Section timing shim for the decode-breakdown measuring build.
 * SPDX-License-Identifier: GPL-3.0-only */
#ifndef CROG_SUB0H264_DECODE_TIMING_HPP
#define CROG_SUB0H264_DECODE_TIMING_HPP
#include <cstdint>

/* Supply the vendor guard before decoder.hpp to avoid a chrono/clock runtime.
 * The decoder's own section hooks and its per-call measuring wrappers
 * (SUB0H264_SECTION_PROFILE) read this timer while a profile is attached. */
#define SUB0H264_SECTION_PROFILE 1

/* Upstream names the timer in microseconds. Here it returns raw cycles from
 * the Cortex-M DWT cycle counter (CYCCNT, 0xE0001004), which the firmware's
 * decode timing already runs; cycles become time only in the report, so no
 * division happens in the decoder. Native builds have no such counter. */
inline int64_t sub0h264TimerUs() noexcept {
#if defined(__arm__)
    return *reinterpret_cast<const volatile uint32_t *>(0xE0001004U);
#else
    return 0;
#endif
}

namespace sub0h264 {
/* CYCCNT wraps about every 17 s at 243 MHz. Each bracket is taken modulo
 * 2^32, so any bracket shorter than one wrap adds its exact length. */
struct SectionCycles {
    uint64_t cycles = 0;
    SectionCycles &operator+=(int64_t elapsed) noexcept {
        cycles += static_cast<uint32_t>(elapsed);
        return *this;
    }
};

/* Cycles plus the number of timed calls, so the timer's own cost per call
 * (calibrated once per decoder) can be subtracted in the report. */
struct SectionCalls {
    uint64_t cycles = 0;
    uint32_t calls = 0;
    void add(int64_t elapsed) noexcept {
        cycles += static_cast<uint32_t>(elapsed);
        ++calls;
    }
};

struct SectionProfile {
    /* Upstream's hooks. On the CAVLC path they time whole macroblocks:
     * intraPredUs the macroblocks of I slices, interPredUs the skipped and
     * coded inter macroblocks of P slices, overheadUs picture completion. */
    SectionCycles entropyUs, intraPredUs, interPredUs;
    SectionCycles transformUs, deblockUs, overheadUs;
    uint32_t frameCount = 0;
    /* Measuring build: intra macroblocks inside P slices, plus per-call time
     * of the inner helpers wherever the decoder calls them. */
    SectionCycles intraInPUs;
    SectionCalls motion, residual, transform, intra;
    uint32_t zeroTransforms = 0; /* 4x4 inverse transforms of all-zero input. */
};
}
#endif
