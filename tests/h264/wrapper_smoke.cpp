/* Exercise the real C boundary under ASan/UBSan, without a firmware.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/h264/g2_h264.h"
#include "../../patches/h264/runtime.h"
#include "../../patches/h264/config.hpp"
#include "decoder.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "check failed at %d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (0)

static bool bound = true, failAlloc = false;
static uint32_t refusedTag = 0, requestsSeen = 0, chromaRequests = 0;
static uint32_t allocations = 0, released = 0, live = 0;
struct Allocation { void *memory; uint32_t size; };
static Allocation storage[1024] = {};

static void *allocate(uint32_t size) {
    if (failAlloc) return nullptr;
    void *p = std::malloc(size);
    CHECK(p);
    for (auto &slot : storage) if (!slot.memory) {
        slot = {p, size};
        ++allocations;
        ++live;
        return p;
    }
    CHECK(false);
    return nullptr;
}
static void release(void *memory) {
    for (auto &slot : storage) if (slot.memory == memory) {
        std::free(memory);
        slot = {};
        ++released;
        --live;
        return;
    }
    CHECK(false);
}
static int preflight(const g2_h264_request *requests, uint32_t count,
                     g2_h264_failure *failure) {
    CHECK(count <= 8);
    for (uint32_t i = 0; i < count; ++i) {
        ++requestsSeen;
        if (requests[i].tag == 3 || requests[i].tag == 4) ++chromaRequests;
        if (requests[i].tag == refusedTag) {
            *failure = {requests[i].tag, requests[i].size};
            return 0;
        }
    }
    return 1;
}
static void fatal(uint32_t reason) { std::_Exit(70 + reason); }
static g2_h264_runtime callbacks = {allocate, release, preflight, fatal};
extern "C" const g2_h264_runtime *g2_h264_runtime_current() {
    return bound ? &callbacks : nullptr;
}

/* Generate a flat 16x16 Baseline IDR and P-skip picture, per H.264 7.3.
 * This is a synthetic syntax fixture, not a copied/compressed video asset. */
struct Bits {
    uint8_t bytes[128] = {};
    uint32_t count = 0;
    explicit Bits(uint8_t header) { bits(header, 8); }
    void bits(uint32_t value, uint32_t width) {
        for (uint32_t i = width; i; --i) {
            CHECK(count < sizeof(bytes) * 8);
            bytes[count / 8] |= ((value >> (i - 1)) & 1) << (7 - count % 8);
            ++count;
        }
    }
    void ue(uint32_t value) {
        uint32_t code = value + 1, width = 0;
        for (uint32_t v = code; v; v >>= 1) ++width;
        bits(0, width - 1);
        bits(code, width);
    }
    void se(int32_t value) { ue(value <= 0 ? uint32_t(-value * 2) : uint32_t(value * 2 - 1)); }
    uint32_t finish() { bits(1, 1); return (count + 7) / 8; }
};
static Bits sps() {
    Bits b(0x67);
    b.bits(66, 8); b.bits(0, 8); b.bits(10, 8);
    b.ue(0); b.ue(0); b.ue(2); b.ue(1);
    b.bits(0, 1); b.ue(0); b.ue(0);
    b.bits(1, 1); b.bits(1, 1); b.bits(0, 1); b.bits(0, 1);
    return b;
}
static Bits pps() {
    Bits b(0x68);
    b.ue(0); b.ue(0); b.bits(0, 1); b.bits(0, 1); b.ue(0);
    b.ue(0); b.ue(0); b.bits(0, 1); b.bits(0, 2);
    b.se(0); b.se(0); b.se(0);
    b.bits(1, 1); b.bits(0, 1); b.bits(0, 1);
    return b;
}
static Bits idr() {
    Bits b(0x65);
    b.ue(0); b.ue(2); b.ue(0); b.bits(0, 4); b.ue(0);
    b.bits(0, 1); b.bits(0, 1); b.se(0); b.ue(1);
    b.ue(3); b.ue(0); b.se(0); b.bits(1, 1);
    return b;
}
static Bits predicted() {
    Bits b(0x41);
    b.ue(0); b.ue(0); b.ue(0); b.bits(1, 4);
    b.bits(0, 1); b.bits(0, 1); b.bits(0, 1); b.se(0); b.ue(1); b.ue(1);
    return b;
}
struct Handle {
    void *memory = std::malloc(g2_h264_size());
    void *value;
    Handle() : value(g2_h264_init(memory, g2_h264_size())) { CHECK(value); }
    ~Handle() { g2_h264_destroy(value); std::free(memory); }
};
static g2_h264_result feed(void *handle, Bits b) {
    uint32_t size = b.finish();
    return g2_h264_decode(handle, b.bytes, size);
}

