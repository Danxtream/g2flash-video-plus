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

/** Gather four independent edge crossings in either panel orientation. */
__attribute__((always_inline)) inline int32x4_t filterLumaMveLoad(
    const uint8_t* p, uint32x4_t offsets) noexcept
{
    return vreinterpretq_s32_u32(vldrbq_gather_offset_u32(p, offsets));
}

/** Clip signed filter results before the narrowing byte store. */
__attribute__((always_inline)) inline int32x4_t filterLumaMveClip(int32x4_t value) noexcept
{
    return vmaxq_s32(vdupq_n_s32(0), vminq_s32(vdupq_n_s32(255), value));
}

/** Clamp the normal-filter delta symmetrically around zero. */
__attribute__((always_inline)) inline int32x4_t filterLumaMveClamp(
    int32x4_t value, int32x4_t limit) noexcept
{
    return vmaxq_s32(vnegq_s32(limit), vminq_s32(limit, value));
}

/** Normal luma filtering of four crossings sharing a boundary strength. */
inline void filterLumaMveWeak(uint8_t* q, uint32_t across, uint32_t along,
    int32_t alpha, int32_t beta, int32_t tc0) noexcept
{
    if (alpha == 0 || beta == 0) return;
    auto offsets = vmulq_n_u32(vidupq_n_u32(0, 1), along);
    auto p0 = filterLumaMveLoad(q - across, offsets);
    auto p1 = filterLumaMveLoad(q - 2U * across, offsets);
    auto p2 = filterLumaMveLoad(q - 3U * across, offsets);
    auto q0 = filterLumaMveLoad(q, offsets);
    auto q1 = filterLumaMveLoad(q + across, offsets);
    auto q2 = filterLumaMveLoad(q + 2U * across, offsets);
    mve_pred16_t enabled = vcmpltq_n_s32(vabdq_s32(p0, q0), alpha) &
        vcmpltq_n_s32(vabdq_s32(q1, q0), beta) & vcmpltq_n_s32(vabdq_s32(p1, p0), beta);
    if (!enabled) return;
    mve_pred16_t ap = vcmpltq_n_s32(vabdq_s32(p2, p0), beta);
    mve_pred16_t aq = vcmpltq_n_s32(vabdq_s32(q2, q0), beta);
    auto tc = vaddq_n_s32(vaddq_s32(vpselq_s32(vdupq_n_s32(1), vdupq_n_s32(0), ap),
        vpselq_s32(vdupq_n_s32(1), vdupq_n_s32(0), aq)), tc0);
    auto delta = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vmulq_n_s32(vsubq_s32(q0, p0), 4),
        vsubq_s32(p1, q1)), 4), 3);
    delta = filterLumaMveClamp(delta, tc);
    auto average = vrhaddq_s32(p0, q0);
    auto dp1 = vshrq_n_s32(vsubq_s32(vaddq_s32(p2, average), vmulq_n_s32(p1, 2)), 1);
    auto dq1 = vshrq_n_s32(vsubq_s32(vaddq_s32(q2, average), vmulq_n_s32(q1, 2)), 1);
    dp1 = filterLumaMveClamp(dp1, vdupq_n_s32(tc0));
    dq1 = filterLumaMveClamp(dq1, vdupq_n_s32(tc0));
    auto outP0 = vpselq_s32(filterLumaMveClip(vaddq_s32(p0, delta)), p0, enabled);
    auto outQ0 = vpselq_s32(filterLumaMveClip(vsubq_s32(q0, delta)), q0, enabled);
    auto outP1 = vpselq_s32(filterLumaMveClip(vaddq_s32(p1, dp1)), p1, enabled & ap);
    auto outQ1 = vpselq_s32(filterLumaMveClip(vaddq_s32(q1, dq1)), q1, enabled & aq);
    vstrbq_scatter_offset_u32(q - across, offsets, vreinterpretq_u32_s32(outP0));
    vstrbq_scatter_offset_u32(q, offsets, vreinterpretq_u32_s32(outQ0));
    vstrbq_scatter_offset_u32(q - 2U * across, offsets, vreinterpretq_u32_s32(outP1));
    vstrbq_scatter_offset_u32(q + across, offsets, vreinterpretq_u32_s32(outQ1));
}

