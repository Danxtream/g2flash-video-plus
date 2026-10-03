/** Sub0h264 Trace Tool — First-class diagnostic decode tracer
 *
 *  Decodes an H.264 bitstream using the same code paths as the library,
 *  with structured trace output at configurable detail levels.
 *
 *  Usage:
 *    sub0h264_trace <input.h264> [options]
 *
 *  Options:
 *    --frame N           Decode only frame N (default: all)
 *    --mb X,Y            Filter trace to macroblock (X,Y)
 *    --level LEVEL       Output detail: summary|mb|block|coeff|pixel
 *    --dump FILE.yuv     Dump decoded frame(s) to YUV file
 *    --compare FILE.yuv  Compare against reference and report per-MB PSNR
 *    --width W           Frame width (auto-detected from SPS)
 *    --height H          Frame height (auto-detected from SPS)
 *
 *  SPDX-License-Identifier: MIT
 */
#include "decoder.hpp"
#include "annexb.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#if __has_include("Version.h")
#include "Version.h"
#endif
#ifndef VERSION_FULL
#define VERSION_FULL "unknown"
#endif

using namespace sub0h264;

// ── Detail levels ───────────────────────────────────────────────────────

enum class TraceLevel { Summary, Mb, Block, Coeff, Pixel, Entropy };

static TraceLevel parseLevel(const char* s)
{
    if (std::strcmp(s, "summary") == 0) return TraceLevel::Summary;
    if (std::strcmp(s, "mb") == 0) return TraceLevel::Mb;
    if (std::strcmp(s, "block") == 0) return TraceLevel::Block;
    if (std::strcmp(s, "coeff") == 0) return TraceLevel::Coeff;
    if (std::strcmp(s, "pixel") == 0) return TraceLevel::Pixel;
    if (std::strcmp(s, "entropy") == 0) return TraceLevel::Entropy;
    std::fprintf(stderr, "Unknown level '%s' — using 'mb'\n", s);
    return TraceLevel::Mb;
}

// ── File I/O ────────────────────────────────────────────────────────────

static std::vector<uint8_t> loadFile(const char* path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    f.seekg(0);
    std::vector<uint8_t> d(static_cast<size_t>(sz));
    f.read(reinterpret_cast<char*>(d.data()), sz);
    return d;
}

static void dumpFrame(const Frame& frame, const char* path)
{
    FILE* f = std::fopen(path, "ab"); // append for multi-frame
    if (!f) { std::fprintf(stderr, "Cannot open %s\n", path); return; }
    for (uint32_t r = 0; r < frame.height(); ++r)
        std::fwrite(frame.yRow(r), 1, frame.width(), f);
    for (uint32_t r = 0; r < frame.height() / 2; ++r)
        std::fwrite(frame.uRow(r), 1, frame.width() / 2, f);
    for (uint32_t r = 0; r < frame.height() / 2; ++r)
        std::fwrite(frame.vRow(r), 1, frame.width() / 2, f);
    std::fclose(f);
}

static double computePsnr(const Frame& dec, const uint8_t* ref, uint32_t w, uint32_t h)
{
    double sse = 0;
    for (uint32_t r = 0; r < h; ++r)
    {
        const uint8_t* dr = dec.yRow(r);
        const uint8_t* rr = ref + r * w;
        for (uint32_t c = 0; c < w; ++c)
        {
            double d = static_cast<double>(dr[c]) - rr[c];
            sse += d * d;
        }
    }
    double mse = sse / (w * h);
    return (mse > 0) ? 10.0 * std::log10(255.0 * 255.0 / mse) : 999.0;
}

// ── Trace callback ──────────────────────────────────────────────────────

struct TraceState
{
    TraceLevel level = TraceLevel::Mb;
    int32_t filterMbX = -1; // -1 = all
    int32_t filterMbY = -1;
    uint32_t frameIdx = 0;
};

