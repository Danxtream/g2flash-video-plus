#ifndef ESP_PLATFORM
/** Small CABAC + CAVLC fixture tests — isolate per-MB decode quality.
 *
 *  Tests decoder on 1-16 MB frames with both entropy coding modes
 *  to identify CABAC-specific bugs by comparison with CAVLC.
 *
 *  SPDX-License-Identifier: MIT
 */
#include "doctest.h"
#include <memory>
#include "../components/sub0h264/src/decoder.hpp"
#include "../components/sub0h264/src/annexb.hpp"
#include "../components/sub0h264/src/frame_verify.hpp"
#include "test_fixtures.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace sub0h264;

static double decodeAndPsnr(const char* h264Name, const char* rawName,
                             uint32_t w, uint32_t h)
{
    auto h264 = getFixture(h264Name);
    auto raw = getFixture(rawName);
    if (h264.empty() || raw.empty()) return -1.0;

    auto decoder = std::make_unique<H264Decoder>();
    int frames = decoder->decodeStream(h264.data(), static_cast<uint32_t>(h264.size()));
    if (frames < 1) return -2.0;

    const Frame* frame = decoder->currentFrame();
    if (!frame) return -3.0;

    // Y-plane PSNR
    double mse = 0.0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x)
        {
            int diff = static_cast<int>(frame->y(x, y)) - raw[y * w + x];
            mse += diff * diff;
        }
    mse /= (w * h);
    if (mse < 0.001) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 / mse);
}

/// Decode and return first pixel value, or -1 on failure.
static int decodeFirstPixel(const char* h264Name)
{
    auto h264 = getFixture(h264Name);
    if (h264.empty()) return -1;
    auto decoder = std::make_unique<H264Decoder>();
    if (decoder->decodeStream(h264.data(), static_cast<uint32_t>(h264.size())) < 1) return -1;
    const Frame* f = decoder->currentFrame();
    return f ? static_cast<int>(f->y(0, 0)) : -1;
}

// ── 1 MB (16x16) — no neighbor context, simplest case ──────────────────

TEST_CASE("CABAC small: 1MB flat (16x16)")
{
    double p = decodeAndPsnr("cabac_1mb_flat.h264", "cabac_1mb_flat_raw.yuv", 16, 16);
    MESSAGE("1MB flat Y PSNR: " << p << " dB (ffmpeg: 99.0)");
    CHECK(p > 0.0);
}

TEST_CASE("CABAC small: 1MB gradient (16x16)")
{
    double p = decodeAndPsnr("cabac_1mb_gradient.h264", "cabac_1mb_gradient_raw.yuv", 16, 16);
    MESSAGE("1MB gradient Y PSNR: " << p << " dB (ffmpeg: 51.1)");
    CHECK(p > 0.0);
}

TEST_CASE("CABAC small: 1MB noisy (16x16)")
{
    double p = decodeAndPsnr("cabac_1mb_noisy.h264", "cabac_1mb_noisy_raw.yuv", 16, 16);
    MESSAGE("1MB noisy Y PSNR: " << p << " dB (ffmpeg: 48.2)");
    CHECK(p > 0.0);
}

TEST_CASE("CABAC small: 1MB checkerboard (16x16)")
{
    double p = decodeAndPsnr("cabac_1mb_checkerboard.h264", "cabac_1mb_checkerboard_raw.yuv", 16, 16);
    MESSAGE("1MB checkerboard Y PSNR: " << p << " dB (ffmpeg: 99.0)");
    CHECK(p > 0.0);
}

// ── 4 MBs (32x32) — both neighbor types ────────────────────────────────

TEST_CASE("CABAC small: 4MB flat (32x32)")
{
    double p = decodeAndPsnr("cabac_4mb_flat.h264", "cabac_4mb_flat_raw.yuv", 32, 32);
    MESSAGE("4MB flat Y PSNR: " << p << " dB (ffmpeg: 99.0)");
    CHECK(p > 0.0);
}

TEST_CASE("CABAC small: 4MB noisy (32x32)")
{
    double p = decodeAndPsnr("cabac_4mb_noisy.h264", "cabac_4mb_noisy_raw.yuv", 32, 32);
    MESSAGE("4MB noisy Y PSNR: " << p << " dB (ffmpeg: 48.2)");
    CHECK(p > 0.0);
}

// ── 16 MBs (64x64) — full neighbor grid ────────────────────────────────

