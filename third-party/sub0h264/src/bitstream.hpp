/** Sub0h264 — Bit-level reader + Exp-Golomb decoder
 *
 *  Reads from a byte buffer at bit granularity. All H.264 bitstream
 *  parsing (headers, entropy, etc.) builds on this class.
 *
 *  Spec-annotated review (2026-04-09):
 *    §7.2 readUev: code_num = 2^N - 1 + suffix, CLZ for leading zeros [CHECKED §7.2]
 *    §7.2 readSev: 0→0, odd→+ceil(k/2), even→-ceil(k/2) [CHECKED §7.2]
 *    §9.1 readTev: range==1 → !readBit(), else ue(v) [CHECKED §9.1]
 *    MSB-first bit order, 64-bit read-ahead cache for PSRAM efficiency [CHECKED]
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_BITSTREAM_HPP
#define CROG_SUB0H264_BITSTREAM_HPP

#include <cassert>
#include <cstdint>
#include <cstring>

#ifdef _MSC_VER
#include <intrin.h>
#endif

namespace sub0h264 {

/// Sentinel value returned by readUev() on overflow (>31 leading zeros).
inline constexpr uint32_t cExpGolombOverflow = 0xFFFFFFFFU;

/** Bit-level reader over a byte buffer.
 *
 *  Data is read MSB-first (network/big-endian bit order).
 *  The buffer does NOT need to be 32-bit aligned — we handle unaligned access.
 */
class BitReader
{
public:
    BitReader() noexcept = default;

    /** Construct from raw byte buffer.
     *  @param data   Pointer to bitstream data
     *  @param sizeBytes  Size in bytes
     */
    BitReader(const uint8_t* data, uint32_t sizeBytes) noexcept
        : data_(data)
        , sizeBytes_(sizeBytes)
        , bitOffset_(0U)
        , cache_(0U)
        , cacheByteOff_(0xFFFFFFFFU) // force first refill
    {}

    /** Read N bits (1-32) and advance the offset. */
    uint32_t readBits(uint32_t n) noexcept
    {
        assert(n <= 32U && "readBits: n must be 0-32");
        // No bounds assert: CABAC divergence can cause reads past slice end.
        // The reader returns 0 for out-of-bounds bits (via peekBits padding).
        uint32_t val = peekBits(n);
        bitOffset_ += n;
        return val;
    }

    /** Peek N bits (1-32) without advancing the offset.
     *
     *  Uses a cached 64-bit window to avoid re-reading from PSRAM on
     *  every call. The window is refilled only when the current read
     *  falls outside the cached range. For sequential reads (the common
     *  case in entropy decode), this reduces PSRAM accesses from ~5 per
     *  call to ~1 per 8 bytes.
     */
    uint32_t peekBits(uint32_t n) const noexcept
    {
        if (n == 0U)
            return 0U;

        uint32_t byteOff = bitOffset_ >> 3U;
        uint32_t bitPos  = bitOffset_ & 7U;

        // Refill cache if the current byte is outside the cached window.
        // The cache holds 8 bytes starting at cacheByteOff_.
        // A read of n bits at bitPos can span at most 5 bytes: [byteOff..byteOff+4].
        if (byteOff < cacheByteOff_ || byteOff + 4U > cacheByteOff_ + 7U)
            refillCache(byteOff);

        // Extract from cached 64-bit window.
        // cache_ bits are arranged MSB-first from cacheByteOff_.
        uint32_t bitInCache = (byteOff - cacheByteOff_) * 8U + bitPos;
        uint32_t shift = 64U - bitInCache - n;
        uint64_t mask  = (1ULL << n) - 1ULL;
        return static_cast<uint32_t>((cache_ >> shift) & mask);
    }

    /** Skip N bits. */
    void skipBits(uint32_t n) noexcept
    {
        bitOffset_ += n;
    }

    /** Read a single bit — optimized fast path.
     *  Uses the cached window when available, falls back to direct byte read.
     */
    uint32_t readBit() noexcept
    {
        uint32_t byteOff = bitOffset_ >> 3U;
        uint32_t bitPos  = bitOffset_ & 7U;
        ++bitOffset_;

        // Fast path: byte is in cache
        if (byteOff >= cacheByteOff_ && byteOff < cacheByteOff_ + 8U)
        {
            uint32_t bitInCache = (byteOff - cacheByteOff_) * 8U + bitPos;
            return static_cast<uint32_t>((cache_ >> (63U - bitInCache)) & 1U);
        }

        // Slow path: direct byte read
        return (byteOff < sizeBytes_)
            ? (data_[byteOff] >> (7U - bitPos)) & 1U
            : 0U;
    }