/** Strong luma filtering; each side uses the original, unmodified samples. */
inline void filterLumaMveStrong(uint8_t* q, uint32_t across, uint32_t along,
    int32_t alpha, int32_t beta) noexcept
{
    if (alpha == 0 || beta == 0) return;
    auto offsets = vmulq_n_u32(vidupq_n_u32(0, 1), along);
    auto p0 = filterLumaMveLoad(q - across, offsets);
    auto p1 = filterLumaMveLoad(q - 2U * across, offsets);
    auto p2 = filterLumaMveLoad(q - 3U * across, offsets);
    auto p3 = filterLumaMveLoad(q - 4U * across, offsets);
    auto q0 = filterLumaMveLoad(q, offsets);
    auto q1 = filterLumaMveLoad(q + across, offsets);
    auto q2 = filterLumaMveLoad(q + 2U * across, offsets);
    auto q3 = filterLumaMveLoad(q + 3U * across, offsets);
    auto difference = vabdq_s32(p0, q0);
    mve_pred16_t enabled = vcmpltq_n_s32(difference, alpha) &
        vcmpltq_n_s32(vabdq_s32(q1, q0), beta) & vcmpltq_n_s32(vabdq_s32(p1, p0), beta);
    if (!enabled) return;
    mve_pred16_t strong = enabled & vcmpltq_n_s32(difference, (alpha >> 2) + 2);
    mve_pred16_t ps = strong & vcmpltq_n_s32(vabdq_s32(p2, p0), beta);
    mve_pred16_t qs = strong & vcmpltq_n_s32(vabdq_s32(q2, q0), beta);
    auto sum0 = vaddq_s32(p0, q0);
    auto pWeak = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vmulq_n_s32(p1, 2), vaddq_s32(p0, q1)), 2), 2);
    auto qWeak = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vmulq_n_s32(q1, 2), vaddq_s32(q0, p1)), 2), 2);
    auto pStrong = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vaddq_s32(p2, q1),
        vmulq_n_s32(vaddq_s32(p1, sum0), 2)), 4), 3);
    auto qStrong = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vaddq_s32(q2, p1),
        vmulq_n_s32(vaddq_s32(q1, sum0), 2)), 4), 3);
    auto pOne = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vaddq_s32(p2, p1), sum0), 2), 2);
    auto qOne = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vaddq_s32(q2, q1), sum0), 2), 2);
    auto pTwo = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vaddq_s32(vmulq_n_s32(p3, 2),
        vmulq_n_s32(p2, 3)), vaddq_s32(p1, sum0)), 4), 3);
    auto qTwo = vshrq_n_s32(vaddq_n_s32(vaddq_s32(vaddq_s32(vmulq_n_s32(q3, 2),
        vmulq_n_s32(q2, 3)), vaddq_s32(q1, sum0)), 4), 3);
    auto outP0 = vpselq_s32(vpselq_s32(pStrong, pWeak, ps), p0, enabled);
    auto outQ0 = vpselq_s32(vpselq_s32(qStrong, qWeak, qs), q0, enabled);
    vstrbq_scatter_offset_u32(q - across, offsets, vreinterpretq_u32_s32(outP0));
    vstrbq_scatter_offset_u32(q, offsets, vreinterpretq_u32_s32(outQ0));
    vstrbq_scatter_offset_u32(q - 2U * across, offsets, vreinterpretq_u32_s32(vpselq_s32(pOne, p1, ps)));
    vstrbq_scatter_offset_u32(q + across, offsets, vreinterpretq_u32_s32(vpselq_s32(qOne, q1, qs)));
    vstrbq_scatter_offset_u32(q - 3U * across, offsets, vreinterpretq_u32_s32(vpselq_s32(pTwo, p2, ps)));
    vstrbq_scatter_offset_u32(q + 2U * across, offsets, vreinterpretq_u32_s32(vpselq_s32(qTwo, q2, qs)));
}

} // namespace sub0h264
#endif
#endif
