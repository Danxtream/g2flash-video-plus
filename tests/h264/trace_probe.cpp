/** Exercise optional structured tracing without changing decoder output.
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "decoder.hpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char **argv)
{
    using namespace sub0h264;
    unsigned events = 0U, arguments = 0U;
    DecodeTrace trace;
    trace.setCallback([&](const TraceEvent&) { ++events; });
    if (trace.hasCallback() != bool(SUB0H264_ENABLE_CALLBACK_TRACE)) return 2;
    SUB0H264_TRACE_EVENT(trace.onMbStart(0U, 0U, ++arguments, 0U));
    if (events != unsigned(SUB0H264_ENABLE_CALLBACK_TRACE) ||
        arguments != unsigned(SUB0H264_TRACE_EVENTS_ENABLED)) return 3;
    trace.clearCallback();
    if (trace.hasCallback()) return 4;
    if (argc == 1)
    {
        trace.enabled = true;
        SUB0H264_TRACE_EVENT(trace.onMbEnd(0U, 0U, 7U));
        return 0;
    }
    if (argc != 2) return 5;
    std::ifstream file(argv[1], std::ios::binary);
    if (!file) return 6;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)), {});
    std::vector<NalBounds> bounds;
    findNalUnits(bytes.data(), bytes.size(), bounds);
    H264Decoder decoder(H264Decoder::Settings{true});
    decoder.trace().setCallback([&](const TraceEvent&) { ++events; });
    events = 0U;
    uint32_t pictures = 0U;
    for (const auto& b : bounds)
    {
        NalUnit nal;
        int status = parseNalUnit(bytes.data() + b.offset, b.size, nal) ?
            int(decoder.processNal(nal)) : -1;
        bool picture = pictures != decoder.frameCount();
        uint32_t header[] = {uint32_t(status), decoder.frameCount(),
            decoder.diagBitOffset(), decoder.diagMbEntered(), decoder.diagMbCompleted(),
            uint32_t(picture)};
        std::fwrite(header, sizeof(header), 1U, stdout);
        const auto& contexts = decoder.cabacContexts();
        std::fwrite(contexts.data(), sizeof(CabacCtx), contexts.size(), stdout);
        if (picture)
        {
            const Frame *frame = decoder.currentFrame();
            if (!frame) return 7;
            uint32_t dimensions[] = {frame->width(), frame->height()};
            std::fwrite(dimensions, sizeof(dimensions), 1U, stdout);
            for (uint32_t y = 0U; y < frame->height(); ++y)
                std::fwrite(frame->yRow(y), 1U, frame->width(), stdout);
            pictures = decoder.frameCount();
        }
    }
    if (!SUB0H264_ENABLE_CALLBACK_TRACE && events) return 8;
    std::fprintf(stderr, "pictures=%u events=%u\n", pictures, events);
    return std::ferror(stdout) ? 9 : 0;
}