TEST_CASE("CABAC small: 16MB flat (64x64)")
{
    double p = decodeAndPsnr("cabac_16mb_flat.h264", "cabac_16mb_flat_raw.yuv", 64, 64);
    MESSAGE("16MB flat Y PSNR: " << p << " dB (ffmpeg: 99.0)");
    CHECK(p > 0.0);
}

TEST_CASE("CABAC small: 16MB noisy (64x64)")
{
    double p = decodeAndPsnr("cabac_16mb_noisy.h264", "cabac_16mb_noisy_raw.yuv", 64, 64);
    MESSAGE("16MB noisy Y PSNR: " << p << " dB (ffmpeg: 48.3)");
    CHECK(p > 0.0);
}

// ── CABAC vs CAVLC comparison: same content, different entropy ──────────
// This isolates CABAC-specific bugs: CAVLC at 52+ dB proves the shared
// pipeline (dequant, IDCT, prediction) works. Any CABAC gap is entropy.

TEST_CASE("CABAC vs CAVLC: 1MB noisy pixel comparison")
{
    int cabac_px = decodeFirstPixel("cabac_1mb_noisy.h264");
    int cavlc_px = decodeFirstPixel("cavlc_1mb_noisy.h264");
    auto raw = getFixture("cabac_1mb_noisy_raw.yuv");
    int raw_px = raw.empty() ? -1 : raw[0];

    MESSAGE("1MB noisy pixel[0]: CABAC=" << cabac_px
            << " CAVLC=" << cavlc_px << " raw=" << raw_px
            << " (ffmpeg CABAC=164, CAVLC=164)");
    // Both should be close to raw; CABAC gap reveals entropy bug
    CHECK(cabac_px >= 0);
    CHECK(cavlc_px >= 0);
}

TEST_CASE("CABAC vs CAVLC: 1MB noisy PSNR comparison")
{
    double cabac = decodeAndPsnr("cabac_1mb_noisy.h264", "cabac_1mb_noisy_raw.yuv", 16, 16);
    double cavlc = decodeAndPsnr("cavlc_1mb_noisy.h264", "cavlc_1mb_noisy_raw.yuv", 16, 16);
    MESSAGE("1MB noisy PSNR: CABAC=" << cabac << " CAVLC=" << cavlc
            << " gap=" << (cavlc - cabac) << " dB");
    CHECK(cabac > 0.0);
    CHECK(cavlc > 0.0);
}

TEST_CASE("CABAC vs CAVLC: 4MB noisy PSNR comparison")
{
    double cabac = decodeAndPsnr("cabac_4mb_noisy.h264", "cabac_4mb_noisy_raw.yuv", 32, 32);
    double cavlc = decodeAndPsnr("cavlc_4mb_noisy.h264", "cavlc_4mb_noisy_raw.yuv", 32, 32);
    MESSAGE("4MB noisy PSNR: CABAC=" << cabac << " CAVLC=" << cavlc
            << " gap=" << (cavlc - cabac) << " dB");
    CHECK(cabac > 0.0);
    CHECK(cavlc > 0.0);
}

// ── Constant-value 1MB: isolates I_16x16 DC decode ─────────────────────
// Encoder uses I_16x16 for constant blocks. These test the Hadamard +
// DC dequant path with zero AC residual.

TEST_CASE("CABAC constant-value 1MB sweep")
{
    struct { const char* name; int expected; } cases[] = {
        {"cabac_1mb_const0.h264",   0},
        {"cabac_1mb_const64.h264",  64},
        {"cabac_1mb_const121.h264", 121},
        {"cabac_1mb_const128.h264", 128},
        {"cabac_1mb_const200.h264", 200},
        {"cabac_1mb_const255.h264", 255},
    };

    for (const auto& c : cases)
    {
        int px = decodeFirstPixel(c.name);
        int err = (px >= 0) ? std::abs(px - c.expected) : -1;
        MESSAGE("  " << c.name << ": pixel=" << px
                << " expected=" << c.expected << " err=" << err);
        CHECK(px >= 0); // At least decodes
    }
}

// ── QP sweep: tests dequant at different scales ─────────────────────────

