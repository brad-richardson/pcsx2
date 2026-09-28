add_executable(mv1_replay Main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness/RecompilerTestEnvironment.cpp)
target_include_directories(mv1_replay PRIVATE
    "${CMAKE_BINARY_DIR}/common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness"
    "${CMAKE_CURRENT_SOURCE_DIR}/../../capture-include")
target_link_libraries(mv1_replay PRIVATE PCSX2_FLAGS PCSX2)
if(ANDROID)
    target_sources(mv1_replay PRIVATE HeadlessAndroidStubs.cpp)
    target_link_libraries(mv1_replay PRIVATE shaderc_combined)
endif()