    /** Read unsigned Exp-Golomb coded value: ue(v).
     *
     *  Encoding: leadingZeros '1' suffix
     *  Value = (1 << leadingZeros) + suffix - 1
     */
    uint32_t readUev() noexcept
    {
        // Peek up to 32 bits and count leading zeros with CLZ.
        // peekBits returns RIGHT-aligned bits, so we left-shift to align
        // the MSB of the peeked window with bit 31 for correct CLZ.
        uint32_t remaining = sizeBytes_ * 8U - bitOffset_;
        uint32_t peekN = (remaining < 32U) ? remaining : 32U;
        if (peekN == 0U)
            return cExpGolombOverflow;

        uint32_t bits = peekBits(peekN);
        if (bits == 0U)
            return cExpGolombOverflow;

        // Left-align: shift peeked bits to MSB so CLZ gives correct count
        bits <<= (32U - peekN);

#if defined(__GNUC__) || defined(__clang__)
        uint32_t leadingZeros = static_cast<uint32_t>(__builtin_clz(bits));
#elif defined(_MSC_VER)
        unsigned long idx;
        _BitScanReverse(&idx, bits);
        uint32_t leadingZeros = 31U - idx;
#else
        uint32_t leadingZeros = 0U;
        uint32_t tmp = bits;
        while ((tmp & 0x80000000U) == 0U) { ++leadingZeros; tmp <<= 1U; }
#endif

        if (leadingZeros > 31U)
            return cExpGolombOverflow;

        // Skip leading zeros + the '1' bit
        bitOffset_ += leadingZeros + 1U;

        if (leadingZeros == 0U)
            return 0U;

        uint32_t suffix = readBits(leadingZeros);
        return (1U << leadingZeros) - 1U + suffix;
    }

    /** Read signed Exp-Golomb coded value: se(v).
     *
     *  Maps: 0→0, 1→1, 2→-1, 3→2, 4→-2, ...
     */
    int32_t readSev() noexcept
    {
        uint32_t uval = readUev();
        if (uval == 0U)
            return 0;
        int32_t val = static_cast<int32_t>((uval + 1U) >> 1U);
        return (uval & 1U) ? val : -val;
    }

    /** Read truncated Exp-Golomb: te(v).
     *  When range == 1: read 1 bit, return !bit.
     *  Otherwise: same as ue(v).
     */
    uint32_t readTev(uint32_t range) noexcept
    {
        if (range <= 1U)
            return 1U - readBit();
        return readUev();
    }

    /** Check if the offset is byte-aligned. */
    bool isAligned() const noexcept
    {
        return (bitOffset_ & 7U) == 0U;
    }

    /** Advance to the next byte boundary. */
    void alignToByte() noexcept
    {
        bitOffset_ = (bitOffset_ + 7U) & ~7U;
    }

    /** @return Current bit offset from start. */
    uint32_t bitOffset() const noexcept { return bitOffset_; }

    /** @return Total bits available. */
    uint32_t totalBits() const noexcept { return sizeBytes_ * 8U; }

    /** @return True if there are at least N bits remaining. */
    bool hasBits(uint32_t n) const noexcept
    {
        return (bitOffset_ + n) <= (sizeBytes_ * 8U);
    }

    /** @return True if all bits have been consumed. */
    bool isExhausted() const noexcept
    {
        return bitOffset_ >= (sizeBytes_ * 8U);
    }

    /** Seek to an absolute bit position.
     *  Invalidates the read-ahead cache so the next read refills from the
     *  new position. Used by CabacEngine::restore() for state snapshots.
     *  @param pos  Bit offset from start of buffer
     */
    void seekToBit(uint32_t pos) noexcept
    {
        bitOffset_ = pos;
        cacheByteOff_ = 0xFFFFFFFFU; // invalidate cache — force refill
    }

    /** @return Pointer to the underlying data. */
    const uint8_t* data() const noexcept { return data_; }

    /** @return Size in bytes. */
    uint32_t sizeBytes() const noexcept { return sizeBytes_; }

private:
    const uint8_t* data_ = nullptr;
    uint32_t sizeBytes_  = 0U;
    uint32_t bitOffset_  = 0U;

    // 64-bit read-ahead cache: avoids re-reading 5 bytes from PSRAM per peekBits().
    // Mutable because peekBits() is logically const but updates the cache.
    mutable uint64_t cache_ = 0U;
    mutable uint32_t cacheByteOff_ = 0xFFFFFFFFU;

    /** Refill the 64-bit cache from the byte at byteOff. */
    void refillCache(uint32_t byteOff) const noexcept
    {
        cacheByteOff_ = byteOff;
        cache_ = 0U;
        // Read up to 8 bytes into MSB-first 64-bit window.
        uint32_t avail = (byteOff < sizeBytes_) ? sizeBytes_ - byteOff : 0U;
        uint32_t toRead = (avail < 8U) ? avail : 8U;
        for (uint32_t i = 0U; i < toRead; ++i)
            cache_ = (cache_ << 8U) | data_[byteOff + i];
        uint32_t padBits = (8U - toRead) * 8U;
        if (padBits > 0U && padBits < 64U)
            cache_ <<= padBits; // pad remaining MSB bits with 0
    }
};

} // namespace sub0h264

#endif // CROG_SUB0H264_BITSTREAM_HPP
