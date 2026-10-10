/* Offline Y references from the firmware's configured C ABI.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/h264/g2_h264.h"
#include "../../patches/h264/runtime.h"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <initializer_list>

static void *allocate(uint32_t bytes) { return std::malloc(bytes); }
static void release(void *memory) { std::free(memory); }
static int preflight(const g2_h264_request *requests, uint32_t count, g2_h264_failure *failure) {
    for (uint32_t i = 0; i < count; ++i) if (requests[i].size > 1024 * 1024) {
        *failure = {requests[i].tag, requests[i].size}; return 0;
    }
    return 1;
}
static void fatal(uint32_t reason) {
    std::fprintf(stderr, "decoder runtime failure %u\n", reason); std::_Exit(70 + reason);
}
static g2_h264_runtime callbacks = {allocate, release, preflight, fatal};
extern "C" const g2_h264_runtime *g2_h264_runtime_current() { return &callbacks; }
static void write32(uint32_t value) {
    uint8_t bytes[4];
    for (uint32_t i = 0; i < 4; ++i) bytes[i] = uint8_t(value >> (8 * i));
    if (std::fwrite(bytes, 1, sizeof(bytes), stdout) != sizeof(bytes)) std::exit(2);
}
int main() {
    void *memory = std::malloc(g2_h264_size());
    void *decoder = g2_h264_init(memory, g2_h264_size());
    if (!decoder || !g2_h264_limit_format(decoder, 320, 192, 1, 2)) return 2;
    std::fwrite("G2Y1", 1, 4, stdout);
    uint32_t sequence = 0, first = 0, frames = 0;
    for (;;) {
        uint8_t length[4], nal[4086];
        size_t read = std::fread(length, 1, 4, stdin);
        if (!read && std::feof(stdin)) break;
        if (read != 4) return 2;
        uint32_t bytes = uint32_t(length[0]) | uint32_t(length[1]) << 8 |
                         uint32_t(length[2]) << 16 | uint32_t(length[3]) << 24;
        if (!bytes || bytes > sizeof(nal) || std::fread(nal, 1, bytes, stdin) != bytes) return 2;
        g2_h264_result result = g2_h264_decode(decoder, nal, bytes);
        if (result == G2_H264_ERROR) {
            std::fprintf(stderr, "decoder refused NAL %u type %u\n", sequence, nal[0] & 31); return 3;
        }
        if (result == G2_H264_FRAME_READY) {
            g2_h264_frame_info frame = {};
            if (g2_h264_frame(decoder, &frame) != G2_H264_FRAME_READY ||
                frame.count != ++frames || frame.width != 320 || frame.height != 192 ||
                ((nal[0] & 31) != 1 && (nal[0] & 31) != 5)) return 3;
            std::fputc('F', stdout);
            for (uint32_t value : {frames, first, sequence, uint32_t(frame.width),
                                  uint32_t(frame.height), uint32_t(nal[0] & 31)}) write32(value);
            for (uint32_t y = 0; y < frame.height; ++y)
                if (std::fwrite(frame.y + y * frame.stride, 1, frame.width, stdout) != frame.width) return 2;
            first = sequence + 1;
        }
        ++sequence;
    }
    std::fputc('E', stdout); write32(sequence); write32(frames);
    g2_h264_destroy(decoder); std::free(memory);
    return !frames || std::ferror(stdout) ? 3 : 0;
}
