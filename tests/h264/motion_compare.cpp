/* Exhaustive fractional positions and synthetic edge/random comparisons.
 * SPDX-License-Identifier: MIT */
#include "inter_pred.hpp"
#include "motion_oracle.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>

static unsigned comparisons = 0;
static void check(const sub0h264::Frame& frame, int x, int y, unsigned w,
                  unsigned h, unsigned dx, unsigned dy, unsigned stride)
{
    std::array<uint8_t, 32 * 20> expected, actual;
    expected.fill(0xa5);
    actual = expected;
    sub0h264::referenceLumaMotionComp(frame, x, y, dx, dy, w, h, expected.data(), stride);
    sub0h264::lumaMotionComp(frame, x, y, dx, dy, w, h, actual.data(), stride);
    if (actual != expected)
    {
        std::fprintf(stderr, "MC mismatch %d,%d frac %u,%u block %u,%u stride %u\n", x,y,dx,dy,w,h,stride);
        std::abort();
    }
    ++comparisons;
}

int main()
{
    std::mt19937 random(0x6a09e667);
    constexpr unsigned shapes[][2] = {{4,4},{4,8},{8,4},{8,8},{8,16},{16,8},{16,16}};
    constexpr int coords[] = {-20,-3,-2,-1,0,1,2,3,15,16,29,30,31,32,33,50};
    sub0h264::Frame frame;
    if (!frame.allocate(32,32,true)) return 2;
    for (unsigned pattern = 0; pattern < 7; ++pattern)
    {
        for (unsigned y = 0; y < 32; ++y) for (unsigned x = 0; x < 32; ++x)
            frame.y(x,y) = static_cast<uint8_t>(pattern < 5 ? (pattern == 0 ? 0 : pattern == 1 ? 1 :
                pattern == 2 ? 127 : pattern == 3 ? 254 : 255) : pattern == 5 ? ((x+y)&1)*255 : random());
        for (const auto& shape : shapes) for (int y : coords) for (int x : coords)
            for (unsigned dy = 0; dy < 4; ++dy) for (unsigned dx = 0; dx < 4; ++dx)
                check(frame,x,y,shape[0],shape[1],dx,dy,shape[0]+3);
    }
    for (unsigned size : {1U,2U,4U,17U,64U})
    {
        if (!frame.allocate(size,size,true)) return 2;
        for (unsigned y = 0; y < size; ++y) for (unsigned x = 0; x < size; ++x)
            frame.y(x,y) = static_cast<uint8_t>(random());
        for (unsigned n = 0; n < 3000; ++n)
        {
            const auto& shape = shapes[random()%7];
            check(frame, int(random()%(size+40))-20, int(random()%(size+40))-20,
                  shape[0],shape[1],random()%4,random()%4,32);
        }
    }
    std::printf("MC original C oracle: %u comparisons PASS\n", comparisons);
}
