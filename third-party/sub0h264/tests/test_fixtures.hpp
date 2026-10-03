/** Sub0h264 — Test fixture provider (cross-platform)
 *
 *  Provides H.264 test vector data on both desktop (file I/O) and
 *  ESP32-P4 (embedded binary data in flash). Test files #include this
 *  header and call getFixture() instead of loadFile().
 *
 *  SPDX-License-Identifier: MIT
 */
#ifndef CROG_SUB0H264_TEST_FIXTURES_HPP
#define CROG_SUB0H264_TEST_FIXTURES_HPP

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

#ifdef ESP_PLATFORM

// ── ESP32: Embedded binary data (linked by target_add_binary_data) ──────
// All *.h264 fixtures from tests/fixtures/ are auto-embedded via CMake glob.
// Each fixture needs an extern declaration and a lookup entry below.

#define DECLARE_FIXTURE(name) \
    extern const uint8_t _binary_##name##_h264_start[] asm("_binary_" #name "_h264_start"); \
    extern const uint8_t _binary_##name##_h264_end[]   asm("_binary_" #name "_h264_end");

// 86 fixtures (auto-generated from tests that reference existing files)
DECLARE_FIXTURE(baseline_640x480)
DECLARE_FIXTURE(baseline_640x480_short)
DECLARE_FIXTURE(bench_ball_baseline_640x480)
DECLARE_FIXTURE(bench_ball_high_640x480)
DECLARE_FIXTURE(bench_scroll_baseline_640x480)
DECLARE_FIXTURE(bench_scroll_high_640x480)
DECLARE_FIXTURE(bench_still_baseline_640x480)
DECLARE_FIXTURE(bench_still_high_640x480)
DECLARE_FIXTURE(bouncing_ball_baseline)
DECLARE_FIXTURE(bouncing_ball_ionly_baseline)
DECLARE_FIXTURE(bouncing_ball_ionly_main)
DECLARE_FIXTURE(bouncing_ball_main)
DECLARE_FIXTURE(cabac_16mb_flat)
DECLARE_FIXTURE(cabac_16mb_noisy)
DECLARE_FIXTURE(cabac_1mb_checkerboard)
DECLARE_FIXTURE(cabac_1mb_const0)
DECLARE_FIXTURE(cabac_1mb_const121)
DECLARE_FIXTURE(cabac_1mb_const128)
DECLARE_FIXTURE(cabac_1mb_const200)
DECLARE_FIXTURE(cabac_1mb_const255)
DECLARE_FIXTURE(cabac_1mb_const64)
DECLARE_FIXTURE(cabac_1mb_flat)
DECLARE_FIXTURE(cabac_1mb_grad_diag_ne)
DECLARE_FIXTURE(cabac_1mb_grad_diag_se)
DECLARE_FIXTURE(cabac_1mb_grad_h)
DECLARE_FIXTURE(cabac_1mb_grad_v)
DECLARE_FIXTURE(cabac_1mb_gradient)
DECLARE_FIXTURE(cabac_1mb_noisy)
DECLARE_FIXTURE(cabac_1mb_noisy_qp10)
DECLARE_FIXTURE(cabac_1mb_noisy_qp17)
DECLARE_FIXTURE(cabac_1mb_noisy_qp24)
DECLARE_FIXTURE(cabac_1mb_noisy_qp30)
DECLARE_FIXTURE(cabac_1mb_noisy_qp40)
DECLARE_FIXTURE(cabac_4mb_flat)
DECLARE_FIXTURE(cabac_4mb_noisy)
DECLARE_FIXTURE(cabac_flat_main)
DECLARE_FIXTURE(cabac_idr_only)
DECLARE_FIXTURE(cabac_min_u100)
DECLARE_FIXTURE(cavlc_16mb_noisy)
DECLARE_FIXTURE(cavlc_1mb_checkerboard)
DECLARE_FIXTURE(cavlc_1mb_const0)
DECLARE_FIXTURE(cavlc_1mb_const128)
DECLARE_FIXTURE(cavlc_1mb_const255)
DECLARE_FIXTURE(cavlc_1mb_flat)
DECLARE_FIXTURE(cavlc_1mb_flat_perturb)
DECLARE_FIXTURE(cavlc_1mb_grad_diag_ne)
DECLARE_FIXTURE(cavlc_1mb_grad_diag_nw)
DECLARE_FIXTURE(cavlc_1mb_grad_diag_se)
DECLARE_FIXTURE(cavlc_1mb_grad_diag_sw)
DECLARE_FIXTURE(cavlc_1mb_grad_h)
DECLARE_FIXTURE(cavlc_1mb_grad_h_rev)
DECLARE_FIXTURE(cavlc_1mb_grad_h_shallow)
DECLARE_FIXTURE(cavlc_1mb_grad_h_steep)
DECLARE_FIXTURE(cavlc_1mb_grad_v)
DECLARE_FIXTURE(cavlc_1mb_grad_v_rev)
DECLARE_FIXTURE(cavlc_1mb_gradient)
DECLARE_FIXTURE(cavlc_1mb_hrow)
DECLARE_FIXTURE(cavlc_1mb_isolated_block)
DECLARE_FIXTURE(cavlc_1mb_noisy)
DECLARE_FIXTURE(cavlc_1mb_vcol)
DECLARE_FIXTURE(cavlc_4mb_dc_only)
DECLARE_FIXTURE(cavlc_4mb_grad_diag)
DECLARE_FIXTURE(cavlc_4mb_grad_h)
DECLARE_FIXTURE(cavlc_4mb_grad_v)
DECLARE_FIXTURE(cavlc_4mb_hbands)
DECLARE_FIXTURE(cavlc_4mb_noisy)
DECLARE_FIXTURE(cavlc_4mb_tall_flat)
DECLARE_FIXTURE(cavlc_4mb_tall_grad_v)
DECLARE_FIXTURE(cavlc_4mb_vbands)
DECLARE_FIXTURE(cavlc_4mb_wide_flat)
DECLARE_FIXTURE(cavlc_4mb_wide_grad_h)
DECLARE_FIXTURE(flat_black_baseline_640x480)
DECLARE_FIXTURE(gradient_pan_baseline)
DECLARE_FIXTURE(gradient_pan_high)
DECLARE_FIXTURE(gradient_pan_ionly_baseline)
DECLARE_FIXTURE(high_640x480)
DECLARE_FIXTURE(pan_down_baseline)
DECLARE_FIXTURE(pan_fast_diag_baseline)
DECLARE_FIXTURE(pan_left_baseline)
DECLARE_FIXTURE(pan_slow_baseline)
DECLARE_FIXTURE(pan_up_baseline)
DECLARE_FIXTURE(scrolling_texture_baseline)
DECLARE_FIXTURE(scrolling_texture_high)
DECLARE_FIXTURE(scrolling_texture_ionly_baseline)
DECLARE_FIXTURE(static_scene_baseline)
DECLARE_FIXTURE(tapo_c110_stream2_high)

