/** Sub0h264 — Full CAVLC VLC lookup tables from ITU-T H.264 spec
 *
 *  These tables are used to decode coeff_token, total_zeros, and run_before
 *  in the CAVLC entropy decoder. Derived from ITU-T H.264 Tables 9-5 through 9-10.
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_CAVLC_TABLES_HPP
#define CROG_SUB0H264_CAVLC_TABLES_HPP

#include <cstdint>
#include <array>

namespace sub0h264 {

// ── Coeff token tables — ITU-T H.264 Table 9-5 ─────────────────────────
//
// Derived from libavc gau2_ih264d_code_gx (Android Open Source Project,
// ih264d_tables.c) — cross-validated against ITU-T H.264 Table 9-5 and
// verified with scripts/gen_vlc_from_libavc.py (0 duplicates in all 3 tables).

/// VLC code values: [nC_range][trailingOnes][totalCoeff]
/// nC_range: 0 = nC<2, 1 = 2<=nC<4, 2 = 4<=nC<8
/// totalCoeff index: 0..16 (17 entries, including totalCoeff=0)
inline constexpr uint16_t cCoeffTokenCode[3][4][17] = {
    { // Table 9-5(a): 0 <= nC < 2
        {  1,  5,  7,  7,  7,  7, 15, 11,  8, 15, 11, 15, 11, 15, 11,  7,  4, },
        {  0,  1,  4,  6,  6,  6,  6, 14, 10, 14, 10, 14, 10,  1, 14, 10,  6, },
        {  0,  0,  1,  5,  5,  5,  5,  5, 13,  9, 13,  9, 13,  9, 13,  9,  5, },
        {  0,  0,  0,  3,  3,  4,  4,  4,  4,  4, 12, 12,  8, 12,  8, 12,  8, },
    },
    { // Table 9-5(b): 2 <= nC < 4
        {  3, 11,  7,  7,  7,  4,  7, 15, 11, 15, 11,  8, 15, 11,  7,  9,  7, },
        {  0,  2,  7, 10,  6,  6,  6,  6, 14, 10, 14, 10, 14, 10, 11,  8,  6, },
        {  0,  0,  3,  9,  5,  5,  5,  5, 13,  9, 13,  9, 13,  9,  6, 10,  5, },
        {  0,  0,  0,  5,  4,  6,  8,  4,  4,  4, 12,  8, 12, 12,  8,  1,  4, },
    },
    { // Table 9-5(c): 4 <= nC < 8
        { 15, 15, 11,  8, 15, 11,  9,  8, 15, 11, 15, 11,  8, 13,  9,  5,  1, },
        {  0, 14, 15, 12, 10,  8, 14, 10, 14, 14, 10, 14, 10,  7, 12,  8,  4, },
        {  0,  0, 13, 14, 11,  9, 13,  9, 13, 10, 13,  9, 13,  9, 11,  7,  3, },
        {  0,  0,  0, 12, 11, 10,  9,  8, 13, 12, 12, 12,  8, 12, 10,  6,  2, },
    },
};

/// VLC code lengths in bits: [nC_range][trailingOnes][totalCoeff]
inline constexpr uint8_t cCoeffTokenSize[3][4][17] = {
    {
        {  1,  6,  8,  9, 10, 11, 13, 13, 13, 14, 14, 15, 15, 16, 16, 16, 16, },
        {  0,  2,  6,  8,  9, 10, 11, 13, 13, 14, 14, 15, 15, 15, 16, 16, 16, },
        {  0,  0,  3,  7,  8,  9, 10, 11, 13, 13, 14, 14, 15, 15, 16, 16, 16, },
        {  0,  0,  0,  5,  6,  7,  8,  9, 10, 11, 13, 14, 14, 15, 15, 16, 16, },
    },
    {
        {  2,  6,  6,  7,  8,  8,  9, 11, 11, 12, 12, 12, 13, 13, 13, 14, 14, },
        {  0,  2,  5,  6,  6,  7,  8,  9, 11, 11, 12, 12, 13, 13, 14, 14, 14, },
        {  0,  0,  3,  6,  6,  7,  8,  9, 11, 11, 12, 12, 13, 13, 13, 14, 14, },
        {  0,  0,  0,  4,  4,  5,  6,  6,  7,  9, 11, 11, 12, 13, 13, 13, 14, },
    },
    {
        {  4,  6,  6,  6,  7,  7,  7,  7,  8,  8,  9,  9,  9, 10, 10, 10, 10, },
        {  0,  4,  5,  5,  5,  5,  6,  6,  7,  8,  8,  9,  9,  9, 10, 10, 10, },
        {  0,  0,  4,  5,  5,  5,  6,  6,  7,  7,  8,  8,  9,  9, 10, 10, 10, },
        {  0,  0,  0,  4,  4,  4,  4,  4,  5,  6,  7,  8,  8,  9, 10, 10, 10, },
    },
};

/// Chroma DC coeff_token code values: [trailingOnes][totalCoeff]
/// For nC == -1 (chroma DC 2x2 block, max 4 coefficients)
/// totalCoeff index: 0..4 (5 entries, including totalCoeff=0)
inline constexpr uint8_t cCoeffTokenCodeChroma[4][5] = {
    {  1,  7,  4,  3,  2, },
    {  0,  1,  6,  3,  3, },
    {  0,  0,  1,  2,  2, },
    {  0,  0,  0,  5,  0, },
};

/// Chroma DC coeff_token code sizes: [trailingOnes][totalCoeff]
inline constexpr uint8_t cCoeffTokenSizeChroma[4][5] = {
    {  2,  6,  6,  6,  6, },
    {  0,  1,  6,  7,  8, },
    {  0,  0,  3,  7,  8, },
    {  0,  0,  0,  6,  7, },
};

// ── Total zeros tables — ITU-T H.264 Tables 9-7, 9-8 ───────────────────

/// Index offsets into total_zeros tables, by totalCoeff (1-based).
inline constexpr uint8_t cTotalZerosIndex[15] = {
    0, 16, 31, 45, 58, 70, 81, 91, 100, 108, 115, 121, 126, 130, 133,
};

/// Total zeros VLC code sizes (flattened, indexed via cTotalZerosIndex).
/// ITU-T H.264 Table 9-7. Note: totalCoeff=3 has a characteristic
/// 3-bit "dip" at tz=6-7 (lengths: 4,3,3,3,4,4,3,3,4,5,5,6,5,6).
inline constexpr uint8_t cTotalZerosSize[135] = {
     1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9,
     3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6,
     4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6,
     5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5,
     4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5,
     6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6,
     6, 5, 3, 3, 3, 2, 3, 4, 3, 6,
     6, 4, 5, 3, 2, 2, 3, 3, 6,
     6, 6, 4, 2, 2, 3, 2, 5,
     5, 5, 3, 2, 2, 2, 4,
     4, 4, 3, 3, 1, 3,
     4, 4, 2, 1, 3,
     3, 3, 1, 2,
     2, 2, 1,
     1, 1,
};

/// Total zeros VLC code values (flattened, indexed via cTotalZerosIndex).
/// ITU-T H.264 Table 9-7.
inline constexpr uint8_t cTotalZerosCode[135] = {
     1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1,
     7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0,
     5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0,
     3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0,
     5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0,
     1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0,
     1, 1, 5, 4, 3, 3, 2, 1, 1, 0,
     1, 1, 1, 3, 3, 2, 2, 1, 0,
     1, 0, 1, 3, 2, 1, 1, 1,
     1, 0, 1, 3, 2, 1, 1,
     0, 1, 1, 2, 1, 3,
     0, 1, 1, 1, 1,
     0, 1, 1, 1,
     0, 1, 1,
     0, 1,
};

/// Chroma DC total_zeros sizes (totalCoeff 1-3).
inline constexpr uint8_t cTotalZerosSizeChroma[9] = {
     1, 2, 3, 3,
     1, 2, 2,
     1, 1,
};

/// Chroma DC total_zeros codes.
inline constexpr uint8_t cTotalZerosCodeChroma[9] = {
     1, 1, 1, 0,
     1, 1, 0,
     1, 0,
};

// ── Run before tables — ITU-T H.264 Table 9-10 ─────────────────────────

/// Index offsets into run_before tables, by zerosLeft (1-based).
inline constexpr uint8_t cRunBeforeIndex[7] = {
    0, 2, 5, 9, 14, 20, 27,
};

/// Run before VLC code sizes (flattened).
inline constexpr uint8_t cRunBeforeSize[42] = {
      1,  1,
      1,  2,  2,
      2,  2,  2,  2,
      2,  2,  2,  3,  3,
      2,  2,  3,  3,  3,  3,
      2,  3,  3,  3,  3,  3,  3,
      3,  3,  3,  3,  3,  3,  3,  4,  5,  6,  7,  8,  9, 10, 11,
};

/// Run before VLC code values (flattened).
inline constexpr uint8_t cRunBeforeCode[42] = {
      1,  0,
      1,  1,  0,
      3,  2,  1,  0,
      3,  2,  1,  1,  0,
      3,  2,  3,  2,  1,  0,
      3,  0,  1,  3,  2,  5,  4,
      7,  6,  5,  4,  3,  2,  1,  1,  1,  1,  1,  1,  1,  1,  1,
};

} // namespace sub0h264

#endif // CROG_SUB0H264_CAVLC_TABLES_HPP
