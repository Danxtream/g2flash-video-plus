/* Integer MVE luma kernels; scalar targets keep their C implementations.
 * SPDX-License-Identifier: MIT */
#ifndef CROG_SUB0H264_LUMA_MVE_HPP
#define CROG_SUB0H264_LUMA_MVE_HPP

#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE & 1)
#include <arm_mve.h>

namespace sub0h264 {

/** Luma is unsigned even though the filter's arithmetic is signed. */
__attribute__((always_inline)) inline int32x4_t lumaMotionCompMveLoad(
    const uint8_t* src, mve_pred16_t lanes) noexcept
{
    return vreinterpretq_s32_u32(vldrbq_z_u32(src, lanes));
}

/** Keep six-tap arithmetic wide through the second, unnormalized pass. */
__attribute__((always_inline)) inline int32x4_t lumaMotionCompMveFir(
    int32x4_t a, int32x4_t b, int32x4_t c,
    int32x4_t d, int32x4_t e, int32x4_t f) noexcept
{
    return vaddq_s32(vsubq_s32(vaddq_s32(a, f), vmulq_n_s32(vaddq_s32(b, e), 5)),
                     vmulq_n_s32(vaddq_s32(c, d), 20));
}

/** Widen four unsigned samples from each tap without reading unused lanes. */
__attribute__((always_inline)) inline int32x4_t lumaMotionCompMveSamples(
    const uint8_t* first, int32_t step, mve_pred16_t lanes) noexcept
{
    return lumaMotionCompMveFir(lumaMotionCompMveLoad(first, lanes),
        lumaMotionCompMveLoad(first + step, lanes), lumaMotionCompMveLoad(first + 2 * step, lanes),
        lumaMotionCompMveLoad(first + 3 * step, lanes), lumaMotionCompMveLoad(first + 4 * step, lanes),
        lumaMotionCompMveLoad(first + 5 * step, lanes));
}

/** Clip after the prescribed rounding, never between the two filter passes. */
__attribute__((always_inline)) inline int32x4_t lumaMotionCompMveClip(int32x4_t value) noexcept
{
    return vmaxq_s32(vdupq_n_s32(0), vminq_s32(vdupq_n_s32(255), value));
}

/** Four independent columns per lane group, with a scalar-compatible halo. */
inline void lumaMotionCompMve(const uint8_t* src, uint32_t stride,
    uint32_t dx, uint32_t dy, uint32_t width, uint32_t height,
    uint8_t* dst, uint32_t dstStride) noexcept
{
    const int32_t step = static_cast<int32_t>(stride);
    if (dx == 0U || dy == 0U || ((dx & 1U) && (dy & 1U)))
    {
        for (uint32_t row = 0U; row < height; ++row)
        {
            const uint8_t* p = src + row * stride;
            for (uint32_t col = 0U; col < width; col += 4U)
            {
                mve_pred16_t lanes = vctp32q(width - col);
                int32x4_t value;
                if (dy == 0U)
                {
                    auto raw = lumaMotionCompMveSamples(p + col - 2, 1, lanes);
                    value = lumaMotionCompMveClip(vshrq_n_s32(vaddq_n_s32(raw, 16), 5));
                    if (dx != 2U) value = vrhaddq_s32(value, lumaMotionCompMveLoad(p + col + (dx >> 1U), lanes));
                }
                else if (dx == 0U)
                {
                    auto raw = lumaMotionCompMveSamples(p + col - 2 * step, step, lanes);
                    value = lumaMotionCompMveClip(vshrq_n_s32(vaddq_n_s32(raw, 16), 5));
                    if (dy != 2U) value = vrhaddq_s32(value, lumaMotionCompMveLoad(p + (dy >> 1U) * stride + col, lanes));
                }
                else
                {
                    auto h = lumaMotionCompMveSamples(p + (dy >> 1U) * stride + col - 2, 1, lanes);
                    auto v = lumaMotionCompMveSamples(p + col + (dx >> 1U) - 2 * step, step, lanes);
                    value = vrhaddq_s32(lumaMotionCompMveClip(vshrq_n_s32(vaddq_n_s32(h, 16), 5)),
                                        lumaMotionCompMveClip(vshrq_n_s32(vaddq_n_s32(v, 16), 5)));
                }
                vstrbq_p_u32(dst + row * dstStride + col, vreinterpretq_u32_s32(value), lanes);
            }
        }
        return;
    }

    // All first-pass results fit int16_t, but six of them cannot be summed
    // in that width. Widen on load for the vertical pass.
    int16_t horizontal[21][16];
    for (uint32_t row = 0U; row < height + 5U; ++row)
    {
        const uint8_t* p = src + (static_cast<int32_t>(row) - 2) * step;
        for (uint32_t col = 0U; col < width; col += 4U)
        {
            mve_pred16_t lanes = vctp32q(width - col);
            vstrhq_p_s32(horizontal[row] + col, lumaMotionCompMveSamples(p + col - 2, 1, lanes), lanes);
        }
    }
    for (uint32_t row = 0U; row < height; ++row)
        for (uint32_t col = 0U; col < width; col += 4U)
        {
            mve_pred16_t lanes = vctp32q(width - col);
            auto sum = lumaMotionCompMveFir(vldrhq_z_s32(horizontal[row] + col, lanes),
                vldrhq_z_s32(horizontal[row + 1U] + col, lanes),
                vldrhq_z_s32(horizontal[row + 2U] + col, lanes),
                vldrhq_z_s32(horizontal[row + 3U] + col, lanes),
                vldrhq_z_s32(horizontal[row + 4U] + col, lanes),
                vldrhq_z_s32(horizontal[row + 5U] + col, lanes));
            auto value = lumaMotionCompMveClip(vshrq_n_s32(vaddq_n_s32(sum, 512), 10));
            if (dy != 2U)
            {
                auto h = vldrhq_z_s32(horizontal[row + 2U + (dy >> 1U)] + col, lanes);
                value = vrhaddq_s32(value, lumaMotionCompMveClip(vshrq_n_s32(vaddq_n_s32(h, 16), 5)));
            }
            else if (dx != 2U)
            {
                auto v = lumaMotionCompMveSamples(src + row * stride + col + (dx >> 1U) - 2 * step, step, lanes);
                value = vrhaddq_s32(value, lumaMotionCompMveClip(vshrq_n_s32(vaddq_n_s32(v, 16), 5)));
            }
            vstrbq_p_u32(dst + row * dstStride + col, vreinterpretq_u32_s32(value), lanes);
        }
}

} // namespace sub0h264
#endif
#endif
