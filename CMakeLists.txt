cmake_minimum_required(VERSION 3.24)

# These are source-controlled release values. FORCE keeps a reused build tree
# from retaining an older version in its CMake cache after this file changes.
set(SRZ80_GUI_VERSION "0.2.0" CACHE STRING "GUI version" FORCE)
set(SRZ80_ENGINE_VERSION "1.0.10" CACHE STRING "Engine version" FORCE)
project(SRZ80 VERSION "${SRZ80_GUI_VERSION}" LANGUAGES C CXX)

# SRZ80 host: the rack engine, the SDK a plugin is written against, the
# SDL3/ImGui debugger, and the headless frontend.
#
# No plugin is built here.  Cards are shared libraries discovered at run time in
# the executable-relative plugins/ directory, and GUI tools in tools/.  Both come
# from the srz80-plugins repository; the host starts and runs with neither.
#
# Output layout:
#
#   <build>/bin/srz80_gui[.exe]
#   <build>/bin/srz80[.exe]                  simulation CLI and capture runner
#   <build>/bin/srz80engine.dll              Linux: libsrz80engine.so
#   <build>/bin/resources/...                fonts, images
#   <build>/bin/plugins/                     empty until cards are copied in
#   <build>/bin/tools/                       empty until tools are copied in

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

set(SRZ80_GUI_VERSION_PRERELEASE "BETA" CACHE STRING
  "GUI version suffix shown in the About window, or empty" FORCE)
set(SRZ80_ENGINE_VERSION_PRERELEASE "REL" CACHE STRING
  "Engine version suffix shown in the About window, or empty" FORCE)
if(SRZ80_GUI_VERSION_PRERELEASE)
  set(SRZ80_GUI_VERSION_DISPLAY
    "${SRZ80_GUI_VERSION} ${SRZ80_GUI_VERSION_PRERELEASE}")
else()
  set(SRZ80_GUI_VERSION_DISPLAY "${SRZ80_GUI_VERSION}")
endif()
if(SRZ80_ENGINE_VERSION_PRERELEASE)
  set(SRZ80_ENGINE_VERSION_DISPLAY
    "${SRZ80_ENGINE_VERSION} ${SRZ80_ENGINE_VERSION_PRERELEASE}")
else()
  set(SRZ80_ENGINE_VERSION_DISPLAY "${SRZ80_ENGINE_VERSION}")
endif()

# SRZ80 builds with GCC only: the vendored components use GNU extensions, and
# the Windows build is MSYS2/MinGW-w64.  The presets drive single-config
# Makefile generators.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR
    "SRZ80 builds with GCC only (found C=${CMAKE_C_COMPILER_ID} CXX=${CMAKE_CXX_COMPILER_ID}). "
    "Use mingw-* on Windows, mingwcross-* for MinGW-w64 cross builds on Linux, or gcc-* on other hosts.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SRZ80_BUILD_GUI "Build the SDL3/ImGui application" ON)

