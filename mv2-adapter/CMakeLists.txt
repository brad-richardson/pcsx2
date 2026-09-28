add_library(mv2_microvu SHARED MicrovuBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness/RecompilerTestEnvironment.cpp
    HeadlessAndroidStubs.cpp)
target_include_directories(mv2_microvu PRIVATE
    "${CMAKE_BINARY_DIR}/common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness")
target_compile_options(mv2_microvu PRIVATE -fvisibility=hidden)
target_link_options(mv2_microvu PRIVATE -Wl,--exclude-libs,ALL)
target_link_libraries(mv2_microvu PRIVATE PCSX2_FLAGS PCSX2 shaderc_combined)
