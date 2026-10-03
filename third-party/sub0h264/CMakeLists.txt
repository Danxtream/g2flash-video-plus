cmake_minimum_required(VERSION 3.21)

# Detect if Sub0h264 is being included inside another project
if(DEFINED PROJECT_NAME)
    set(IS_SUBPROJECT ON)
endif()

project(Sub0h264 VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

# ── CPM package manager + Version.cmake for git-based versioning ─────────
include(cmake/CPM.cmake)
CPMAddPackage("gh:BareCpper/Version.cmake@0.2")
# Provides version::version target. Link to get Version.h with:
#   VERSION_SHA, VERSION_SEMANTIC, VERSION_FULL, VERSION_MAJOR/MINOR/PATCH

option(SUB0H264_BUILD_TESTING "Build unit-tests" ON)

# Platform selection for SIMD dispatch
# On desktop: auto-detected (generic or x86)
# On ESP-IDF: set by IDF toolchain (riscv_pie)
if(NOT DEFINED SUB0H264_PLATFORM)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86|AMD64|i[3-6]86")
        set(SUB0H264_PLATFORM "x86")
    else()
        set(SUB0H264_PLATFORM "generic")
    endif()
endif()

# The library is built from components/sub0h264/
add_subdirectory(components/sub0h264)

# Alias targets for consumers
add_library(Sub0h264::Sub0h264 ALIAS sub0h264)
add_library(Sub0h264::Sub0h264_Traced ALIAS sub0h264_traced)

if(SUB0H264_BUILD_TESTING AND NOT IS_SUBPROJECT)
    enable_testing()
    add_subdirectory(tests)
    add_subdirectory(test_apps/trace)
    add_subdirectory(test_apps/shootout)
endif()

# Install project if not included by add_subdirectory() from another project
if(NOT IS_SUBPROJECT)

    include(CMakePackageConfigHelpers)

    set(SUB0H264_CMAKE_CONFIG_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Sub0h264")

    configure_package_config_file(
        "${CMAKE_CURRENT_LIST_DIR}/cmake/Sub0h264Config.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Sub0h264Config.cmake"
        INSTALL_DESTINATION
            "${SUB0H264_CMAKE_CONFIG_DESTINATION}"
    )

    write_basic_package_version_file(
        "${PROJECT_BINARY_DIR}/Sub0h264ConfigVersion.cmake"
        COMPATIBILITY
            SameMajorVersion
    )

    install(
        TARGETS
            sub0h264
        EXPORT
            Sub0h264Targets
        DESTINATION
            ${CMAKE_INSTALL_LIBDIR}
    )

    install(
        EXPORT
            Sub0h264Targets
        NAMESPACE
            Sub0h264::
        DESTINATION
            ${SUB0H264_CMAKE_CONFIG_DESTINATION}
    )

    install(
        FILES
            "${PROJECT_BINARY_DIR}/Sub0h264ConfigVersion.cmake"
            "${PROJECT_BINARY_DIR}/Sub0h264Config.cmake"
        DESTINATION
            "${SUB0H264_CMAKE_CONFIG_DESTINATION}"
    )

    # Install include sources
    install(
        DIRECTORY
            "components/sub0h264/include/"
        DESTINATION
            "${CMAKE_INSTALL_INCLUDEDIR}"
    )

endif()