static void lifecycle() {
    CHECK(g2_h264_size() <= G2_H264_RECORD_BYTES);
    CHECK(g2_h264_alignment() && !(g2_h264_alignment() & (g2_h264_alignment() - 1)));
    void *memory = std::malloc(g2_h264_size() + g2_h264_alignment());
    CHECK(memory);
    CHECK(!g2_h264_init(nullptr, g2_h264_size()));
    CHECK(!g2_h264_init(memory, g2_h264_size() - 1));
    CHECK(!g2_h264_init(static_cast<uint8_t *>(memory) + 1, g2_h264_size()));
    bound = false;
    CHECK(!g2_h264_init(memory, g2_h264_size()));
    bound = true;
    g2_h264_runtime saved = callbacks;
    callbacks.alloc = nullptr; CHECK(!g2_h264_init(memory, g2_h264_size())); callbacks = saved;
    callbacks.release = nullptr; CHECK(!g2_h264_init(memory, g2_h264_size())); callbacks = saved;
    callbacks.preflight = nullptr; CHECK(!g2_h264_init(memory, g2_h264_size())); callbacks = saved;
    callbacks.fail = nullptr; CHECK(!g2_h264_init(memory, g2_h264_size())); callbacks = saved;
    CHECK(!allocations);
    std::free(memory);
    g2_h264_destroy(nullptr);
    g2_h264_frame_info info = {reinterpret_cast<uint8_t *>(1), 1, 1, 1, 1};
    CHECK(g2_h264_frame(nullptr, &info) == G2_H264_ERROR);
    CHECK(!info.y && !info.count);
    CHECK(g2_h264_frame(nullptr, nullptr) == G2_H264_ERROR);
    {
        Handle h;
        CHECK(live == 1);
        CHECK(g2_h264_frame(h.value, &info) == G2_H264_ERROR);
        uint8_t forbidden[] = {0x80};
        CHECK(g2_h264_decode(h.value, forbidden, 1) == G2_H264_ERROR);
        CHECK(g2_h264_decode(h.value, nullptr, 1) == G2_H264_ERROR);
        CHECK(g2_h264_decode(h.value, forbidden, 0) == G2_H264_ERROR);
        CHECK(g2_h264_decode(nullptr, forbidden, 1) == G2_H264_ERROR);
        uint8_t maximum[G2_H264_MAX_NAL_BYTES + 1] = {0x09};
        uint32_t before = allocations;
        CHECK(g2_h264_decode(h.value, maximum, G2_H264_MAX_NAL_BYTES) == G2_H264_CONSUMED);
        CHECK(allocations == before);
        CHECK(g2_h264_decode(h.value, maximum, sizeof(maximum)) == G2_H264_ERROR);
        CHECK(feed(h.value, sps()) == G2_H264_CONSUMED);
        CHECK(feed(h.value, pps()) == G2_H264_CONSUMED);
        CHECK(g2_h264_frame(h.value, &info) == G2_H264_ERROR);
        CHECK(feed(h.value, idr()) == G2_H264_FRAME_READY);
        CHECK(g2_h264_frame(h.value, &info) == G2_H264_FRAME_READY);
        CHECK(info.width == 16 && info.height == 16 && info.stride >= 16 && info.count == 1);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x) CHECK(info.y[y * info.stride + x] == 128);
        CHECK(feed(h.value, predicted()) == G2_H264_FRAME_READY);
        CHECK(g2_h264_frame(h.value, &info) == G2_H264_FRAME_READY && info.count == 2);
        for (uint32_t y = 0; y < 16; ++y)
            for (uint32_t x = 0; x < 16; ++x) CHECK(info.y[y * info.stride + x] == 128);
        CHECK(g2_h264_decode(h.value, forbidden, 1) == G2_H264_ERROR);
        CHECK(g2_h264_frame(h.value, &info) == G2_H264_ERROR && !info.y);
    }
    CHECK(!live && allocations == released && requestsSeen && !chromaRequests);
}

