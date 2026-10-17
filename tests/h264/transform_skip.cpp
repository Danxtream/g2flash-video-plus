/* Count-aware inverse transform equivalence on synthetic blocks.
 * SPDX-License-Identifier: MIT */
#include <cstring>
#include "transform.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <random>

static void check(const std::array<int16_t, 16>& coeffs, bool nonzero,
                  uint32_t predStride, uint32_t outStride, std::mt19937& random)
{
    std::array<uint8_t, 128> pred, expected, actual;
    for (auto& p : pred) p = static_cast<uint8_t>(random());
    expected.fill(0xa5);
    actual = expected;
    sub0h264::inverseDct4x4AddPred(coeffs.data(), pred.data(), predStride,
                                 expected.data(), outStride);
    sub0h264::inverseDct4x4AddPred(coeffs.data(), pred.data(), predStride,
                                 actual.data(), outStride, nonzero);
    if (expected != actual) std::abort();
    // In-place reconstruction is used by some decoder paths.
    expected = pred;
    actual = pred;
    sub0h264::inverseDct4x4AddPred(coeffs.data(), expected.data(), predStride,
                                 expected.data(), predStride);
    sub0h264::inverseDct4x4AddPred(coeffs.data(), actual.data(), predStride,
                                 actual.data(), predStride, nonzero);
    if (expected != actual) std::abort();
}

int main()
{
    std::mt19937 random(0x4dc700);
    std::array<int16_t, 16> coeffs{};
    for (uint32_t ps : {4U, 7U, 16U, 31U})
        for (uint32_t os : {4U, 9U, 16U, 32U})
            for (unsigned n = 0; n < 256; ++n) check(coeffs, false, ps, os, random);
    // Every signed DC value, including wrap of the transform rounding bias.
    for (int dc = -32768; dc <= 32767; ++dc)
    {
        coeffs[0] = static_cast<int16_t>(dc);
        check(coeffs, dc != 0, 16, 32, random);
    }
    for (unsigned n = 0; n < 100000; ++n)
    {
        bool nonzero = false;
        for (auto& c : coeffs)
        {
            c = static_cast<int16_t>(random());
            nonzero |= c != 0;
        }
        check(coeffs, nonzero, 7, 9, random);
    }
    std::puts("Transform skip: zero, exhaustive DC, random and in-place PASS");
}