# Makefile generators are single-config, so the runtime tree is always
# <build>/bin with no per-configuration subdirectory.
set(SRZ80_RUNTIME_DIR "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${SRZ80_RUNTIME_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${SRZ80_RUNTIME_DIR}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

function(srz80_warnings target)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endfunction()

file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated/srz80")
configure_file(cmake/srz80_version.h.in
  "${CMAKE_CURRENT_BINARY_DIR}/generated/srz80/version.h" @ONLY)

include(FetchContent)
find_package(Threads REQUIRED)

# The public C ABI, the engine ABI, and the C++ status helpers.
add_library(srz80_sdk INTERFACE)
target_include_directories(srz80_sdk INTERFACE
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/include"
  "${CMAKE_CURRENT_SOURCE_DIR}/sdk/helpers"
  "${CMAKE_CURRENT_BINARY_DIR}/generated")

include(cmake/Dependencies.cmake)
include(cmake/EngineBackend.cmake)

# -------------------------------------------------------- host libraries -----

# The editable project document and its persistence.
add_library(srz80_project STATIC
  ui/project_session.cpp
  ui/project_persistence.cpp
  ui/project_workspace.cpp
  ui/project_file_state.cpp
  ui/text_codec.cpp)
target_include_directories(srz80_project PUBLIC ui)
target_link_libraries(srz80_project PUBLIC
  srz80_sdk nlohmann_json::nlohmann_json Threads::Threads)
srz80_warnings(srz80_project)

# The simulation runtime reaches the rack only through the engine ABI.  It never
# links an engine implementation: the library that owns the rack is loaded from
# beside the executable at run time.
add_library(srz80_simulation STATIC
  ui/simulation_controller.cpp
  ui/simulation_requests.cpp
  ui/simulation_commands.cpp
  ui/simulation_snapshot.cpp
  ui/simulation_inspection_commands.cpp
  ui/pcm_queue.cpp
  ui/platform_paths.cpp)
target_include_directories(srz80_simulation PUBLIC ui)
target_link_libraries(srz80_simulation PUBLIC
  srz80_sdk srz80_engine nlohmann_json::nlohmann_json Threads::Threads)
srz80_warnings(srz80_simulation)

# ------------------------------------------------------ headless frontend ----

add_executable(srz80 app/headless.cpp)
target_link_libraries(srz80 PRIVATE srz80_engine nlohmann_json::nlohmann_json)
srz80_warnings(srz80)

# ------------------------------------------------------------ GUI frontend --

if(SRZ80_BUILD_GUI)
  add_executable(srz80_gui
    ui/main.cpp
    ui/gui.cpp
    ui/theme.cpp
    ui/host_input.cpp
    ui/video_shader.cpp
    ui/rack_thumbnail.cpp
    ui/welcome.cpp
    ui/inspection.cpp
    ui/audio_backend.cpp
    ui/platform_file_actions.cpp
    ui/tools.cpp
    ui/panels/mixer.cpp
    ui/panels/oscilloscope.cpp
    ui/panels/menu.cpp
    ui/panels/edit.cpp
    ui/panels/rack.cpp
    ui/panels/rack_info.cpp
    ui/panels/clocks.cpp
    ui/panels/inspector.cpp
    ui/panels/memory.cpp
    ui/panels/breakpoints.cpp
    ui/panels/monitor.cpp
    ui/panels/console.cpp
    ui/panels/disassembly.cpp
    ui/panels/project_files.cpp
    ui/panels/settings.cpp
    ui/panels/project_settings.cpp
    ui/panels/video.cpp
    # Application language definitions and palette for ImGuiColorTextEdit.
    ui/project_text_editor.cpp)
  target_include_directories(srz80_gui PRIVATE ui)
  # The video panel uses SDL's renderer internals for the effect pipeline.
  target_include_directories(srz80_gui PRIVATE "${SDL3_SOURCE_DIR}/src/video")
  target_link_libraries(srz80_gui PRIVATE
    srz80_project srz80_simulation srz80_text_editor SDL3::SDL3
    nlohmann_json::nlohmann_json ${CMAKE_DL_LIBS})
  target_compile_definitions(srz80_gui PRIVATE
    SRZ80_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
  if(WIN32)
    target_link_libraries(srz80_gui PRIVATE ole32 shell32)
  endif()
  srz80_warnings(srz80_gui)

  # The GUI resolves tools relative to the executable and falls back to the
  # source tree, so a build tree works in place.
  add_custom_command(TARGET srz80_gui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SRZ80_RUNTIME_DIR}/resources"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/banner-logo.png"
      "${SRZ80_RUNTIME_DIR}/resources/banner-logo.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/banner.png"
      "${SRZ80_RUNTIME_DIR}/resources/banner.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/SarasaMonoSC-Regular.ttf"
      "${SRZ80_RUNTIME_DIR}/resources/SarasaMonoSC-Regular.ttf"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/resources/FontAwesome.otf"
      "${SRZ80_RUNTIME_DIR}/resources/FontAwesome.otf"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SRZ80_RUNTIME_DIR}/licenses/text-editor"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${imgui_color_text_edit_SOURCE_DIR}/LICENSE"
      "${SRZ80_RUNTIME_DIR}/licenses/text-editor/LICENSE"
    VERBATIM)
endif()

# ------------------------------------------------------------ build order ----

# The Rust engine is produced by a Cargo custom target rather than a CMake
# library target, so each consumer needs the build-order edge explicitly.
srz80_engine_dependency(ALL)

# Stage the MinGW runtime next to the executables so a copied Windows build tree
# runs on a machine without MSYS2 or MinGW installed.
if(WIN32)
  set(SRZ80_RUNTIME_STRIP_ARG "")
  if(CMAKE_BUILD_TYPE STREQUAL "Release")
    set(SRZ80_RUNTIME_STRIP_ARG "-DSRZ80_STRIP=${CMAKE_STRIP}")
  endif()
  add_custom_target(srz80_runtime ALL
    COMMAND ${CMAKE_COMMAND}
      "-DSRZ80_RUNTIME_DIR=${SRZ80_RUNTIME_DIR}"
      "-DSRZ80_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DSRZ80_OBJDUMP=${CMAKE_OBJDUMP}"
      ${SRZ80_RUNTIME_STRIP_ARG}
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/StageRuntimeDlls.cmake"
    COMMENT "Staging MinGW runtime DLLs in ${SRZ80_RUNTIME_DIR}"
    VERBATIM)
  add_dependencies(srz80_runtime srz80)
  if(SRZ80_BUILD_GUI)
    add_dependencies(srz80_runtime srz80_gui)
  endif()
endif()

# Release builds ship stripped.  Both linkers keep a symbol table by default, so
# every distributed executable drops it right after linking.  PE exports and ELF
# dynamic symbols live in their own tables, which stripping keeps.
if(CMAKE_BUILD_TYPE STREQUAL "Release")
  if(NOT CMAKE_STRIP)
    message(FATAL_ERROR "Release builds strip their binaries, but no 'strip' program was found")
  endif()
  get_property(srz80_strip_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS srz80_strip_targets)
    get_target_property(target_type ${target} TYPE)
    if(target_type STREQUAL "EXECUTABLE" OR target_type STREQUAL "SHARED_LIBRARY")
      add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${CMAKE_STRIP}" --strip-all "$<TARGET_FILE:${target}>"
        COMMENT "Stripping $<TARGET_FILE_NAME:${target}>"
        VERBATIM)
    endif()
  endforeach()
endif()

message(STATUS "Host builds into ${SRZ80_RUNTIME_DIR}")
message(STATUS
  "Copy the srz80-plugins build's plugins/ and tools/ directories into ${SRZ80_RUNTIME_DIR} to run a rack")
