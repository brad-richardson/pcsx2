# OM1 route-a capture tool: replays a VU1CAP file through the microVU JIT with
# the capture recorder attached and writes the offline .s + tables.
# Built only when requested: cmake --build <dir> --target om1_record
# Requires -DOM1_CAPTURE_INCLUDE=<dir holding ps2_vu1cap.h> (private capture
# headers; never committed). Fails fast when unset.
if(NOT OM1_CAPTURE_INCLUDE)
    message(FATAL_ERROR "om1-adapter needs -DOM1_CAPTURE_INCLUDE=<dir holding ps2_vu1cap.h>")
endif()
add_executable(om1_record Record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness/RecompilerTestEnvironment.cpp)
target_include_directories(om1_record PRIVATE
    "${CMAKE_BINARY_DIR}/common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness"
    "${OM1_CAPTURE_INCLUDE}")
target_link_libraries(om1_record PRIVATE PCSX2_FLAGS PCSX2)
