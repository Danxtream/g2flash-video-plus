/** Sub0h264 — Structured decode trace system
 *
 *  Provides per-MB and per-block trace events for decoder debugging.
 *  Two modes:
 *
 *  1. Printf trace (SUB0H264_TRACE=1): compile-time enabled, zero cost otherwise.
 *     Logs to stdout with runtime MB/block filtering.
 *
 *  2. Callback trace: always available. Set a callback to capture trace events
 *     programmatically (e.g., in unit tests). The callback is checked per-MB
 *     so the cost is one pointer test per MB when no callback is set.
 *
 *  Usage in decoder:
 *    trace_.onMbStart(mbX, mbY, mbType, bitOffset);
 *    trace_.onChromaDc(mbX, mbY, dcCb, dcCr, 4);  // after dequant
 *    trace_.onBlockResidual(mbX, mbY, blkIdx, nC, tc, bits);
 *
 *  Usage in tests:
 *    decoder.trace().setCallback([](const TraceEvent& e) { ... });
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_DECODE_TRACE_HPP
#define CROG_SUB0H264_DECODE_TRACE_HPP

#include "motion.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <functional>

namespace sub0h264 {

/// Sentinel value: trace all MBs/blocks (no filter).
inline constexpr uint32_t cTraceAll = UINT32_MAX;

/// Trace event types for structured callback tracing.
enum class TraceEventType : uint8_t
{
    // Level: summary
    FrameStart,       ///< a=frameIdx, b=sliceType, c=QP, d=width|height<<16
    FrameDone,        ///< a=frameIdx, b=totalBits

    // Level: mb
    MbStart,          ///< a=mbType, b=bitOffset
    MbEnd,            ///< a=0, b=bitOffset
    MbDone,           ///< a=cbp, b=bitOffset, c=QP

    // Level: block
    BlockResidual,    ///< a=scanIdx, b=nC, c=totalCoeff, d=bitsConsumed
    BlockPredMode,    ///< a=scanIdx, b=rasterIdx, c=predMode, d=mpm

    // Level: coeff
    ChromaDcRaw,      ///< a=0 Cb / 1 Cr; data=raw coefficients
    ChromaDcDequant,  ///< a=0 Cb / 1 Cr; data=dequantized coefficients
    LumaDcDequant,    ///< data=16 DC values after Hadamard + dequant
    BlockCoeffs,      ///< a=scanIdx; data=dequantized coefficients

    // Level: pixel
    BlockPixels,      ///< a=scanIdx; data=pred[16] then output[16]
    MvPrediction,     ///< a=partIdx, data=[mvpX,mvpY,mvdX,mvdY,mvX,mvY,...]

    // Level: slice
    SliceStart,       ///< a=sliceIdx, b=sliceType, c=cabacFlag, d=sliceQp

    // Level: entropy — CABAC (emitted when SUB0H264_TRACE is defined)
    CabacInit,        ///< a=sliceIdx, b=range, c=offset (after 9-bit init)
    CabacBin,         ///< a=binIdx, b=ctxIdx (-1 for bypass), c=decodedBit,
                      ///  d=postRange; data=[postOffset, preState, preMps]

    // Level: entropy — CAVLC (emitted when SUB0H264_TRACE is defined)
    CavlcCoeffToken,  ///< a=totalCoeff, b=trailingOnes, c=nC
    CavlcLevel,       ///< a=levelIdx, b=levelValue
    CavlcTotalZeros,  ///< a=totalZeros
    CavlcRunBefore,   ///< a=runIdx, b=runValue
};

/// Structured trace event — passed to callbacks.
struct TraceEvent
{
    TraceEventType type;
    uint16_t mbX;
    uint16_t mbY;
    uint32_t a;            ///< Context-dependent: mbType, blkIdx, etc.
    uint32_t b;            ///< Context-dependent: bitOffset, nC, etc.
    uint32_t c;            ///< Context-dependent: totalCoeff, etc.
    uint32_t d;            ///< Context-dependent: bits consumed, etc.
    const int16_t* data;   ///< Optional pointer to coefficient array
    uint32_t dataLen;      ///< Length of data array
};

/// Trace callback type.
using TraceCallback = std::function<void(const TraceEvent&)>;

/** Runtime trace filter + structured event logger.
 *
 *  Always compiled in (not gated by SUB0H264_TRACE).
 *  Printf output is only available with SUB0H264_TRACE=1.
 *  Callback is always available.
 */
