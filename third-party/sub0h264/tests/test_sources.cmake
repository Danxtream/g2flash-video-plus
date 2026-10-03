# tests/test_sources.cmake
#
# Single source of truth for shared test source files.
# Included by both desktop tests/CMakeLists.txt and
# ESP32 test_apps/unit_tests/main/CMakeLists.txt.
#
# Usage:
#   include(${CMAKE_CURRENT_LIST_DIR}/../../tests/test_sources.cmake)  # from ESP32
#   include(test_sources.cmake)                                         # from desktop
#
# Provides:
#   SUB0H264_TEST_SOURCES — list of test .cpp files (relative to tests/)
#   SUB0H264_TEST_DIR     — absolute path to the tests/ directory

set(SUB0H264_TEST_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Shared test sources — add new test files HERE (single place).
# test_main.cpp is excluded: desktop provides its own doctest main,
# ESP32 provides app_main() in test_runner.cpp.
set(SUB0H264_TEST_SOURCES
    test_version.cpp
    test_bitstream.cpp
    test_nal.cpp
    test_sps_pps.cpp
    test_slice.cpp
    test_cavlc.cpp
    test_cavlc_levels.cpp
    test_iframe.cpp
    test_pframe.cpp
    test_deblock.cpp
    test_cabac.cpp
    test_decode_pipeline.cpp
    test_frame_verify.cpp
    test_debug_flatblack.cpp
    test_bench.cpp
    test_reconstruct.cpp
    test_mb9_bitcount.cpp
    test_bitstream_trace.cpp
    test_cavlc_regression.cpp
    test_synthetic_quality.cpp
    test_spec_tables.cpp
    test_cabac_engine.cpp
    test_full_trace.cpp
    test_bitstream_edge.cpp
    test_cabac_diag.cpp
    test_cabac_small.cpp
    test_hbands_pixel.cpp
    test_cabac_spec_units.cpp
    test_pframe_cabac.cpp
    test_pframe_cabac_diag.cpp
)
