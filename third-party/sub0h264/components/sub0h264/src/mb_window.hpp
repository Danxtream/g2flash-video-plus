/** Sub0h264 — Per-macroblock context for a sliding window of macroblock rows
 *
 *  Decoding a macroblock reads only its own row and the row above (left, top,
 *  top-left and top-right neighbours), and deblocking one row behind decoding
 *  adds one more row above that. Three rows therefore replace whole-picture
 *  context arrays, about 200 bytes per macroblock. Indices stay picture-global:
 *  the window maps them by subtracting the first element it holds.
 *
 *  SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

namespace sub0h264 {

template <typename T>
class MbRowWindow
{
    static_assert(std::is_trivially_copyable<T>::value, "rows move with memmove");

public:
    /// Elements allocated for the window (rows * elements per row).
    size_t capacity() const noexcept { return data_.capacity(); }

    /// Hold @p rows macroblock rows of @p rowElems elements each.
    void resize(uint32_t rows, size_t rowElems)
    {
        data_.resize(static_cast<size_t>(rows) * rowElems);
        rows_ = rows;
        rowElems_ = rowElems;
    }

    /// Start a picture: the window holds rows 0..rows-1, every element @p fill.
    void reset(const T& fill) noexcept
    {
        firstRow_ = 0U;
        base_ = 0U;
        // The same std::fill instantiation the whole-picture arrays used, so the
        // firmware's frozen hot-function list still finds it.
        std::fill(data_.begin(), data_.end(), fill);
    }

    /// Drop the oldest row and append the next one, every element @p fill.
    void advance(const T& fill) noexcept
    {
        // Raw storage only: the firmware's closed linker cannot take the new
        // out-of-line iterator helpers std::copy or std::fill would add here.
        T* d = data_.data();
        const size_t size = data_.size(), keep = size - rowElems_;
        std::memmove(d, d + rowElems_, keep * sizeof(T));
        for (size_t i = keep; i < size; ++i)
            d[i] = fill;
        ++firstRow_;
        base_ += rowElems_;
    }

    T& operator[](size_t i) noexcept { check(i); return data_[i - base_]; }
    const T& operator[](size_t i) const noexcept { check(i); return data_[i - base_]; }

    /// Window storage; its first element belongs to firstRow().
    const T* data() const noexcept { return data_.data(); }
    /// First macroblock row held.
    uint32_t firstRow() const noexcept { return firstRow_; }
    /// One past the last macroblock row held.
    uint32_t endRow() const noexcept { return firstRow_ + rows_; }

private:
    // Host builds can trap any read outside the window; the firmware build
    // keeps the single subtraction.
    void check(size_t i) const noexcept
    {
#ifdef SUB0H264_CHECK_MB_WINDOW
        if (i < base_ || i - base_ >= data_.size())
            std::abort();
#else
        (void)i;
#endif
    }

    std::vector<T> data_;
    size_t base_ = 0U;
    size_t rowElems_ = 0U;
    uint32_t firstRow_ = 0U;
    uint32_t rows_ = 0U;
};

} // namespace sub0h264
