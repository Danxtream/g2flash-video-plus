# test_apps/shootout/CMakeLists.txt — A-vs-B decoder performance comparison
#
# Compares Sub0h264 vs libavc (from source) on benchmark fixtures.
# Both decoders are linked as static libraries — no subprocess overhead.
#
# Build (MinGW — required for libavc compatibility):
#   cmake -G "MinGW Makefiles" -B build-shootout -DCMAKE_BUILD_TYPE=Release \
#         -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
#         test_apps/shootout
#   cmake --build build-shootout
#
# Usage:
#   build-shootout/sub0h264_shootout [--fixtures-dir tests/fixtures]

cmake_minimum_required(VERSION 3.21)
project(sub0h264_shootout LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_C_STANDARD 11)

# ── Sub0h264 (header-only, compiled as unity build) ─────────────────────
set(SUB0H264_SRC "${CMAKE_CURRENT_SOURCE_DIR}/../../components/sub0h264/src")
add_library(sub0h264_shootout_lib STATIC
    "${SUB0H264_SRC}/sub0h264_unity.cpp"
)
target_include_directories(sub0h264_shootout_lib PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/sub0h264/include"
    "${SUB0H264_SRC}"
)
# Platform stub (generic for MinGW)
target_sources(sub0h264_shootout_lib PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../components/sub0h264/platform/generic/sub0h264_platform_generic.cpp"
)

# ── libavc decoder (from source at docs/reference/libavc/) ──────────────
set(LIBAVC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../docs/reference/libavc")
if(EXISTS "${LIBAVC_ROOT}/CMakeLists.txt")
    # Build libavc as a subdirectory — it produces libavcdec target
    add_subdirectory("${LIBAVC_ROOT}" "${CMAKE_CURRENT_BINARY_DIR}/libavc" EXCLUDE_FROM_ALL)
    set(HAVE_LIBAVC TRUE)
else()
    message(WARNING "libavc not found at ${LIBAVC_ROOT} — shootout will be sub0h264-only")
    set(HAVE_LIBAVC FALSE)
endif()

# ── Shootout binary ─────────────────────────────────────────────────────
add_executable(sub0h264_shootout main.cpp)
target_link_libraries(sub0h264_shootout PRIVATE sub0h264_shootout_lib)
target_include_directories(sub0h264_shootout PRIVATE "${SUB0H264_SRC}")

if(HAVE_LIBAVC)
    target_link_libraries(sub0h264_shootout PRIVATE libavcdec)
    target_compile_definitions(sub0h264_shootout PRIVATE HAVE_LIBAVC=1)
    target_include_directories(sub0h264_shootout PRIVATE
        "${LIBAVC_ROOT}/decoder"
        "${LIBAVC_ROOT}/common"
    )
endif()
