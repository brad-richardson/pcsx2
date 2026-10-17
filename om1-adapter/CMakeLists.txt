# OM1 route-a capture tool: replays a VU1CAP file through the microVU JIT with
# the capture recorder attached and writes the offline .s + tables.
# Built only when requested: cmake --build <dir> --target om1_record
add_executable(om1_record Record.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness/RecompilerTestEnvironment.cpp)
target_include_directories(om1_record PRIVATE
    "${CMAKE_BINARY_DIR}/common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness"
    "${CMAKE_CURRENT_SOURCE_DIR}/../.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../../capture-include")
target_link_libraries(om1_record PRIVATE PCSX2_FLAGS PCSX2)

# om1-extract-2: manifest-driven replay. Builds each Micro image from frozen
# ELF pieces (no capture needed) and recompiles every manifest triple.
# Built only when requested: cmake --build <dir> --target om1_replay
add_executable(om1_replay om1_replay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness/RecompilerTestEnvironment.cpp)
target_include_directories(om1_replay PRIVATE
    "${CMAKE_BINARY_DIR}/common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness"
    "${CMAKE_CURRENT_SOURCE_DIR}/../..")
target_link_libraries(om1_replay PRIVATE PCSX2_FLAGS PCSX2)
