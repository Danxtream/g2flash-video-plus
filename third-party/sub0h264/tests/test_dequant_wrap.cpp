/** Dequantization must retain 32-bit wraparound before signed rounding.
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include <cstring>
#include "../components/sub0h264/src/transform.hpp"

using namespace sub0h264;

/** Wide arithmetic models the old low-bit result without overflowing itself. */
static int16_t roundedProduct(int16_t coefficient, int32_t multiplier)
{
    int64_t wide = static_cast<int64_t>(coefficient) * multiplier + 32;
    return static_cast<int16_t>(static_cast<int32_t>(static_cast<uint32_t>(wide)) >> 6);
}

TEST_CASE("4x4 dequantization preserves overflowing products and rounding")
{
    // QP 51 at (0,0) has qmul 229376: -9856*qmul is below INT32_MIN.
    int16_t regression[16] = {-9856};
    inverseQuantize4x4(regression, 51);
    CHECK(regression[0] == roundedProduct(-9856, 229376));

    for (int32_t qp = 0; qp <= 51; ++qp)
    {
        for (int16_t coefficient : {int16_t(INT16_MIN), int16_t(-9856), int16_t(-1),
                                    int16_t(0), int16_t(1), int16_t(9856), int16_t(INT16_MAX)})
        {
            int16_t flat[16], scaled[16], weights[16];
            std::fill_n(flat, 16, coefficient);
            inverseQuantize4x4(flat, qp);
            for (uint32_t i = 0; i < 16; ++i)
            {
                int32_t multiplier = cDequantScale[qp % 6][cDequantPosClass[i]] *
                                     16 << (qp / 6 + 2);
                CHECK(flat[i] == roundedProduct(coefficient, multiplier));
            }
            for (int16_t weight : {int16_t(1), int16_t(16), int16_t(255)})
            {
                std::fill_n(scaled, 16, coefficient);
                std::fill_n(weights, 16, weight);
                inverseQuantize4x4Scaled(scaled, qp, weights);
                for (uint32_t i = 0; i < 16; ++i)
                {
                    int32_t multiplier = cDequantScale[qp % 6][cDequantPosClass[i]] *
                                         weight << (qp / 6 + 2);
                    CHECK(scaled[i] == roundedProduct(coefficient, multiplier));
                }
            }
        }
    }
}

TEST_CASE("DC and 8x8 dequantization retain bounded signed arithmetic")
{
    for (int32_t qp = 0; qp <= 51; ++qp)
    {
        for (int16_t coefficient : {int16_t(INT16_MIN), int16_t(-1), int16_t(0),
                                    int16_t(1), int16_t(INT16_MAX)})
        {
            int16_t dc[16];
            std::fill_n(dc, 16, coefficient);
            inverseQuantize4x4(dc, qp, true);
            for (uint32_t i = 0; i < 16; ++i)
                CHECK(dc[i] == static_cast<int16_t>(static_cast<int64_t>(coefficient) *
                    cDequantScale[qp % 6][cDequantPosClass[i]] * (int64_t(1) << (qp / 6))));
            int16_t flat[64];
            std::fill_n(flat, 64, coefficient);
            inverseQuantize8x8(flat, qp);
            for (uint32_t i = 0; i < 64; ++i)
            {
                int64_t value = static_cast<int64_t>(coefficient) *
                    cDequantScale8x8[qp % 6][cDequantPosClass8x8[i]];
                value = qp / 6 >= 2 ? value * (int64_t(1) << (qp / 6 - 2))
                    : (value + (int64_t(1) << (1 - qp / 6))) >> (2 - qp / 6);
                CHECK(flat[i] == static_cast<int16_t>(value));
            }
            for (int16_t weight : {int16_t(1), int16_t(16), int16_t(255)})
            {
                int16_t block[64], weights[64];
                std::fill_n(block, 64, coefficient);
                std::fill_n(weights, 64, weight);
                inverseQuantize8x8Scaled(block, qp, weights);
                for (uint32_t i = 0; i < 64; ++i)
                {
                    int64_t value = static_cast<int64_t>(coefficient) *
                        cDequantScale8x8[qp % 6][cDequantPosClass8x8[i]] * weight;
                    value = qp / 6 >= 6 ? value * (int64_t(1) << (qp / 6 - 6))
                        : (value + (int64_t(1) << (5 - qp / 6))) >> (6 - qp / 6);
                    CHECK(block[i] == static_cast<int16_t>(value));
                }
            }
        }
    }
}
