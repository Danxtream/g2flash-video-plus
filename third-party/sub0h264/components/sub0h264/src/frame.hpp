/** Sub0h264 — Frame buffer for decoded pictures
 *
 *  I420 planar frame storage with macroblock-level access.
 *  Max resolution 640x480 (40x30 macroblocks).
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_FRAME_HPP
#define CROG_SUB0H264_FRAME_HPP

#include "features.hpp"
#include "allocation_preflight.hpp"
#if !SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
#include <cstdlib>
#endif
#include "sps.hpp"

#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

namespace sub0h264 {

/// Macroblock size in pixels — ITU-T H.264 §6.3.
inline constexpr uint32_t cMbSize = 16U;

/// Chroma block size for 4:2:0 — half of luma in each dimension.
inline constexpr uint32_t cChromaBlockSize = 8U;

/** I420 planar frame buffer.
 *
 *  Stores Y, U, V planes separately with configurable stride.
 *  Stride >= width to allow for alignment padding.
 *  Without chroma reconstruction, U/V pointers are null and sample access
 *  aborts; use hasChroma() before accessing chroma samples.
 */
class Frame
{
public:
    Frame() = default;

    /** Allocate frame planes; omit U/V storage for a luma-only decoder. */
    bool allocate(uint16_t width, uint16_t height, bool skipChroma = false) noexcept
    {
#if !SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        // A disabled build cannot acquire chroma storage through the Frame API.
        skipChroma = true;
#endif
        width_ = width;
        height_ = height;
        yStride_ = width;
        uvStride_ = skipChroma ? 0U : width / 2U;

        uint32_t ySize  = static_cast<uint32_t>(yStride_) * height_;
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        uint32_t uvSize = static_cast<uint32_t>(uvStride_) * (height_ / 2U);
#endif

#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        if (skipChroma)
        {
            // clear() retains capacity and stale pixels when a frame is reused.
            std::vector<uint8_t>().swap(uPlane_);
            std::vector<uint8_t>().swap(vPlane_);
        }
#endif

        const AllocationRequest requests[] = {
            { AllocationTag::FrameY, ySize },
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
            { AllocationTag::FrameU, uvSize },
            { AllocationTag::FrameV, uvSize },
#endif
        };
        allocationFailure_ = {};
        if (!allocationPreflight(requests, skipChroma ? 1U : 3U, &allocationFailure_))
        {
            width_ = height_ = yStride_ = uvStride_ = 0U;
            return false;
        }

        yPlane_.resize(ySize, 0U);
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        if (!skipChroma)
        {
            uPlane_.resize(uvSize, 0U);
            vPlane_.resize(uvSize, 0U);
        }
#endif
        return true;
    }

    /** Fill entire frame with a constant value (useful for testing). */
    void fill(uint8_t yVal, uint8_t uVal, uint8_t vVal) noexcept
    {
        std::fill(yPlane_.begin(), yPlane_.end(), yVal);
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        std::fill(uPlane_.begin(), uPlane_.end(), uVal);
        std::fill(vPlane_.begin(), vPlane_.end(), vVal);
#else
        (void)uVal;
        (void)vVal;
#endif
    }

    // ── Pixel access ────────────────────────────────────────────────────

    uint8_t& y(uint32_t x, uint32_t y) noexcept { return yPlane_[y * yStride_ + x]; }
    uint8_t  y(uint32_t x, uint32_t y) const noexcept { return yPlane_[y * yStride_ + x]; }

    uint8_t& u(uint32_t x, uint32_t y) noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return uPlane_[y * uvStride_ + x];
#else
        (void)x; (void)y; std::abort();
#endif
    }
    uint8_t  u(uint32_t x, uint32_t y) const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return uPlane_[y * uvStride_ + x];
#else
        (void)x; (void)y; std::abort();
#endif
    }

    uint8_t& v(uint32_t x, uint32_t y) noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return vPlane_[y * uvStride_ + x];
#else
        (void)x; (void)y; std::abort();
#endif
    }
    uint8_t  v(uint32_t x, uint32_t y) const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return vPlane_[y * uvStride_ + x];
#else
        (void)x; (void)y; std::abort();