static void refusal(uint32_t tag) {
    uint32_t before = live;
    {
        Handle h;
        CHECK(feed(h.value, sps()) == G2_H264_CONSUMED);
        CHECK(feed(h.value, pps()) == G2_H264_CONSUMED);
        refusedTag = tag;
        CHECK(feed(h.value, idr()) == G2_H264_ERROR);
        g2_h264_frame_info info = {};
        CHECK(g2_h264_frame(h.value, &info) == G2_H264_ERROR && !info.y);
        refusedTag = 0;
    }
    CHECK(live == before);
}

/* Optional ignored clip path comes from argv. Compare every emitted byte with
 * the same unwrapped decoder, independently tracking NAL and picture counts. */
static void clip(const char *path) {
    FILE *file = std::fopen(path, "rb");
    CHECK(file);
    CHECK(!std::fseek(file, 0, SEEK_END));
    long size = std::ftell(file);
    CHECK(size > 0 && size < 30000000);
    std::rewind(file);
    auto *data = static_cast<uint8_t *>(std::malloc(size));
    CHECK(data && std::fread(data, 1, size, file) == static_cast<size_t>(size));
    std::fclose(file);
    uint32_t frames = 0, nals = 0;
    {
        std::vector<sub0h264::NalBounds> bounds;
        sub0h264::findNalUnits(data, static_cast<uint32_t>(size), bounds);
        CHECK(!bounds.empty());
        Handle h;
        sub0h264::H264Decoder reference(sub0h264::H264Decoder::Settings{true});
        sub0h264::NalUnit parsed;
        for (const auto &b : bounds) {
            CHECK(b.size <= G2_H264_MAX_NAL_BYTES);
            CHECK(sub0h264::parseNalUnit(data + b.offset, b.size, parsed));
            auto expected = reference.processNal(parsed);
            auto actual = g2_h264_decode(h.value, data + b.offset, b.size);
            CHECK(static_cast<int>(expected) == static_cast<int>(actual));
            CHECK(actual != G2_H264_ERROR);
            ++nals;
            g2_h264_frame_info info = {};
            if (actual == G2_H264_FRAME_READY) {
                ++frames;
                CHECK(g2_h264_frame(h.value, &info) == G2_H264_FRAME_READY);
                const auto *frame = reference.currentFrame();
                CHECK(frame && !frame->uData() && !frame->vData());
                CHECK(info.count == frames && info.width == frame->width() &&
                      info.height == frame->height() && info.stride == frame->yStride());
                for (uint32_t y = 0; y < info.height; ++y)
                    CHECK(!std::memcmp(info.y + y * info.stride,
                                      frame->yData() + y * frame->yStride(), info.width));
            } else {
                CHECK(g2_h264_frame(h.value, &info) == G2_H264_ERROR && !info.y);
            }
        }
    }
    std::free(data);
    CHECK(frames && nals > frames && !live && !chromaRequests);
    std::printf("%s: %u NALs, %u pictures, all Y bytes identical\n", path, nals, frames);
}

int main(int argc, char **argv) {
    if (argc == 2 && !std::strcmp(argv[1], "--fatal")) {
        failAlloc = true;
        Handle h;
        CHECK(false);
    }
    lifecycle();
    refusal(1);  // DPB metadata.
    refusal(2);  // Y plane.
    refusal(5);  // Macroblock context batch.
    for (int i = 1; i < argc; ++i) clip(argv[i]);
    CHECK(!live && allocations == released);
    std::printf("C interface PASS: size %u, align %u, %u allocations released\n",
                g2_h264_size(), g2_h264_alignment(), allocations);
}
