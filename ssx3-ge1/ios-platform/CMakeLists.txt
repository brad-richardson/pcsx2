# GI1: PCSX2 core + GE1 adapter as static libraries for iOS (device/simulator).
# Metal-only (no Vulkan/OpenGL/shaderc), no Qt/tests/GSRunner. Mirrors the
# android-platform/ structure; the dependency recipe (vendored sources, no
# find_package for core deps) follows ARMSX2's iOS cmake (ARMSX2 247fa6f),
# pointed at our own core. No ARMSX2 GS behavior or policy is imported.
#
# Configure (Ninja; Xcode also works):
#   cmake -S <this> -B <build> -G Ninja \
#     -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_BUILD_TYPE=Release \
#     -DGE1_IOS_DEPS=<dir with zstd freetype sdl3 libjpeg-turbo libpng
#       libwebp lz4 rapidyaml plutovg plutosvg>
# Produces: common/libcommon.a, pcsx2/libPCSX2.a, ge1-adapter/libge1_gs.a.
# The app links those plus Apple frameworks, and compiles the .metal shaders
# itself (Xcode builds default.metallib; PCSX2 falls back to newDefaultLibrary).
cmake_minimum_required(VERSION 3.22)
project(GE1iOS C CXX OBJC OBJCXX)

get_filename_component(GE1_PCSX2_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(GE1_IOS_DEPS "" CACHE PATH "Vendored iOS dependency sources (see header)")
if(NOT GE1_IOS_DEPS)
  message(FATAL_ERROR "GI1: -DGE1_IOS_DEPS=<dir> is required")
endif()

# iOS platform: ARM64, Metal-only, no desktop UI/test/runner targets.
set(IOS TRUE)
set(CMAKE_SYSTEM_PROCESSOR "arm64")
set(CMAKE_OSX_ARCHITECTURES "arm64")
set(CMAKE_OSX_DEPLOYMENT_TARGET "17.0")
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_GSRUNNER OFF CACHE BOOL "" FORCE)
set(USE_VULKAN OFF CACHE BOOL "" FORCE)
set(USE_LINKED_FFMPEG OFF CACHE BOOL "" FORCE)
set(BAKE_SHADERS_IN_CPP OFF CACHE BOOL "" FORCE)

# Our iOS SearchForStuff wins; root modules (Pcsx2Utils, BuildParameters)
# resolve behind it.
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" "${GE1_PCSX2_ROOT}/cmake")
set(TOP_CMAKE_WAS_SOURCED TRUE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include(SearchForStuff)
get_git_version_info()
write_svnrev_h()

add_subdirectory("${GE1_PCSX2_ROOT}/common" "${CMAKE_BINARY_DIR}/common")
# CN1A: ARMSX2's pcsx2/CMakeLists.txt links the header-only libretro-headers target.
add_subdirectory("${GE1_PCSX2_ROOT}/3rdparty/libretro" "${CMAKE_BINARY_DIR}/3rdparty/libretro")
add_subdirectory("${GE1_PCSX2_ROOT}/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")

# CN1A: ARMSX2 2.7.2 has no GSCapture at all, so GSCaptureStub.cpp is no longer built.
# GI1iOSStubs.cpp covers the IOKit disc sources, the curl downloader factory
# and the Cocoa achievement chime, all excluded on iOS.
target_sources(PCSX2 PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/GI1iOSStubs.cpp"
  "${CMAKE_CURRENT_SOURCE_DIR}/GI1CocoaTools.mm")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../adapter" "${CMAKE_BINARY_DIR}/ge1-adapter")