TEST_CASE("CABAC QP sweep on 1MB noisy")
{
    struct { const char* h264; const char* raw; int qp; double ffmpegPsnr; } cases[] = {
        {"cabac_1mb_noisy_qp10.h264", "cabac_1mb_noisy_qp10_raw.yuv", 10, 54.2},
        {"cabac_1mb_noisy_qp17.h264", "cabac_1mb_noisy_qp17_raw.yuv", 17, 48.6},
        {"cabac_1mb_noisy_qp24.h264", "cabac_1mb_noisy_qp24_raw.yuv", 24, 42.1},
        {"cabac_1mb_noisy_qp30.h264", "cabac_1mb_noisy_qp30_raw.yuv", 30, 35.6},
        {"cabac_1mb_noisy_qp40.h264", "cabac_1mb_noisy_qp40_raw.yuv", 40, 25.2},
    };

    for (const auto& c : cases)
    {
        double p = decodeAndPsnr(c.h264, c.raw, 16, 16);
        MESSAGE("  QP=" << c.qp << ": ours=" << p << " ffmpeg=" << c.ffmpegPsnr
                << " gap=" << (c.ffmpegPsnr - p));
        CHECK(p > 0.0);
    }
}

// ── CAVLC accuracy verification ─────────────────────────────────────────
// Verify CAVLC decode is correct BEFORE using it as CABAC reference.

TEST_CASE("CAVLC small: verify accuracy on micro fixtures")
{
    struct { const char* h264; const char* raw; uint32_t w; uint32_t h; double ffmpegPsnr; } cases[] = {
        {"cavlc_1mb_flat.h264",        "cavlc_1mb_flat_raw.yuv",        16, 16, 99.0},
        {"cavlc_1mb_noisy.h264",       "cavlc_1mb_noisy_raw.yuv",       16, 16, 48.2},
        {"cavlc_1mb_gradient.h264",    "cavlc_1mb_gradient_raw.yuv",    16, 16, 51.1},
        {"cavlc_1mb_checkerboard.h264","cavlc_1mb_checkerboard_raw.yuv",16, 16, 99.0},
        {"cavlc_1mb_const0.h264",      "cavlc_1mb_const0_raw.yuv",      16, 16, 99.0},
        {"cavlc_1mb_const128.h264",    "cavlc_1mb_const128_raw.yuv",    16, 16, 99.0},
        {"cavlc_1mb_const255.h264",    "cavlc_1mb_const255_raw.yuv",    16, 16, 99.0},
        {"cavlc_4mb_noisy.h264",       "cavlc_4mb_noisy_raw.yuv",       32, 32, 48.2},
        {"cavlc_16mb_noisy.h264",      "cavlc_16mb_noisy_raw.yuv",      64, 64, 48.3},
    };

    for (const auto& c : cases)
    {
        double p = decodeAndPsnr(c.h264, c.raw, c.w, c.h);
        double gap = c.ffmpegPsnr - p;
        MESSAGE("  " << c.h264 << ": ours=" << p << " ffmpeg=" << c.ffmpegPsnr
                << " gap=" << gap << " dB");
        // CAVLC should be within ~12 dB of ffmpeg (gradient has known rounding gap)
        if (p > 0 && c.ffmpegPsnr < 90.0)
            CHECK(gap < 15.0);
        else if (p > 0 && c.ffmpegPsnr >= 90.0)
            CHECK(p > 30.0);   // Pixel-perfect content should be very close
    }
}

// ── Gradient axis analysis — isolate CAVLC IDCT rounding issue ──────────
// Tests 8 gradient directions to find which axes trigger accumulated rounding.

TEST_CASE("CAVLC gradient axis: per-direction PSNR")
{
    struct { const char* h264; const char* raw; const char* axis; } cases[] = {
        {"cavlc_1mb_grad_h.h264",        "cavlc_1mb_grad_h_raw.yuv",        "horizontal →"},
        {"cavlc_1mb_grad_v.h264",        "cavlc_1mb_grad_v_raw.yuv",        "vertical ↓"},
        {"cavlc_1mb_grad_diag_se.h264",  "cavlc_1mb_grad_diag_se_raw.yuv",  "diagonal SE ↘"},
        {"cavlc_1mb_grad_diag_ne.h264",  "cavlc_1mb_grad_diag_ne_raw.yuv",  "diagonal NE ↗"},
        {"cavlc_1mb_grad_h_rev.h264",    "cavlc_1mb_grad_h_rev_raw.yuv",    "horizontal ← (rev)"},
        {"cavlc_1mb_grad_v_rev.h264",    "cavlc_1mb_grad_v_rev_raw.yuv",    "vertical ↑ (rev)"},
        {"cavlc_1mb_grad_diag_sw.h264",  "cavlc_1mb_grad_diag_sw_raw.yuv",  "diagonal SW ↙"},
        {"cavlc_1mb_grad_diag_nw.h264",  "cavlc_1mb_grad_diag_nw_raw.yuv",  "diagonal NW ↖"},
        {"cavlc_1mb_grad_h_shallow.h264", "cavlc_1mb_grad_h_shallow_raw.yuv","h shallow (20 range)"},
        {"cavlc_1mb_grad_h_steep.h264",  "cavlc_1mb_grad_h_steep_raw.yuv",  "h steep (0-255)"},
    };

    for (const auto& c : cases)
    {
        double p = decodeAndPsnr(c.h264, c.raw, 16, 16);
        // ffmpeg gets ~50-55 dB on all axes. Our gap reveals the rounding issue.
        MESSAGE("  " << c.axis << ": " << p << " dB");
        CHECK(p > 0.0);
    }
}

