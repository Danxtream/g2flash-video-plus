/* Synthetic Baseline stream for the real worker's allocation paths.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/h264/g2_h264.h"
#include <cstdint>
#include <cstdlib>

struct Bits {
    uint8_t bytes[2048] = {};
    uint32_t count = 0;
    explicit Bits(uint8_t header) { bits(header, 8); }
    void bits(uint32_t value, uint32_t width) {
        for (uint32_t i = width; i; --i) {
            if (count == sizeof(bytes) * 8) std::abort();
            bytes[count / 8] |= ((value >> (i - 1)) & 1) << (7 - count % 8);
            ++count;
        }
    }
    void ue(uint32_t value) {
        uint32_t code = value + 1, width = 0;
        for (uint32_t v = code; v; v >>= 1) ++width;
        bits(0, width - 1); bits(code, width);
    }
    void se(int32_t value) { ue(value <= 0 ? uint32_t(-value * 2) : uint32_t(value * 2 - 1)); }
    uint32_t finish() { bits(1, 1); return (count + 7) / 8; }
};

extern "C" void worker_finish_call(void);
extern "C" int worker_upload_nal(const uint8_t *, uint32_t, uint32_t);
static int emit(void *handle, Bits b, uint32_t sequence, g2_h264_result expected, bool queued) {
    uint32_t bytes = b.finish();
    if (queued) return worker_upload_nal(b.bytes, bytes, sequence);
    if (g2_h264_decode(handle, b.bytes, bytes) != expected) return 0;
    worker_finish_call();
    return 1;
}
static int synthetic_stream(void *handle, bool queued) {
    Bits sps(0x67);
    sps.bits(66, 8); sps.bits(0, 8); sps.bits(30, 8);
    sps.ue(0); sps.ue(0); sps.ue(2); sps.ue(1);
    sps.bits(0, 1); sps.ue(19); sps.ue(11);
    sps.bits(1, 1); sps.bits(1, 1); sps.bits(0, 1); sps.bits(0, 1);
    if (!emit(handle, sps, 0, G2_H264_CONSUMED, queued)) return 0;
    Bits pps(0x68);
    pps.ue(0); pps.ue(0); pps.bits(0, 1); pps.bits(0, 1); pps.ue(0);
    pps.ue(0); pps.ue(0); pps.bits(0, 1); pps.bits(0, 2);
    pps.se(0); pps.se(0); pps.se(0);
    pps.bits(1, 1); pps.bits(0, 1); pps.bits(0, 1);
    if (!emit(handle, pps, 1, G2_H264_CONSUMED, queued)) return 0;
    for (uint32_t n = 0; n < 32; ++n) {
        Bits b(n ? 0x41 : 0x65);
        b.ue(0); b.ue(n ? 0 : 2); b.ue(0); b.bits(n % 16, 4);
        if (!n) { b.ue(0); b.bits(0, 1); b.bits(0, 1); }
        else { b.bits(0, 1); b.bits(0, 1); b.bits(0, 1); }
        b.se(0); b.ue(1);
        if (n) b.ue(240);
        else for (uint32_t i = 0; i < 240; ++i) {
            b.ue(3); b.ue(0); b.se(0); b.bits(1, 1);
        }
        if (!emit(handle, b, n + 2, G2_H264_FRAME_READY, queued)) return 0;
        if (queued) continue;
        g2_h264_frame_info frame = {};
        if (g2_h264_frame(handle, &frame) != G2_H264_FRAME_READY ||
            frame.width != 320 || frame.height != 192 || frame.count != n + 1) return 0;
        for (uint32_t y = 0; y < frame.height; ++y)
            for (uint32_t x = 0; x < frame.width; ++x)
                if (frame.y[y * frame.stride + x] != 128) return 0;
    }
    return 1;
}

extern "C" int worker_test_stream(void *handle) { return synthetic_stream(handle, false); }
extern "C" int worker_enqueue_stream() { return synthetic_stream(nullptr, true); }
