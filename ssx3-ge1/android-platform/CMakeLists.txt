cmake_minimum_required(VERSION 3.22)
project(GE1Android C CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(TOP_CMAKE_WAS_SOURCED TRUE)
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(USE_VULKAN ON CACHE BOOL "" FORCE)
set(BAKE_SHADERS_IN_CPP OFF CACHE BOOL "" FORCE)
# CN1A (ARMSX2 base): the adapter links the bundled shaderc_combined itself, so the core must not go
# looking for Shaderc::shaderc_shared (pcsx2/CMakeLists.txt). LSFG and adrenotools stay off (their
# defaults): LSFG is never shipped, and Turnip loads through our GE1_VK_TURNIP HAL seam.
set(ARMSX2_USE_BUNDLED_SHADERC ON CACHE BOOL "" FORCE)
# BF2: Android GE1 release candidates (match the ARMSX2 release subset):
# LTO on the hot EE/VU/GS core sources + local binding of intra-.so calls
# (cf. NP1 -Bsymbolic on the runner). Output-only (GS; readback off).
set(LTO_PCSX2_CORE ON CACHE BOOL "" FORCE)
add_link_options(-Wl,-Bsymbolic)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include(SearchForStuff)
get_git_version_info()
write_svnrev_h()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/common" "${CMAKE_BINARY_DIR}/common")
# CN1A: ARMSX2's Vulkan GS sources include VKLibretro.cpp unconditionally and need the
# header-only libretro-headers target before pcsx2/ (as in platforms/android/.../cpp/CMakeLists.txt).
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/3rdparty/libretro" "${CMAKE_BINARY_DIR}/3rdparty/libretro")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../vendor-pcsx2/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../adapter" "${CMAKE_BINARY_DIR}/ge1-adapter")
