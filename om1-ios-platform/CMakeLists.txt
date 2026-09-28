# OM1: PCSX2 core + common as static libraries for iOS (device/simulator).
# Offline-VU1 archive build only (no adapter, no app): produces
# common/libcommon.a + pcsx2/libPCSX2.a, which the fork's PS2X_OM1_LIB block
# links into the iOS app. Metal-only (no Vulkan/OpenGL/shaderc), no
# Qt/tests/GSRunner. Structure + dep recipe follow GI1's ios-platform
# (vendored sources, no find_package for core deps); dep SOURCES are reused
# from GI1's ios-3rdparty via -DOM1_IOS_DEPS (no OM1 changes needed there).
#
# Configure (Ninja):
#   cmake -S <this> -B <build> -G Ninja \
#     -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
#     -DOM1_IOS_DEPS=$HOME/dev/ssx3-work/GI1/ios-3rdparty
# Build: cmake --build <build> --target PCSX2
# (common + the pulled 3rdparty archives come along as dependencies.)
# OM1-private (lives in the OM1 scratch ARMSX2 tree; never in the fork).
cmake_minimum_required(VERSION 3.22)
project(OM1iOS C CXX OBJC OBJCXX)

get_filename_component(OM1_PCSX2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(OM1_IOS_DEPS "" CACHE PATH "Vendored iOS dependency sources (see header)")
if(NOT OM1_IOS_DEPS)
  message(FATAL_ERROR "OM1: -DOM1_IOS_DEPS=<dir> is required")
endif()

# iOS platform: ARM64, Metal-only, no desktop UI/test/runner targets.
# IOS drives the core's platform guards; ARMSX2_IOS adds the in-tree iOS
# stubs (MacOSStubs no-ops itself on embedded; AndroidPcapStubs answers
# DEV9; AppleEmbeddedStubs below answers CDVD/optical/sound/downloader).
set(IOS TRUE)
set(ARMSX2_IOS TRUE)
set(CMAKE_SYSTEM_PROCESSOR "arm64")
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0")
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_GSRUNNER OFF CACHE BOOL "" FORCE)
# The offline core compiles with PCSX2_RECOMPILER_TESTS and references the
# gif/divtrace/vu1-trace hook globals; the archives must define them (same
# as the desktop bench/dylib archives).
set(ENABLE_RECOMPILER_TEST_HOOKS ON CACHE BOOL "" FORCE)
set(USE_VULKAN OFF CACHE BOOL "" FORCE)
set(USE_LINKED_FFMPEG OFF CACHE BOOL "" FORCE)
set(BAKE_SHADERS_IN_CPP OFF CACHE BOOL "" FORCE)

# Our iOS SearchForStuff wins; root modules (Pcsx2Utils, BuildParameters)
# resolve behind it.
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" "${OM1_PCSX2_ROOT}/cmake")
set(TOP_CMAKE_WAS_SOURCED TRUE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include(SearchForStuff)
get_git_version_info()
write_svnrev_h()

add_subdirectory("${OM1_PCSX2_ROOT}/common" "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${OM1_PCSX2_ROOT}/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")

# In-tree iOS stubs: embedded CDVD/optical/sound/downloader answers.
# (AndroidPcapStubs rides the core's ARMSX2_IOS block; OM1iOSStubs covers
# the one IOCtlSrc method AppleEmbeddedStubs omits.)
target_sources(PCSX2 PRIVATE
  "${OM1_PCSX2_ROOT}/pcsx2/Apple/AppleEmbeddedStubs.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/OM1iOSStubs.cpp")