#endif
    }

    // ── Row pointers ────────────────────────────────────────────────────

    uint8_t* yRow(uint32_t row) noexcept { return yPlane_.data() + row * yStride_; }
    const uint8_t* yRow(uint32_t row) const noexcept { return yPlane_.data() + row * yStride_; }

    uint8_t* uRow(uint32_t row) noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return uPlane_.data() + row * uvStride_;
#else
        (void)row; return nullptr;
#endif
    }
    const uint8_t* uRow(uint32_t row) const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return uPlane_.data() + row * uvStride_;
#else
        (void)row; return nullptr;
#endif
    }

    uint8_t* vRow(uint32_t row) noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return vPlane_.data() + row * uvStride_;
#else
        (void)row; return nullptr;
#endif
    }
    const uint8_t* vRow(uint32_t row) const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return vPlane_.data() + row * uvStride_;
#else
        (void)row; return nullptr;
#endif
    }

    // ── Macroblock access ───────────────────────────────────────────────

    /** Get pointer to top-left of luma macroblock at (mbX, mbY). */
    uint8_t* yMb(uint32_t mbX, uint32_t mbY) noexcept
    {
        return yPlane_.data() + (mbY * cMbSize) * yStride_ + (mbX * cMbSize);
    }

    /** Get pointer to top-left of chroma U macroblock at (mbX, mbY). */
    uint8_t* uMb(uint32_t mbX, uint32_t mbY) noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return uPlane_.data() + (mbY * cChromaBlockSize) * uvStride_ + (mbX * cChromaBlockSize);
#else
        (void)mbX; (void)mbY; return nullptr;
#endif
    }

    /** Get pointer to top-left of chroma V macroblock at (mbX, mbY). */
    uint8_t* vMb(uint32_t mbX, uint32_t mbY) noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return vPlane_.data() + (mbY * cChromaBlockSize) * uvStride_ + (mbX * cChromaBlockSize);
#else
        (void)mbX; (void)mbY; return nullptr;
#endif
    }

    // ── Properties ──────────────────────────────────────────────────────

    uint16_t width() const noexcept { return width_; }
    uint16_t height() const noexcept { return height_; }
    uint16_t yStride() const noexcept { return yStride_; }
    uint16_t uvStride() const noexcept { return uvStride_; }

    /** @return Whether chroma sample/row/MB access is valid. */
    bool hasChroma() const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return !uPlane_.empty();
#else
        return false;
#endif
    }

    /** @return Bytes held by frame plane allocations, excluding metadata. */
    uint32_t allocatedBytes() const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return static_cast<uint32_t>(yPlane_.capacity() + uPlane_.capacity() + vPlane_.capacity());
#else
        return static_cast<uint32_t>(yPlane_.capacity());
#endif
    }

    const uint8_t* yData() const noexcept { return yPlane_.data(); }
    const uint8_t* uData() const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return uPlane_.empty() ? nullptr : uPlane_.data();
#else
        return nullptr;
#endif
    }
    const uint8_t* vData() const noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return vPlane_.empty() ? nullptr : vPlane_.data();
#else
        return nullptr;
#endif
    }

    uint8_t* yData() noexcept { return yPlane_.data(); }
    uint8_t* uData() noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return uPlane_.empty() ? nullptr : uPlane_.data();
#else
        return nullptr;
#endif
    }
    uint8_t* vData() noexcept
    {
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
        return vPlane_.empty() ? nullptr : vPlane_.data();
#else
        return nullptr;
#endif
    }

    bool isAllocated() const noexcept { return !yPlane_.empty(); }
    AllocationFailure allocationFailure() const noexcept { return allocationFailure_; }

private:
    AllocationFailure allocationFailure_{};
    uint16_t width_ = 0U;
    uint16_t height_ = 0U;
    uint16_t yStride_ = 0U;
    uint16_t uvStride_ = 0U;

    std::vector<uint8_t> yPlane_;
#if SUB0H264_ENABLE_CHROMA_RECONSTRUCTION
    std::vector<uint8_t> uPlane_;
    std::vector<uint8_t> vPlane_;
#endif
};

} // namespace sub0h264

#endif // CROG_SUB0H264_FRAME_HPP
