/** Sub0h264 — Annex-B byte stream parser
 *
 *  Splits an Annex-B framed H.264 byte stream into individual NAL units.
 *  Handles start code detection (00 00 01 and 00 00 00 01) and
 *  emulation prevention byte removal (00 00 03 → 00 00).
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_ANNEXB_HPP
#define CROG_SUB0H264_ANNEXB_HPP

#include "nal.hpp"

#include <cstdint>
#include <vector>

namespace sub0h264 {

/// H.264 Annex-B start code byte — ITU-T H.264 §B.1.
inline constexpr uint8_t cStartCodeByte = 0x01U;

/// Emulation prevention byte — ITU-T H.264 §7.4.1.
inline constexpr uint8_t cEmulationPreventionByte = 0x03U;

/** Represents the raw boundaries of a NAL unit within an Annex-B stream. */
struct NalBounds
{
    uint32_t offset;   ///< Byte offset of first NAL data byte (after start code)
    uint32_t size;     ///< Size of NAL data in bytes (before next start code or end)
};

/** Find all NAL unit boundaries in an Annex-B byte stream.
 *
 *  Scans for start code prefixes (00 00 01 or 00 00 00 01) and returns
 *  the boundaries of each NAL unit between them.
 *
 *  ITU-T H.264 §B.1.1: start_code_prefix_one_3bytes = 00 00 01.
 *  §B.1.2: zero_byte (0x00) precedes the 3-byte prefix for NAL types 7 (SPS),
 *  8 (PPS), and the first VCL NAL of each access unit, making them 4-byte
 *  start codes (00 00 00 01). Both forms are detected here. [CHECKED §B.1.1]
 *  NOTE: trailing_zero_8bits bytes that follow a NAL (per §B.1.1) may be
 *  included in the reported NAL size; RBSP parsing is not affected since
 *  the bitstream reader stops at rbsp_stop_one_bit. [CHECKED §B.1.2]
 *
 *  @param data      Pointer to Annex-B byte stream
 *  @param size      Size of the byte stream in bytes
 *  @param[out] nals Vector of NAL boundaries found
 */
inline void findNalUnits(const uint8_t* data, uint32_t size,
                         std::vector<NalBounds>& nals) noexcept
{
    nals.clear();
    if (size < 4U)
        return;

    uint32_t i = 0U;
    // Skip leading zeros
    while (i < size && data[i] == 0x00U)
        ++i;
    if (i >= size || data[i] != cStartCodeByte)
        return;
    ++i; // skip the 0x01

    uint32_t nalStart = i;

    while (i + 2U < size)
    {
        // Look for 00 00 01 or 00 00 00 01
        if (data[i] == 0x00U && data[i + 1U] == 0x00U)
        {
            if (data[i + 2U] == cStartCodeByte)
            {
                // Found 3-byte start code
                uint32_t nalEnd = i;
                // Trim trailing zero that's part of 4-byte start code
                // (the zero before 00 00 01 might be the leading zero of 00 00 00 01)
                nals.push_back({ nalStart, nalEnd - nalStart });
                nalStart = i + 3U;
                i = nalStart;
                continue;
            }
            else if (i + 3U < size && data[i + 2U] == 0x00U && data[i + 3U] == cStartCodeByte)
            {
                // Found 4-byte start code
                uint32_t nalEnd = i;
                nals.push_back({ nalStart, nalEnd - nalStart });
                nalStart = i + 4U;
                i = nalStart;
                continue;
            }
        }
        ++i;
    }

    // Last NAL extends to end of buffer
    if (nalStart < size)
        nals.push_back({ nalStart, size - nalStart });
}

/** Remove emulation prevention bytes from a NAL unit (EBSP → RBSP).
 *
 *  In H.264, the byte sequence 00 00 03 XX (XX in {00,01,02,03}) prevents
 *  00 00 00, 00 00 01, 00 00 02, 00 00 03 from appearing in the payload.
 *  The 03 byte (emulation_prevention_three_byte) is removed on decode.
 *
 *  ITU-T H.264 §7.4.1: emulation_prevention_three_byte shall be equal to 0x03.
 *  The byte following the 03 shall be 00, 01, 02, or 03.
 *  [CHECKED §7.4.1]
 *
 *  @param ebsp     Input NAL data (may contain emulation prevention bytes)
 *  @param size     Size of input data
 *  @param[out] rbsp  Output RBSP data (emulation bytes removed)
 */
inline void removeEmulationPrevention(const uint8_t* ebsp, uint32_t size,
                                       std::vector<uint8_t>& rbsp) noexcept
{
    rbsp.clear();
    rbsp.reserve(size);

    uint32_t zeroCount = 0U;
    for (uint32_t i = 0U; i < size; ++i)
    {
        if (zeroCount == 2U && ebsp[i] == cEmulationPreventionByte)
        {
            // §7.4.1: remove emulation_prevention_three_byte (0x03).
            // Spec requires the following byte to be in {00,01,02,03}; valid
            // streams always satisfy this — we do not validate it here.
            zeroCount = 0U;
            continue;
        }

        if (ebsp[i] == 0x00U)
            ++zeroCount;
        else
            zeroCount = 0U;

        rbsp.push_back(ebsp[i]);
    }
}

/** Parse a single NAL unit from raw Annex-B data.
 *
 *  Extracts the NAL header (forbidden_bit, ref_idc, unit_type) and
 *  removes emulation prevention bytes from the payload.
 *
 *  ITU-T H.264 §7.3.1: nal_unit() syntax:
 *    forbidden_zero_bit  f(1)
 *    nal_ref_idc         u(2)
 *    nal_unit_type       u(5)
 *    rbsp_bytes[]        follows; emulation prevention removed
 *  [CHECKED §7.3.1]
 *
 *  @param data   Pointer to NAL data (first byte is the NAL header byte)
 *  @param size   Size of NAL data in bytes
 *  @param[out] nal  Parsed NAL unit
 *  @return true on success, false if data is too short or forbidden_bit is set
 */
inline bool parseNalUnit(const uint8_t* data, uint32_t size, NalUnit& nal) noexcept
{
    if (size == 0U)
        return false;

    // NAL header byte: forbidden_zero_bit(1) | nal_ref_idc(2) | nal_unit_type(5)
    uint8_t header = data[0];
    bool forbiddenBit = (header >> 7U) & 1U;
    if (forbiddenBit)
        return false;

    nal.refIdc = (header >> 5U) & 0x03U;
    nal.type   = static_cast<NalType>(header & 0x1FU);

    // RBSP is everything after the header byte, with emulation bytes removed
    if (size > 1U)
        removeEmulationPrevention(data + 1U, size - 1U, nal.rbspData);
    else
        nal.rbspData.clear();

    return true;
}

} // namespace sub0h264

#endif // CROG_SUB0H264_ANNEXB_HPP
