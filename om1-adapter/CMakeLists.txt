# NOTE (om1-extract-2 R7): Record.cpp is retained as the recorder reference but
# is not built: it needs the external capture-format header ps2_vu1cap.h (not
# in this tree) plus a VU1CAP capture. The manifest-driven replay below is the
# only built tool.
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