TEST_CASE("CAVLC vs CABAC gradient: per-axis comparison")
{
    struct { const char* axis; const char* cavlc; const char* cabac; const char* raw; } cases[] = {
        {"h →",     "cavlc_1mb_grad_h.h264",       "cabac_1mb_grad_h.h264",       "cavlc_1mb_grad_h_raw.yuv"},
        {"v ↓",     "cavlc_1mb_grad_v.h264",       "cabac_1mb_grad_v.h264",       "cavlc_1mb_grad_v_raw.yuv"},
        {"diag SE", "cavlc_1mb_grad_diag_se.h264", "cabac_1mb_grad_diag_se.h264", "cavlc_1mb_grad_diag_se_raw.yuv"},
        {"diag NE", "cavlc_1mb_grad_diag_ne.h264", "cabac_1mb_grad_diag_ne.h264", "cavlc_1mb_grad_diag_ne_raw.yuv"},
    };

    for (const auto& c : cases)
    {
        double cavlc = decodeAndPsnr(c.cavlc, c.raw, 16, 16);
        double cabac = decodeAndPsnr(c.cabac, c.raw, 16, 16);
        MESSAGE("  " << c.axis << ": CAVLC=" << cavlc << " CABAC=" << cabac
                << " gap=" << (cavlc - cabac) << " dB");
    }
}

TEST_CASE("CAVLC gradient 4MB: neighbor influence on rounding")
{
    struct { const char* h264; const char* raw; const char* axis; } cases[] = {
        {"cavlc_4mb_grad_h.h264",    "cavlc_4mb_grad_h_raw.yuv",    "4MB horizontal"},
        {"cavlc_4mb_grad_v.h264",    "cavlc_4mb_grad_v_raw.yuv",    "4MB vertical"},
        {"cavlc_4mb_grad_diag.h264", "cavlc_4mb_grad_diag_raw.yuv", "4MB diagonal"},
    };

    for (const auto& c : cases)
    {
        double p = decodeAndPsnr(c.h264, c.raw, 32, 32);
        MESSAGE("  " << c.axis << ": " << p << " dB");
        CHECK(p > 0.0);
    }
}

// ── Rounding isolation tests ────────────────────────────────────────────
// Targeted fixtures to isolate the IDCT horizontal rounding accumulation.

TEST_CASE("Rounding isolation: horizontal vs vertical content (CAVLC)")
{
    // Horizontal row: forces horizontal AC → tests horizontal IDCT pass
    double hrow = decodeAndPsnr("cavlc_1mb_hrow.h264", "cavlc_1mb_hrow_raw.yuv", 16, 16);
    // Vertical column: forces vertical AC → tests vertical IDCT pass
    double vcol = decodeAndPsnr("cavlc_1mb_vcol.h264", "cavlc_1mb_vcol_raw.yuv", 16, 16);
    // Flat with perturbation: minimal AC, mostly DC
    double perturb = decodeAndPsnr("cavlc_1mb_flat_perturb.h264", "cavlc_1mb_flat_perturb_raw.yuv", 16, 16);

    MESSAGE("  hrow=" << hrow << " vcol=" << vcol << " flat_perturb=" << perturb << " dB");
    // If hrow << vcol, the horizontal IDCT pass has the rounding issue
    CHECK(hrow > 0.0);
    CHECK(vcol > 0.0);
    CHECK(perturb > 0.0);
}