static void traceCallback(const TraceEvent& e, const TraceState& state)
{
    // Filter by MB position
    if (state.filterMbX >= 0 && e.mbX != static_cast<uint16_t>(state.filterMbX))
        return;
    if (state.filterMbY >= 0 && e.mbY != static_cast<uint16_t>(state.filterMbY))
        return;

    switch (e.type)
    {
    case TraceEventType::MbStart:
        if (state.level >= TraceLevel::Mb)
            std::printf("  MB(%u,%u) type=%u bit=%u\n", e.mbX, e.mbY, e.a, e.b);
        break;

    case TraceEventType::MbDone:
        if (state.level >= TraceLevel::Mb)
            std::printf("  MB(%u,%u) done cbp=0x%02x bit=%u qp=%u\n",
                        e.mbX, e.mbY, e.a, e.b, e.c);
        break;

    case TraceEventType::BlockResidual:
        if (state.level >= TraceLevel::Coeff && e.data && e.dataLen > 0)
        {
            std::printf("    blk scan%u nC=%u tc=%u bits=%u raw=[", e.a, e.b, e.c, e.d);
            for (uint32_t i = 0; i < e.dataLen; ++i)
                std::printf("%d ", e.data[i]);
            std::printf("]\n");
        }
        else if (state.level >= TraceLevel::Block)
            std::printf("    blk scan%u nC=%u tc=%u bits=%u\n", e.a, e.b, e.c, e.d);
        break;

    case TraceEventType::BlockPredMode:
        if (state.level >= TraceLevel::Block)
            std::printf("    blk scan%u raster%u mode=%u mpm=%u\n",
                        e.a, e.b, e.c, e.d);
        break;

    case TraceEventType::BlockCoeffs:
        if (state.level >= TraceLevel::Coeff && e.data && e.dataLen > 0)
        {
            std::printf("    blk scan%u dequant=[", e.a);
            for (uint32_t i = 0; i < e.dataLen; ++i)
                std::printf("%d ", e.data[i]);
            std::printf("]\n");
        }
        break;

    case TraceEventType::BlockPixels:
        if (state.level >= TraceLevel::Pixel && e.data && e.dataLen >= 32U)
        {
            std::printf("    blk scan%u pred=[", e.a);
            for (uint32_t i = 0; i < 16; ++i) std::printf("%d ", e.data[i]);
            std::printf("]\n");
            std::printf("    blk scan%u  out=[", e.a);
            for (uint32_t i = 16; i < 32; ++i) std::printf("%d ", e.data[i]);
            std::printf("]\n");
        }
        break;

    case TraceEventType::ChromaDcDequant:
        if (state.level >= TraceLevel::Coeff && e.data)
        {
            std::printf("    chroma%s DC dequant=[", e.a == 0 ? "Cb" : "Cr");
            for (uint32_t i = 0; i < e.dataLen; ++i)
                std::printf("%d ", e.data[i]);
            std::printf("]\n");
        }
        break;

    default:
        break;
    }
}

