#ifndef ESP_PLATFORM
/** Pixel-level hbands analysis — isolate horizontal prediction error.
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include "../components/sub0h264/src/decoder.hpp"
#include "test_fixtures.hpp"
#include <cstdio>

using namespace sub0h264;

TEST_CASE("Hbands pixel dump: 4MB CAVLC 32x32")
{
    auto h264 = getFixture("cavlc_4mb_hbands.h264");
    auto raw = getFixture("cavlc_4mb_hbands_raw.yuv");
    REQUIRE(!h264.empty());
    REQUIRE(!raw.empty());

    H264Decoder dec;
    dec.decodeStream(h264.data(), static_cast<uint32_t>(h264.size()));
    const Frame* f = dec.currentFrame();
    REQUIRE(f != nullptr);

    uint32_t w = 32;
    MESSAGE("=== First 4x4 block (0,0): raw vs ours ===");
    for (uint32_t r = 0; r < 4; ++r)
    {
        char buf[200];
        char* p = buf;
        p += std::snprintf(p, 20, "row %u raw:", r);
        for (uint32_t c = 0; c < 4; ++c)
            p += std::snprintf(p, 6, " %3d", raw[r * w + c]);
        p += std::snprintf(p, 10, "  ours:");
        for (uint32_t c = 0; c < 4; ++c)
            p += std::snprintf(p, 6, " %3d", f->y(c, r));
        p += std::snprintf(p, 10, "  diff:");
        for (uint32_t c = 0; c < 4; ++c)
            p += std::snprintf(p, 6, " %+3d", static_cast<int>(f->y(c, r)) - raw[r * w + c]);
        MESSAGE(buf);
    }

    MESSAGE("=== Second 4x4 block (4,0): raw vs ours ===");
    for (uint32_t r = 0; r < 4; ++r)
    {
        char buf[200];
        char* p = buf;
        p += std::snprintf(p, 20, "row %u raw:", r);
        for (uint32_t c = 4; c < 8; ++c)
            p += std::snprintf(p, 6, " %3d", raw[r * w + c]);
        p += std::snprintf(p, 10, "  ours:");
        for (uint32_t c = 4; c < 8; ++c)
            p += std::snprintf(p, 6, " %3d", f->y(c, r));
        p += std::snprintf(p, 10, "  diff:");
        for (uint32_t c = 4; c < 8; ++c)
            p += std::snprintf(p, 6, " %+3d", static_cast<int>(f->y(c, r)) - raw[r * w + c]);
        MESSAGE(buf);
    }

    MESSAGE("=== Third 4x4 block (8,0): raw vs ours ===");
    for (uint32_t r = 0; r < 4; ++r)
    {
        char buf[200];
        char* p = buf;
        p += std::snprintf(p, 20, "row %u raw:", r);
        for (uint32_t c = 8; c < 12; ++c)
            p += std::snprintf(p, 6, " %3d", raw[r * w + c]);
        p += std::snprintf(p, 10, "  ours:");
        for (uint32_t c = 8; c < 12; ++c)
            p += std::snprintf(p, 6, " %3d", f->y(c, r));
        p += std::snprintf(p, 10, "  diff:");
        for (uint32_t c = 8; c < 12; ++c)
            p += std::snprintf(p, 6, " %+3d", static_cast<int>(f->y(c, r)) - raw[r * w + c]);
        MESSAGE(buf);
    }

    // Row 0 across all 8 4x4 blocks (32 pixels wide)
    MESSAGE("=== Row 0 across full width: per-block column 0 ===");
    char buf[200];
    char* p = buf;
    p += std::snprintf(p, 10, "raw: ");
    for (uint32_t c = 0; c < 32; c += 4)
        p += std::snprintf(p, 6, " %3d", raw[c]);
    MESSAGE(buf);

    p = buf;
    p += std::snprintf(p, 10, "ours:");
    for (uint32_t c = 0; c < 32; c += 4)
        p += std::snprintf(p, 6, " %3d", f->y(c, 0));
    MESSAGE(buf);

    p = buf;
    p += std::snprintf(p, 10, "diff:");
    for (uint32_t c = 0; c < 32; c += 4)
        p += std::snprintf(p, 6, " %+3d", static_cast<int>(f->y(c, 0)) - raw[c]);
    MESSAGE(buf);
}

#endif // ESP_PLATFORM
