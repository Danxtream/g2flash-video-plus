/* Emit every Y byte for an independent whole/partitioned comparison.
 * SPDX-License-Identifier: GPL-3.0-only */
#include "../../patches/h264/g2_h264.h"
#include "../../patches/h264/runtime.h"
#include <cstdio>
#include <cstdlib>

extern "C" g2_h264_result h264_partition_test_decode(void *, const uint8_t *, uint32_t);

static void *allocations[2048] = {};
[[noreturn]] static void error() { std::exit(1); }
static void *allocate(uint32_t size) {
    void *memory = std::malloc(size);
    if (!memory) error();
    for (auto &slot : allocations) if (!slot) {
        slot = memory;
        return memory;
    }
    error();
}
static void release(void *memory) {
    for (auto &slot : allocations) if (slot == memory) {
        std::free(memory);
        slot = nullptr;
        return;
    }
    error();
}
static int preflight(const g2_h264_request *, uint32_t, g2_h264_failure *) { return 1; }
static void fatal(uint32_t) { error(); }
static const g2_h264_runtime callbacks = {allocate, release, preflight, fatal};
extern "C" const g2_h264_runtime *g2_h264_runtime_current() { return &callbacks; }

/* Find Annex-B prefixes independently of decoder parsing. Both prefix lengths
 * occur in the fixtures, and input NALs are counted separately from pictures. */
static uint32_t start(const uint8_t *data, uint32_t size, uint32_t from,
                      uint32_t &prefix) {
    for (uint32_t i = from; i + 2 < size; ++i) {
        if (data[i] || data[i + 1]) continue;
        if (data[i + 2] == 1) { prefix = 3; return i; }
        if (i + 3 < size && !data[i + 2] && data[i + 3] == 1) {
            prefix = 4;
            return i;
        }
    }
    prefix = 0;
    return size;
}

int main(int argc, char **argv) {
    if (argc != 2) error();
    FILE *file = std::fopen(argv[1], "rb");
    if (!file || std::fseek(file, 0, SEEK_END)) error();
    long length = std::ftell(file);
    if (length <= 0 || length >= 30000000) error();
    std::rewind(file);
    auto *data = static_cast<uint8_t *>(std::malloc(length));
    if (!data || std::fread(data, 1, length, file) != static_cast<size_t>(length)) error();
    std::fclose(file);
    void *memory = std::malloc(g2_h264_size());
    void *handle = g2_h264_init(memory, g2_h264_size());
    if (!handle) error();
    uint32_t nals = 0, frames = 0, prefix = 0;
    uint32_t position = start(data, length, 0, prefix);
    while (position < static_cast<uint32_t>(length)) {
        uint32_t begin = position + prefix, nextPrefix = 0;
        uint32_t end = start(data, length, begin, nextPrefix);
        uint32_t stop = end;
        while (stop > begin && !data[stop - 1]) --stop;
        if (stop == begin) error();
        auto status = h264_partition_test_decode(handle, data + begin, stop - begin);
        if (status == G2_H264_ERROR) error();
        ++nals;
        if (status == G2_H264_FRAME_READY) {
            g2_h264_frame_info info = {};
            if (g2_h264_frame(handle, &info) != G2_H264_FRAME_READY ||
                !info.y || info.count != ++frames || !info.width || !info.height) error();
            uint32_t header[] = {frames, info.width, info.height};
            if (std::fwrite(header, sizeof(header), 1, stdout) != 1) error();
            for (uint32_t y = 0; y < info.height; ++y)
                if (std::fwrite(info.y + y * info.stride, 1, info.width, stdout) != info.width) error();
        }
        position = end;
        prefix = nextPrefix;
    }
    if (!frames || nals <= frames) error();
    g2_h264_destroy(handle);
    std::free(memory);
    std::free(data);
    for (void *allocation : allocations) if (allocation) error();
    std::fprintf(stderr, "%u NALs, %u pictures, all allocations released\n", nals, frames);
}