#undef DECLARE_FIXTURE

/** Get test fixture data by name. Returns pointer + size for embedded data. */
inline std::vector<uint8_t> getFixture(const char* name)
{
    const char* n = name;
    for (const char* p = name; *p; ++p)
        if (*p == '/' || *p == '\\') n = p + 1;

#define MATCH_FIXTURE(fname) \
    if (std::strcmp(n, #fname ".h264") == 0) \
        return {_binary_##fname##_h264_start, _binary_##fname##_h264_end};

    MATCH_FIXTURE(baseline_640x480)
    MATCH_FIXTURE(baseline_640x480_short)
    MATCH_FIXTURE(bench_ball_baseline_640x480)
    MATCH_FIXTURE(bench_ball_high_640x480)
    MATCH_FIXTURE(bench_scroll_baseline_640x480)
    MATCH_FIXTURE(bench_scroll_high_640x480)
    MATCH_FIXTURE(bench_still_baseline_640x480)
    MATCH_FIXTURE(bench_still_high_640x480)
    MATCH_FIXTURE(bouncing_ball_baseline)
    MATCH_FIXTURE(bouncing_ball_ionly_baseline)
    MATCH_FIXTURE(bouncing_ball_ionly_main)
    MATCH_FIXTURE(bouncing_ball_main)
    MATCH_FIXTURE(cabac_16mb_flat)
    MATCH_FIXTURE(cabac_16mb_noisy)
    MATCH_FIXTURE(cabac_1mb_checkerboard)
    MATCH_FIXTURE(cabac_1mb_const0)
    MATCH_FIXTURE(cabac_1mb_const121)
    MATCH_FIXTURE(cabac_1mb_const128)
    MATCH_FIXTURE(cabac_1mb_const200)
    MATCH_FIXTURE(cabac_1mb_const255)
    MATCH_FIXTURE(cabac_1mb_const64)
    MATCH_FIXTURE(cabac_1mb_flat)
    MATCH_FIXTURE(cabac_1mb_grad_diag_ne)
    MATCH_FIXTURE(cabac_1mb_grad_diag_se)
    MATCH_FIXTURE(cabac_1mb_grad_h)
    MATCH_FIXTURE(cabac_1mb_grad_v)
    MATCH_FIXTURE(cabac_1mb_gradient)
    MATCH_FIXTURE(cabac_1mb_noisy)
    MATCH_FIXTURE(cabac_1mb_noisy_qp10)
    MATCH_FIXTURE(cabac_1mb_noisy_qp17)
    MATCH_FIXTURE(cabac_1mb_noisy_qp24)
    MATCH_FIXTURE(cabac_1mb_noisy_qp30)
    MATCH_FIXTURE(cabac_1mb_noisy_qp40)
    MATCH_FIXTURE(cabac_4mb_flat)
    MATCH_FIXTURE(cabac_4mb_noisy)
    MATCH_FIXTURE(cabac_flat_main)
    MATCH_FIXTURE(cabac_idr_only)
    MATCH_FIXTURE(cabac_min_u100)
    MATCH_FIXTURE(cavlc_16mb_noisy)
    MATCH_FIXTURE(cavlc_1mb_checkerboard)
    MATCH_FIXTURE(cavlc_1mb_const0)
    MATCH_FIXTURE(cavlc_1mb_const128)
    MATCH_FIXTURE(cavlc_1mb_const255)
    MATCH_FIXTURE(cavlc_1mb_flat)
    MATCH_FIXTURE(cavlc_1mb_flat_perturb)
    MATCH_FIXTURE(cavlc_1mb_grad_diag_ne)
    MATCH_FIXTURE(cavlc_1mb_grad_diag_nw)
    MATCH_FIXTURE(cavlc_1mb_grad_diag_se)
    MATCH_FIXTURE(cavlc_1mb_grad_diag_sw)
    MATCH_FIXTURE(cavlc_1mb_grad_h)
    MATCH_FIXTURE(cavlc_1mb_grad_h_rev)
    MATCH_FIXTURE(cavlc_1mb_grad_h_shallow)
    MATCH_FIXTURE(cavlc_1mb_grad_h_steep)
    MATCH_FIXTURE(cavlc_1mb_grad_v)
    MATCH_FIXTURE(cavlc_1mb_grad_v_rev)
    MATCH_FIXTURE(cavlc_1mb_gradient)
    MATCH_FIXTURE(cavlc_1mb_hrow)
    MATCH_FIXTURE(cavlc_1mb_isolated_block)
    MATCH_FIXTURE(cavlc_1mb_noisy)
    MATCH_FIXTURE(cavlc_1mb_vcol)
    MATCH_FIXTURE(cavlc_4mb_dc_only)
    MATCH_FIXTURE(cavlc_4mb_grad_diag)
    MATCH_FIXTURE(cavlc_4mb_grad_h)
    MATCH_FIXTURE(cavlc_4mb_grad_v)
    MATCH_FIXTURE(cavlc_4mb_hbands)
    MATCH_FIXTURE(cavlc_4mb_noisy)
    MATCH_FIXTURE(cavlc_4mb_tall_flat)
    MATCH_FIXTURE(cavlc_4mb_tall_grad_v)
    MATCH_FIXTURE(cavlc_4mb_vbands)
    MATCH_FIXTURE(cavlc_4mb_wide_flat)
    MATCH_FIXTURE(cavlc_4mb_wide_grad_h)
    MATCH_FIXTURE(flat_black_baseline_640x480)
    MATCH_FIXTURE(gradient_pan_baseline)
    MATCH_FIXTURE(gradient_pan_high)
    MATCH_FIXTURE(gradient_pan_ionly_baseline)
    MATCH_FIXTURE(high_640x480)
    MATCH_FIXTURE(pan_down_baseline)
    MATCH_FIXTURE(pan_fast_diag_baseline)
    MATCH_FIXTURE(pan_left_baseline)
    MATCH_FIXTURE(pan_slow_baseline)
    MATCH_FIXTURE(pan_up_baseline)
    MATCH_FIXTURE(scrolling_texture_baseline)
    MATCH_FIXTURE(scrolling_texture_high)
    MATCH_FIXTURE(scrolling_texture_ionly_baseline)
    MATCH_FIXTURE(static_scene_baseline)
    MATCH_FIXTURE(tapo_c110_stream2_high)

#undef MATCH_FIXTURE

    return {}; // Unknown fixture
}

#else

// ── Desktop: File I/O ───────────────────────────────────────────────────

#include <fstream>
#include <string>

#ifndef SUB0H264_TEST_FIXTURES_DIR
#error "SUB0H264_TEST_FIXTURES_DIR must be defined (path to tests/fixtures/)"
#endif

/** Get test fixture data by name. Reads from filesystem. */
inline std::vector<uint8_t> getFixture(const char* name)
{
    std::string path = std::string(SUB0H264_TEST_FIXTURES_DIR) + "/" + name;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
        return {};
    auto size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(data.data()), size);
    return data;
}

#endif // ESP_PLATFORM

#endif // CROG_SUB0H264_TEST_FIXTURES_HPP
