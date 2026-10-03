# tests/CMakeLists.txt — Desktop unit tests + benchmarks

# Shared test source list (single source of truth)
include(${CMAKE_CURRENT_SOURCE_DIR}/test_sources.cmake)

# Prepend test_main.cpp (desktop doctest entry point)
set(_all_sources test_main.cpp)
foreach(_src ${SUB0H264_TEST_SOURCES})
    list(APPEND _all_sources "${_src}")
endforeach()

add_executable(Sub0h264_Tests ${_all_sources})

target_link_libraries(Sub0h264_Tests PRIVATE Sub0h264::Sub0h264_Traced)
target_include_directories(Sub0h264_Tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# Path to test fixtures + Version.h from build dir for traceability
target_compile_definitions(Sub0h264_Tests PRIVATE
    SUB0H264_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_link_libraries(Sub0h264_Tests PRIVATE version::version)

add_test(NAME Sub0h264_Tests COMMAND Sub0h264_Tests)

# Bench-only test: runs only [bench] test suite via doctest filter.
# Use: ctest --preset bench
add_test(NAME Sub0h264_Bench_Tests
    COMMAND Sub0h264_Tests --test-suite=bench)
set_tests_properties(Sub0h264_Bench_Tests PROPERTIES LABELS "bench")

# Standalone benchmark binary (not run by ctest — invoke manually)
add_executable(Sub0h264_Bench bench_decode.cpp)
target_link_libraries(Sub0h264_Bench PRIVATE Sub0h264::Sub0h264)
target_include_directories(Sub0h264_Bench PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/vendor"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_compile_definitions(Sub0h264_Bench PRIVATE
    SUB0H264_TEST_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
)
target_link_libraries(Sub0h264_Bench PRIVATE version::version)