TEST_CASE("Rounding isolation: wide (4x1 MBs) vs tall (1x4 MBs) CAVLC")
{
    // Wide: horizontal MB chain only (no vertical neighbors)
    double wide_grad = decodeAndPsnr("cavlc_4mb_wide_grad_h.h264", "cavlc_4mb_wide_grad_h_raw.yuv", 64, 16);
    double wide_flat = decodeAndPsnr("cavlc_4mb_wide_flat.h264", "cavlc_4mb_wide_flat_raw.yuv", 64, 16);
    // Tall: vertical MB chain only (no horizontal neighbors)
    double tall_grad = decodeAndPsnr("cavlc_4mb_tall_grad_v.h264", "cavlc_4mb_tall_grad_v_raw.yuv", 16, 64);
    double tall_flat = decodeAndPsnr("cavlc_4mb_tall_flat.h264", "cavlc_4mb_tall_flat_raw.yuv", 16, 64);

    MESSAGE("  wide_grad=" << wide_grad << " wide_flat=" << wide_flat);
    MESSAGE("  tall_grad=" << tall_grad << " tall_flat=" << tall_flat);
    // If wide_grad << tall_grad, the horizontal MB chain accumulates more error
    CHECK(wide_grad > 0.0);
    CHECK(tall_grad > 0.0);
}

TEST_CASE("Rounding isolation: forced prediction modes (CAVLC)")
{
    double dc_only = decodeAndPsnr("cavlc_4mb_dc_only.h264", "cavlc_4mb_dc_only_raw.yuv", 32, 32);
    double hbands = decodeAndPsnr("cavlc_4mb_hbands.h264", "cavlc_4mb_hbands_raw.yuv", 32, 32);
    double vbands = decodeAndPsnr("cavlc_4mb_vbands.h264", "cavlc_4mb_vbands_raw.yuv", 32, 32);

    MESSAGE("  dc_only=" << dc_only << " hbands=" << hbands << " vbands=" << vbands << " dB");
    // dc_only should be high (near pixel-perfect)
    // hbands vs vbands shows which prediction direction has more error
    CHECK(dc_only > 0.0);
}

TEST_CASE("Rounding isolation: isolated block vs chained (CAVLC)")
{
    // Single isolated 4x4 block with pattern, rest flat
    double isolated = decodeAndPsnr("cavlc_1mb_isolated_block.h264", "cavlc_1mb_isolated_block_raw.yuv", 16, 16);
    MESSAGE("  isolated_block=" << isolated << " dB");
    // If isolated block is high quality, the IDCT/dequant is correct per-block
    // and the error is only from prediction chain accumulation
    CHECK(isolated > 0.0);
}

TEST_CASE("Rounding isolation: QP sweep on horizontal gradient (CAVLC)")
{
    // If error scales with QP, it's dequant-related.
    // If error is constant, it's IDCT-related.
    struct { int qp; double ffmpegPsnr; } cases[] = {
        {5, 60.0}, {10, 56.0}, {17, 53.0}, {24, 49.0}, {36, 40.0}, {48, 28.0}
    };
    for (const auto& c : cases)
    {
        char h264[80], raw[80];
        std::snprintf(h264, sizeof(h264), "cavlc_1mb_grad_h_qp%d.h264", c.qp);
        std::snprintf(raw, sizeof(raw), "cavlc_1mb_grad_h_qp%d_raw.yuv", c.qp);
        double p = decodeAndPsnr(h264, raw, 16, 16);
        MESSAGE("  QP=" << c.qp << ": ours=" << p << " dB");
        CHECK(p > 0.0);
    }
}

// ── Prediction mode tests ───────────────────────────────────────────────

TEST_CASE("CABAC prediction modes: stripes and edges")
{
    struct { const char* name; uint32_t w; uint32_t h; } cases[] = {
        {"cabac_1mb_hstripes", 16, 16},
        {"cabac_1mb_vstripes", 16, 16},
        {"cabac_1mb_diag",     16, 16},
        {"cabac_1mb_split_v",  16, 16},
        {"cabac_1mb_split_h",  16, 16},
    };

    for (const auto& c : cases)
    {
        char h264[80], raw[80];
        std::snprintf(h264, sizeof(h264), "%s.h264", c.name);
        std::snprintf(raw, sizeof(raw), "%s_raw.yuv", c.name);
        double p = decodeAndPsnr(h264, raw, c.w, c.h);
        MESSAGE("  " << c.name << ": " << p << " dB");
        CHECK(p > 0.0);
    }
}

#endif // ESP_PLATFORM
