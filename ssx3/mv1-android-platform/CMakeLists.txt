cmake_minimum_required(VERSION 3.22)
project(MV1Android C CXX)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
set(TOP_CMAKE_WAS_SOURCED TRUE)
set(ENABLE_QT_UI OFF CACHE BOOL "" FORCE)
set(USE_VULKAN ON CACHE BOOL "" FORCE)
set(BAKE_SHADERS_IN_CPP OFF CACHE BOOL "" FORCE)
set(ENABLE_RECOMPILER_TEST_HOOKS ON CACHE BOOL "" FORCE)

include(Pcsx2Utils)
check_no_parenthesis_in_path()
detect_operating_system()
detect_compiler()
include(BuildParameters)
include(SearchForStuff)
get_git_version_info()
write_svnrev_h()

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../armsx2/3rdparty/libretro" "${CMAKE_BINARY_DIR}/libretro-headers")

add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../armsx2/common" "${CMAKE_BINARY_DIR}/common")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../armsx2/pcsx2" "${CMAKE_BINARY_DIR}/pcsx2")
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../armsx2/mv1-adapter" "${CMAKE_BINARY_DIR}/mv1-adapter")