struct DecodeTrace
{
    uint32_t filterMbX = cTraceAll;
    uint32_t filterMbY = cTraceAll;
    uint32_t filterBlk = cTraceAll;
    bool enabled = false;

    /** Should we trace this macroblock? */
    bool shouldTrace(uint32_t mbX, uint32_t mbY) const noexcept
    {
        if (filterMbX != cTraceAll && filterMbX != mbX) return false;
        if (filterMbY != cTraceAll && filterMbY != mbY) return false;
        return enabled || callback_;
    }

    /** Should we trace this specific block? */
    bool shouldTraceBlock(uint32_t mbX, uint32_t mbY, uint32_t blk) const noexcept
    {
        if (!shouldTrace(mbX, mbY)) return false;
        if (filterBlk != cTraceAll && filterBlk != blk) return false;
        return true;
    }

    /** Set a callback to receive structured trace events. */
    void setCallback(TraceCallback cb) noexcept { callback_ = std::move(cb); }

    /** Clear the trace callback. */
    void clearCallback() noexcept { callback_ = nullptr; }

    /** Has a callback been set? */
    bool hasCallback() const noexcept { return callback_ != nullptr; }

    // ── Structured trace events ─────────────────────────────────────

    void onMbStart(uint32_t mbX, uint32_t mbY, uint32_t mbType, uint32_t bitOff) const noexcept
    {
        emit({TraceEventType::MbStart, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), mbType, bitOff, 0, 0, nullptr, 0});
#if SUB0H264_TRACE
        if (enabled && shouldTrace(mbX, mbY))
            std::printf("[TRACE] MB(%u,%u) type=%u bit=%u\n", mbX, mbY, mbType, bitOff);
#endif
    }

    void onMbEnd(uint32_t mbX, uint32_t mbY, uint32_t bitOff) const noexcept
    {
        emit({TraceEventType::MbEnd, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), 0, bitOff, 0, 0, nullptr, 0});
#if SUB0H264_TRACE
        if (enabled && shouldTrace(mbX, mbY))
            std::printf("[TRACE] MB(%u,%u) END bit=%u\n", mbX, mbY, bitOff);
#endif
    }

    void onChromaDcRaw(uint32_t mbX, uint32_t mbY,
                       const int16_t* rawCb, const int16_t* rawCr) const noexcept
    {
        emit({TraceEventType::ChromaDcRaw, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), 0, 0, 0, 0, rawCb, 4});
        emit({TraceEventType::ChromaDcRaw, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), 1, 0, 0, 0, rawCr, 4});
    }

    void onChromaDcDequant(uint32_t mbX, uint32_t mbY,
                           const int16_t* dcCb, const int16_t* dcCr) const noexcept
    {
        // Pack Cb in first call, Cr in second
        emit({TraceEventType::ChromaDcDequant, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), 0, 0, 0, 0, dcCb, 4});
        emit({TraceEventType::ChromaDcDequant, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), 1, 0, 0, 0, dcCr, 4});
#if SUB0H264_TRACE
        if (enabled && shouldTrace(mbX, mbY))
            std::printf("[TRACE] MB(%u,%u) chromaDC Cb=[%d,%d,%d,%d] Cr=[%d,%d,%d,%d]\n",
                mbX, mbY, dcCb[0], dcCb[1], dcCb[2], dcCb[3],
                dcCr[0], dcCr[1], dcCr[2], dcCr[3]);
#endif
    }

    void onBlockResidual(uint32_t mbX, uint32_t mbY, uint32_t blkIdx,
                         int32_t nC, uint32_t tc, uint32_t bits) const noexcept
    {
        emit({TraceEventType::BlockResidual, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), blkIdx, static_cast<uint32_t>(nC), tc, bits, nullptr, 0});
#if SUB0H264_TRACE
        if (enabled && shouldTraceBlock(mbX, mbY, blkIdx))
            std::printf("[TRACE] MB(%u,%u) blk%u nC=%d tc=%u %ub\n",
                mbX, mbY, blkIdx, nC, tc, bits);
