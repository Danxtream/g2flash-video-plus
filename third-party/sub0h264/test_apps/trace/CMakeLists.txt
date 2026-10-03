# test_apps/trace/CMakeLists.txt — Sub0h264 diagnostic trace tool
#
# Desktop-only CLI for detailed decode tracing.
# Shares all code paths with the library; adds zero overhead when not used.
#
# Build:
#   cmake --preset default && cmake --build --preset default
#   ./build/test_apps/trace/sub0h264_trace <input.h264> [options]

cmake_minimum_required(VERSION 3.21)

add_executable(sub0h264_trace main.cpp)
target_link_libraries(sub0h264_trace PRIVATE Sub0h264::Sub0h264_Traced)
target_include_directories(sub0h264_trace PRIVATE
    "${CMAKE_SOURCE_DIR}/components/sub0h264/src"
)

# Version.h for version string in output
if(TARGET version::version)
    target_link_libraries(sub0h264_trace PRIVATE version::version)
endif()
