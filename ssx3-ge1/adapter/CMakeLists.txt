# GI1: static archive on iOS (the app links it directly; no dlopen), shared
# library elsewhere (Mac replay, Android).
if(IOS)
  add_library(ge1_gs STATIC ge1_gs.cpp ge1_host.cpp)
else()
  add_library(ge1_gs SHARED ge1_gs.cpp ge1_host.cpp)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/pcsx2/GS.h")
  set(GE1_PCSX2_ROOT "${CMAKE_SOURCE_DIR}")
else()
  set(GE1_PCSX2_ROOT "${CMAKE_SOURCE_DIR}/../vendor-pcsx2")
endif()
target_include_directories(ge1_gs PRIVATE "${GE1_PCSX2_ROOT}" "${CMAKE_BINARY_DIR}/common/include")
target_link_libraries(ge1_gs PRIVATE PCSX2_FLAGS PCSX2)
if(ANDROID)
  target_link_libraries(ge1_gs PRIVATE shaderc_combined)
endif()
target_compile_options(ge1_gs PRIVATE -fvisibility=hidden)
set_target_properties(ge1_gs PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES)
if(APPLE AND NOT IOS)
  target_link_options(ge1_gs PRIVATE "-Wl,-exported_symbols_list,${CMAKE_CURRENT_SOURCE_DIR}/exports.txt")
  target_link_options(ge1_gs PRIVATE "-Wl,-map,${CMAKE_BINARY_DIR}/ge1-link.map")
endif()

if(NOT IOS)
  add_executable(ge1_replay ge1_replay.cpp)
  target_link_libraries(ge1_replay PRIVATE ge1_gs)
  if(ANDROID)
    target_link_libraries(ge1_replay PRIVATE nativewindow)
  endif()
endif()

# GI1: macOS metallibs for GE1_RENDERER=metal replays. Same sources and
# std/target pairs as the desktop generateMetallib() in pcsx2/CMakeLists.txt.
# At replay time, point GE1_GS_RESOURCES_DIR at a directory containing these
# metallibs plus the usual bin/resources content (fonts at minimum).
if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
  set(GE1_METAL_SHADERS
    cas.metal convert.metal present.metal merge.metal misc.metal interlace.metal tfx.metal fxaa.metal)
  function(ge1_metallib std triple outputName)
    set(outs)
    foreach(shader IN LISTS GE1_METAL_SHADERS)
      set(shaderOut ${CMAKE_CURRENT_BINARY_DIR}/metallibs/${outputName}/${shader}.air)
      list(APPEND outs ${shaderOut})
      get_filename_component(shaderDir ${shaderOut} DIRECTORY)
      add_custom_command(OUTPUT ${shaderOut}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${shaderDir}
        COMMAND xcrun metal -ffast-math
          $<$<NOT:$<CONFIG:Release,MinSizeRel>>:-gline-tables-only>
          $<$<NOT:$<CONFIG:Release,MinSizeRel>>:-MO>
          -std=${std} -target ${triple} -o ${shaderOut} -c ${GE1_PCSX2_ROOT}/pcsx2/GS/Renderers/Metal/${shader}
        DEPENDS ${GE1_PCSX2_ROOT}/pcsx2/GS/Renderers/Metal/${shader}
          ${GE1_PCSX2_ROOT}/pcsx2/GS/Renderers/Metal/GSMTLSharedHeader.h
          ${GE1_PCSX2_ROOT}/pcsx2/GS/Renderers/Metal/GSMTLShaderCommon.h)
    endforeach()
    set(metallib ${CMAKE_CURRENT_BINARY_DIR}/metallibs/${outputName}.metallib)
    add_custom_command(OUTPUT ${metallib}
      COMMAND xcrun metallib -o ${metallib} ${outs}
      DEPENDS ${outs})
    list(APPEND GE1_METALLIBS ${metallib})
    set(GE1_METALLIBS ${GE1_METALLIBS} PARENT_SCOPE)
  endfunction()
  ge1_metallib(macos-metal2.0 air64-apple-macos10.13 default)
  ge1_metallib(macos-metal2.3 air64-apple-macos11.0 Metal23)
  add_custom_target(ge1_metallibs ALL DEPENDS ${GE1_METALLIBS})
  add_dependencies(ge1_replay ge1_metallibs)
endif()