// ── Main ────────────────────────────────────────────────────────────────

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::printf("Sub0h264 Trace Tool [%s]\n\n", VERSION_FULL);
        std::printf("Usage: %s <input.h264> [options]\n\n", argv[0]);
        std::printf("Options:\n");
        std::printf("  --frame N          Decode only frame N\n");
        std::printf("  --mb X,Y           Filter to macroblock (X,Y)\n");
        std::printf("  --level LEVEL      summary|mb|block|coeff|pixel|entropy\n");
        std::printf("  --slice N          Target CABAC slice for entropy trace (default 1)\n");
        std::printf("  --dump FILE.yuv    Dump decoded frames\n");
        std::printf("  --compare FILE.yuv Compare against reference YUV\n");
        return 1;
    }

    // Parse arguments
    const char* inputPath = argv[1];
    const char* dumpPath = nullptr;
    const char* comparePath = nullptr;
    int32_t targetFrame = -1; // -1 = all
    uint32_t targetSlice = 1U; // for --level entropy
    TraceState state;

    for (int i = 2; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frame") == 0 && i + 1 < argc)
            targetFrame = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--slice") == 0 && i + 1 < argc)
            targetSlice = static_cast<uint32_t>(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--mb") == 0 && i + 1 < argc)
        {
            ++i;
            std::sscanf(argv[i], "%d,%d", &state.filterMbX, &state.filterMbY);
        }
        else if (std::strcmp(argv[i], "--level") == 0 && i + 1 < argc)
            state.level = parseLevel(argv[++i]);
        else if (std::strcmp(argv[i], "--dump") == 0 && i + 1 < argc)
            dumpPath = argv[++i];
        else if (std::strcmp(argv[i], "--compare") == 0 && i + 1 < argc)
            comparePath = argv[++i];
    }

    // For entropy mode: if no explicit --frame given, stop after the target slice
    if (state.level == TraceLevel::Entropy && targetFrame < 0)
        targetFrame = static_cast<int32_t>(targetSlice);

    // Load input
    auto h264 = loadFile(inputPath);
    if (h264.empty())
    {
        std::fprintf(stderr, "Cannot read %s\n", inputPath);
        return 1;
    }

    // Load reference for comparison
    std::vector<uint8_t> refData;
    if (comparePath)
        refData = loadFile(comparePath);

    // Remove dump file if it exists (we append frames)
    if (dumpPath)
        std::remove(dumpPath);

    // Set up decoder with trace callback
    H264Decoder decoder;
    bool entropyActive = false; // tracks whether the current slice is being traced

    decoder.trace().setCallback([&](const TraceEvent& e) {
        // Entropy mode: emit lock-step compatible output to stderr
        if (state.level == TraceLevel::Entropy)
        {
            switch (e.type)
            {
            case TraceEventType::SliceStart:
                entropyActive = (e.a == targetSlice);
                return;
            case TraceEventType::CabacInit:
                if (entropyActive)
                    std::fprintf(stderr, "OUR_SLICE_START slice=%u R=%u O=%u\n",
                                 e.a, e.b, e.c);
                return;
            case TraceEventType::CabacBin:
                if (entropyActive && e.data)
                {
                    int32_t ctxIdx = static_cast<int32_t>(e.b);
                    if (ctxIdx < 0)
                    {
                        // Bypass bin
                        std::fprintf(stderr, "%u BP %u %u %u\n",
                            e.a, e.c, e.d,
                            static_cast<uint32_t>(e.data[0]));
                    }
                    else
                    {
                        // Context bin: binIdx preState postMpsState bit range offset ctxIdx
                        std::fprintf(stderr, "%u %d %u %u %u %u %d\n",
                            e.a,
                            static_cast<int>(e.data[1]),     // preState
                            static_cast<uint32_t>(e.data[2]) | (e.c << 6U), // postMpsState (packed)
                            e.c, e.d,
                            static_cast<uint32_t>(e.data[0]), // postOffset
                            ctxIdx);
                    }
                }
                return;
            default:
                break;
            }
            return; // entropy mode ignores structural events
        }

        // Structural trace modes (summary/mb/block/coeff/pixel)
        traceCallback(e, state);
    });

    // Set MB filter on the trace system
    if (state.filterMbX >= 0)
        decoder.trace().filterMbX = static_cast<uint32_t>(state.filterMbX);
    if (state.filterMbY >= 0)
        decoder.trace().filterMbY = static_cast<uint32_t>(state.filterMbY);

    std::printf("Sub0h264 Trace [%s]\n", VERSION_FULL);
    std::printf("Input: %s (%zu bytes)\n", inputPath, h264.size());
    std::printf("Level: %s\n\n",
                state.level == TraceLevel::Summary ? "summary" :
                state.level == TraceLevel::Mb ? "mb" :
                state.level == TraceLevel::Block ? "block" :
                state.level == TraceLevel::Coeff ? "coeff" :
                state.level == TraceLevel::Pixel ? "pixel" : "entropy");

    // Decode
    std::vector<NalBounds> bounds;
    findNalUnits(h264.data(), static_cast<uint32_t>(h264.size()), bounds);

    uint32_t frameCount = 0;
    for (const auto& b : bounds)
    {
        NalUnit nal;
        if (!parseNalUnit(h264.data() + b.offset, b.size, nal))
            continue;

        DecodeStatus status = decoder.processNal(nal);
        if (status == DecodeStatus::FrameDecoded)
        {
            const Frame* frame = decoder.currentFrame();
            if (!frame) continue;

            // PSNR against reference
            double psnr = 0.0;
            uint32_t w = frame->width(), h = frame->height();
            uint32_t frameSize = w * h + 2 * (w / 2) * (h / 2);
            if (!refData.empty() && refData.size() >= (frameCount + 1) * frameSize)
            {
                psnr = computePsnr(*frame, refData.data() + frameCount * frameSize, w, h);
            }

            if (state.level == TraceLevel::Summary || targetFrame < 0 ||
                static_cast<uint32_t>(targetFrame) == frameCount)
            {
                std::printf("Frame %u: %ux%u", frameCount, w, h);
                if (psnr > 0.0)
                    std::printf(" PSNR=%.2f dB", psnr);
                std::printf("\n");
            }

            // Dump
            if (dumpPath && (targetFrame < 0 ||
                             static_cast<uint32_t>(targetFrame) == frameCount))
                dumpFrame(*frame, dumpPath);

            state.frameIdx = frameCount;
            ++frameCount;

            if (targetFrame >= 0 && frameCount > static_cast<uint32_t>(targetFrame))
                break;
        }
    }

    std::printf("\nDecoded %u frames.\n", frameCount);
    return 0;
}
