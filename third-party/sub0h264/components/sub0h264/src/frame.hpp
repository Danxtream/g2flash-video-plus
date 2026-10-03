/** Sub0h264 — Frame buffer for decoded pictures
 *
 *  I420 planar frame storage with macroblock-level access.
 *  Max resolution 640x480 (40x30 macroblocks).
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_FRAME_HPP
#define CROG_SUB0H264_FRAME_HPP

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
 */
class Frame
{
public:
    Frame() = default;

    /** Allocate frame for given dimensions. */
    bool allocate(uint16_t width, uint16_t height) noexcept
    {
        width_ = width;
        height_ = height;
        yStride_ = width;
        uvStride_ = width / 2U;

        uint32_t ySize  = static_cast<uint32_t>(yStride_) * height_;
        uint32_t uvSize = static_cast<uint32_t>(uvStride_) * (height_ / 2U);

        yPlane_.resize(ySize, 0U);
        uPlane_.resize(uvSize, 0U);
        vPlane_.resize(uvSize, 0U);
        return true;
    }

    /** Fill entire frame with a constant value (useful for testing). */
    void fill(uint8_t yVal, uint8_t uVal, uint8_t vVal) noexcept
    {
        std::fill(yPlane_.begin(), yPlane_.end(), yVal);
        std::fill(uPlane_.begin(), uPlane_.end(), uVal);
        std::fill(vPlane_.begin(), vPlane_.end(), vVal);
    }

    // ── Pixel access ────────────────────────────────────────────────────

    uint8_t& y(uint32_t x, uint32_t y) noexcept { return yPlane_[y * yStride_ + x]; }
    uint8_t  y(uint32_t x, uint32_t y) const noexcept { return yPlane_[y * yStride_ + x]; }

    uint8_t& u(uint32_t x, uint32_t y) noexcept { return uPlane_[y * uvStride_ + x]; }
    uint8_t  u(uint32_t x, uint32_t y) const noexcept { return uPlane_[y * uvStride_ + x]; }

    uint8_t& v(uint32_t x, uint32_t y) noexcept { return vPlane_[y * uvStride_ + x]; }
    uint8_t  v(uint32_t x, uint32_t y) const noexcept { return vPlane_[y * uvStride_ + x]; }

    // ── Row pointers ────────────────────────────────────────────────────

    uint8_t* yRow(uint32_t row) noexcept { return yPlane_.data() + row * yStride_; }
    const uint8_t* yRow(uint32_t row) const noexcept { return yPlane_.data() + row * yStride_; }

    uint8_t* uRow(uint32_t row) noexcept { return uPlane_.data() + row * uvStride_; }
    const uint8_t* uRow(uint32_t row) const noexcept { return uPlane_.data() + row * uvStride_; }

    uint8_t* vRow(uint32_t row) noexcept { return vPlane_.data() + row * uvStride_; }
    const uint8_t* vRow(uint32_t row) const noexcept { return vPlane_.data() + row * uvStride_; }

    // ── Macroblock access ───────────────────────────────────────────────

    /** Get pointer to top-left of luma macroblock at (mbX, mbY). */
    uint8_t* yMb(uint32_t mbX, uint32_t mbY) noexcept
    {
        return yPlane_.data() + (mbY * cMbSize) * yStride_ + (mbX * cMbSize);
    }

    /** Get pointer to top-left of chroma U macroblock at (mbX, mbY). */
    uint8_t* uMb(uint32_t mbX, uint32_t mbY) noexcept
    {
        return uPlane_.data() + (mbY * cChromaBlockSize) * uvStride_ + (mbX * cChromaBlockSize);
    }

    /** Get pointer to top-left of chroma V macroblock at (mbX, mbY). */
    uint8_t* vMb(uint32_t mbX, uint32_t mbY) noexcept
    {
        return vPlane_.data() + (mbY * cChromaBlockSize) * uvStride_ + (mbX * cChromaBlockSize);
    }

    // ── Properties ──────────────────────────────────────────────────────

    uint16_t width() const noexcept { return width_; }
    uint16_t height() const noexcept { return height_; }
    uint16_t yStride() const noexcept { return yStride_; }
    uint16_t uvStride() const noexcept { return uvStride_; }

    const uint8_t* yData() const noexcept { return yPlane_.data(); }
    const uint8_t* uData() const noexcept { return uPlane_.data(); }
    const uint8_t* vData() const noexcept { return vPlane_.data(); }

    uint8_t* yData() noexcept { return yPlane_.data(); }
    uint8_t* uData() noexcept { return uPlane_.data(); }
    uint8_t* vData() noexcept { return vPlane_.data(); }

    bool isAllocated() const noexcept { return !yPlane_.empty(); }

private:
    uint16_t width_ = 0U;
    uint16_t height_ = 0U;
    uint16_t yStride_ = 0U;
    uint16_t uvStride_ = 0U;

    std::vector<uint8_t> yPlane_;
    std::vector<uint8_t> uPlane_;
    std::vector<uint8_t> vPlane_;
};

} // namespace sub0h264

#endif // CROG_SUB0H264_FRAME_HPP
