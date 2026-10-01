# OM1 Part 4: runtime dylibs (Mac): offline core + MV2 JIT reference.
# Built only when requested:
#   cmake --build <dir> --target om1_offline mv2_microvu
# Requires -DOM1RT_WORKTREE=<PS2Recomp worktree> (API headers),
# -DOM1RT_SRC=<private rt-src dir> (OfflineCore/Bridge; P5 licensing hygiene),
# -DOM1_REC_DIR=<dir holding rec-sp1 rec-mid rec-full rec-menus rec-late rec-sv1>
# (guest-derived recordings; never committed), and -DOM1_WORKTREE=<fork worktree>
# (static-test fork headers only).
# OM1 private inputs live outside this repo; every one fails fast when unset.
enable_language(ASM)

if(NOT OM1RT_WORKTREE)
    message(FATAL_ERROR "om1rt-adapter needs -DOM1RT_WORKTREE")
endif()
if(NOT OM1RT_SRC)
    message(FATAL_ERROR "om1rt-adapter needs -DOM1RT_SRC")
endif()
if(NOT OM1_REC_DIR)
    message(FATAL_ERROR "om1rt-adapter needs -DOM1_REC_DIR=<dir holding rec-sp1 rec-mid rec-full rec-menus rec-late rec-sv1>")
endif()
if(NOT OM1_WORKTREE)
    message(FATAL_ERROR "om1rt-adapter needs -DOM1_WORKTREE (om1_static_test fork headers)")
endif()

function(om1rt_common target)
    target_include_directories(${target} PRIVATE
        "${CMAKE_BINARY_DIR}/common/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2"
        "${OM1RT_WORKTREE}/ps2xRuntime/include")
    # No PCH: PCSX2_FLAGS' C++ PCH breaks the C tables files.
    set_target_properties(${target} PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
    target_link_libraries(${target} PRIVATE PCSX2_FLAGS PCSX2)
endfunction()

# Offline core: recorded TEXT + tables, no JIT. Links the PCSX2 archive
# AS-IS (no EMBED recompile): bench-identical objects, incl. the parked VU
# thread Reserve() opens. The offline path never calls Reset/Execute, so the
# EMBED differences (thread open, inline INTC) are behavior-identical to the
# 548,966-job bench.
add_library(om1_offline SHARED
    ${OM1RT_SRC}/offlinevu/OfflineCore.cpp
    ${OM1RT_SRC}/offlinevu/OfflineBridge.cpp
    # Host::*/input leaf stubs (same as the bench; Initialize() never runs).
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${OM1_REC_DIR}/rec-sp1/sp1.s
    ${OM1_REC_DIR}/rec-sp1/sp1_tables.c
    ${OM1_REC_DIR}/rec-mid/mid.s
    ${OM1_REC_DIR}/rec-mid/mid_tables.c
    ${OM1_REC_DIR}/rec-full/full.s
    ${OM1_REC_DIR}/rec-full/full_tables.c
    ${OM1_REC_DIR}/rec-menus/menus.s
    ${OM1_REC_DIR}/rec-menus/menus_tables.c
    ${OM1_REC_DIR}/rec-late/late.s
    ${OM1_REC_DIR}/rec-late/late_tables.c
    ${OM1_REC_DIR}/rec-sv1/sv1.s
    ${OM1_REC_DIR}/rec-sv1/sv1_tables.c)
target_include_directories(om1_offline PRIVATE
    ${OM1RT_SRC}/offlinevu
    "${OM1_REC_DIR}/rec-sp1"
    "${OM1_REC_DIR}/rec-mid"
    "${OM1_REC_DIR}/rec-full"
    "${OM1_REC_DIR}/rec-menus"
    "${OM1_REC_DIR}/rec-late"
    "${OM1_REC_DIR}/rec-sv1")
om1rt_common(om1_offline)

# OM1 P5: static-link proof (Mac): prebuilt PCSX2+common archives (lazy
# member-pull = the exact offline closure) + rec pairs + OM1 sources. The
# linker map enumerates the pulled members -> the iOS in-app TU subset
# (fork PS2X_OM1_STAGE block).
add_executable(om1_static_test
    ${OM1RT_SRC}/offlinevu/OfflineCore.cpp
    ${OM1RT_SRC}/offlinevu/OfflineBridge.cpp
    ${OM1RT_SRC}/offlinevu/OM1StubHost.cpp
    ${OM1RT_SRC}/offlinevu/om1_static_main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${OM1_REC_DIR}/rec-sp1/sp1.s
    ${OM1_REC_DIR}/rec-sp1/sp1_tables.c
    ${OM1_REC_DIR}/rec-mid/mid.s
    ${OM1_REC_DIR}/rec-mid/mid_tables.c
    ${OM1_REC_DIR}/rec-full/full.s
    ${OM1_REC_DIR}/rec-full/full_tables.c
    ${OM1_REC_DIR}/rec-menus/menus.s
    ${OM1_REC_DIR}/rec-menus/menus_tables.c
    ${OM1_REC_DIR}/rec-late/late.s
    ${OM1_REC_DIR}/rec-late/late_tables.c
    ${OM1_REC_DIR}/rec-sv1/sv1.s
    ${OM1_REC_DIR}/rec-sv1/sv1_tables.c)
target_include_directories(om1_static_test PRIVATE
    "${CMAKE_BINARY_DIR}/common/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2"
    "${CMAKE_CURRENT_SOURCE_DIR}/../common"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/fast_float/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/fmt/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/rapidjson/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/vixl/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../3rdparty/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness"
    "${OM1RT_SRC}/offlinevu"
    "${OM1RT_WORKTREE}/ps2xRuntime/include"
    "${OM1_WORKTREE}/ps2xRuntime/include"
    "${OM1_REC_DIR}/rec-sp1"
    "${OM1_REC_DIR}/rec-mid"
    "${OM1_REC_DIR}/rec-full"
    "${OM1_REC_DIR}/rec-menus"
    "${OM1_REC_DIR}/rec-late"
    "${OM1_REC_DIR}/rec-sv1")
target_compile_definitions(om1_static_test PRIVATE PCSX2_RECOMPILER_TESTS)
target_compile_options(om1_static_test PRIVATE -fno-exceptions)
set_target_properties(om1_static_test PROPERTIES DISABLE_PRECOMPILE_HEADERS ON)
target_link_libraries(om1_static_test PRIVATE PCSX2_FLAGS PCSX2 common vixl fmt::fmt)
target_link_options(om1_static_test PRIVATE
    "LINKER:-map,${CMAKE_BINARY_DIR}/om1rt-adapter/om1_static.map")

# MV2 JIT reference (Mac port of the Android lib): the bridge + test-env
# harness, with microVU-arm64.cpp recompiled EMBED=1 (no second VU thread,
# no inline INTC) ahead of the archive member.
add_library(mv2_microvu SHARED
    ${OM1RT_WORKTREE}/ps2xRuntime/third_party/microvu/bridge/MicrovuBridge.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2/arm64/microVU-arm64.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/StubHost.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness/RecompilerTestEnvironment.cpp)
target_include_directories(mv2_microvu PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers"
    "${CMAKE_CURRENT_SOURCE_DIR}/../tests/ctest/core/recompilers/harness")
om1rt_common(mv2_microvu)
# EMBED only on the recompiled TU (needs to be a source-file property: the
# bridge/harness compile identically either way, the archive stays non-EMBED).
set_source_files_properties(
    ${CMAKE_CURRENT_SOURCE_DIR}/../pcsx2/arm64/microVU-arm64.cpp PROPERTIES
    COMPILE_DEFINITIONS PS2X_MICROVU_EMBED=1)