#endif
    }

    /** Trace MV prediction result for a partition.
     *  @param partIdx Partition index (0 or 1)
     *  @param mvp  Predicted MV (before adding MVD)
     *  @param mvd  Motion vector difference from bitstream
     *  @param mv   Final MV (mvp + mvd)
     *  @param a,b,c  Neighbor motion info (LEFT, TOP, TOP_RIGHT/TOP_LEFT)
     */
    void onMvPrediction(uint32_t mbX, uint32_t mbY, uint32_t partIdx,
                         MotionVector mvp, MotionVector mvd, MotionVector mv,
                         const MbMotionInfo& a, const MbMotionInfo& b,
                         const MbMotionInfo& c) const noexcept
    {
        constexpr int16_t cUnavail = INT16_MIN;
        int16_t buf[12] = {
            mvp.x, mvp.y, mvd.x, mvd.y, mv.x, mv.y,
            a.available ? a.mv.x : cUnavail, a.available ? a.mv.y : cUnavail,
            b.available ? b.mv.x : cUnavail, b.available ? b.mv.y : cUnavail,
            c.available ? c.mv.x : cUnavail, c.available ? c.mv.y : cUnavail
        };
        emit({TraceEventType::MvPrediction, static_cast<uint16_t>(mbX),
              static_cast<uint16_t>(mbY), partIdx, 0, 0, 0, buf, 12});
#if SUB0H264_TRACE
        if (enabled && shouldTrace(mbX, mbY))
            std::printf("[TRACE] MB(%u,%u) part%u MVP=(%d,%d) MVD=(%d,%d) MV=(%d,%d) "
                        "A=(%d,%d) B=(%d,%d) C=(%d,%d)\n",
                        mbX, mbY, partIdx, mvp.x, mvp.y, mvd.x, mvd.y, mv.x, mv.y,
                        buf[6], buf[7], buf[8], buf[9], buf[10], buf[11]);
#endif
    }

    // ── Slice-level events ──────────────────────────────────────────

    void onSliceStart(uint32_t sliceIdx, uint32_t sliceType,
                      bool cabac, int32_t sliceQp) const noexcept
    {
        emit({TraceEventType::SliceStart, 0, 0,
              sliceIdx, sliceType, cabac ? 1U : 0U,
              static_cast<uint32_t>(sliceQp), nullptr, 0});
    }

    // ── Entropy-level events (gated by SUB0H264_TRACE) ────────────

#if SUB0H264_TRACE
    void onCabacInit(uint32_t sliceIdx, uint32_t range,
                     uint32_t offset) const noexcept
    {
        emit({TraceEventType::CabacInit, 0, 0,
              sliceIdx, range, offset, 0, nullptr, 0});
    }

    void onCabacBin(uint32_t binIdx, int32_t ctxIdx, uint32_t bit,
                    uint32_t postRange, int16_t postOffset,
                    int16_t preState, int16_t preMps) const noexcept
    {
        int16_t buf[3] = { postOffset, preState, preMps };
        emit({TraceEventType::CabacBin, 0, 0,
              binIdx, static_cast<uint32_t>(ctxIdx), bit,
              postRange, buf, 3});
    }

    void onCavlcCoeffToken(uint32_t totalCoeff, uint32_t trailingOnes,
                           int32_t nC) const noexcept
    {
        emit({TraceEventType::CavlcCoeffToken, 0, 0,
              totalCoeff, trailingOnes, static_cast<uint32_t>(nC),
              0, nullptr, 0});
    }

    void onCavlcLevel(uint32_t levelIdx, int32_t value) const noexcept
    {
        emit({TraceEventType::CavlcLevel, 0, 0,
              levelIdx, static_cast<uint32_t>(value), 0, 0, nullptr, 0});
    }

    void onCavlcTotalZeros(uint32_t totalZeros) const noexcept
    {
        emit({TraceEventType::CavlcTotalZeros, 0, 0,
              totalZeros, 0, 0, 0, nullptr, 0});
    }

    void onCavlcRunBefore(uint32_t runIdx, uint32_t value) const noexcept
    {
        emit({TraceEventType::CavlcRunBefore, 0, 0,
              runIdx, value, 0, 0, nullptr, 0});
    }
#endif // SUB0H264_TRACE

    /** Printf-style log (only with SUB0H264_TRACE). */
    void log([[maybe_unused]] const char* fmt, ...) const noexcept
    {
#if SUB0H264_TRACE
        if (!enabled) return;
        std::va_list args;
        va_start(args, fmt);
        std::printf("[TRACE] ");
        std::vprintf(fmt, args);
        std::printf("\n");
        va_end(args);
#endif
    }

// private: // Made public for direct trace emission from decoder code
    TraceCallback callback_;

    void emit(const TraceEvent& e) const noexcept
    {
        if (callback_) callback_(e);
    }
};

} // namespace sub0h264

#endif // CROG_SUB0H264_DECODE_TRACE_HPP
