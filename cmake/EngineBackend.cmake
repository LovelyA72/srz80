# Engine backend selection and construction.
#
# `sdk/include/srz80/engine.h` is the process-wide rack boundary.  Two
# implementations exist, and both expose the same `srz80_engine` target with the
# same artifact name (`srz80engine.dll` / `libsrz80engine.so`), which is part of
# the ABI contract, so no consumer changes when the backend changes:
#
#   rust  (default, production)  engine/srz80engine, a cdylib built by Cargo
#   cpp   (reference only)       engine/engine.cpp over the C++ core in core/
#
# The Rust engine is the only implementation that ships.  The C++ engine and the
# `srz80_core` archive it links are retained as a reference oracle for the
# differential suite (`tests/engine_probe.cpp` plus
# `tests/compare_engine_probe.py`) and for ABI archaeology; they are compiled
# only when `-DSRZ80_ENGINE_BACKEND=cpp` is requested explicitly.  A default
# configure never contains or links a line of C++ core code.
#
# The Rust branch runs Cargo with the working directory set to engine/, so the
# crate's own `.cargo/config.toml` (when a developer has one) still applies.
# Artifacts land inside the CMake build tree and the DLL/SO is staged beside the
# executables, so a runtime tree never depends on an unrelated copy found
# through the system search path.

set(SRZ80_ENGINE_BACKEND "rust" CACHE STRING
  "Engine implementation: 'rust' (production) or 'cpp' (retained reference engine)")
set_property(CACHE SRZ80_ENGINE_BACKEND PROPERTY STRINGS rust cpp)
if(NOT SRZ80_ENGINE_BACKEND STREQUAL "rust" AND NOT SRZ80_ENGINE_BACKEND STREQUAL "cpp")
  message(FATAL_ERROR
    "SRZ80_ENGINE_BACKEND must be 'rust' or 'cpp' (got '${SRZ80_ENGINE_BACKEND}')")
endif()

set(SRZ80_ENGINE_SOURCE_DIR "${CMAKE_SOURCE_DIR}/engine")
set(SRZ80_ENGINE_CRATE "srz80engine")

if(SRZ80_ENGINE_BACKEND STREQUAL "rust")

  find_program(SRZ80_CARGO_PROGRAM cargo
    HINTS "$ENV{USERPROFILE}/.cargo/bin" "$ENV{HOME}/.cargo/bin")
  if(NOT SRZ80_CARGO_PROGRAM)
    message(FATAL_ERROR
      "The Rust engine needs cargo on PATH (or in ~/.cargo/bin). "
      "Install a Rust toolchain with the host target, see README.md.")
  endif()

  # Cargo keeps debug and release artifacts in separate profiles.  RelWithDebInfo
  # maps to the release profile because the crate sets `opt-level` there; a
  # test-style run with no build type keeps the fast debug profile.
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    set(SRZ80_CARGO_PROFILE "release")
    set(SRZ80_CARGO_PROFILE_FLAG "--release")
  else()
    set(SRZ80_CARGO_PROFILE "debug")
    set(SRZ80_CARGO_PROFILE_FLAG "")
  endif()

  # An explicit target directory keeps the Rust artifacts inside the CMake build
  # tree and away from a developer's ~/.cargo/target.
  set(SRZ80_CARGO_TARGET_DIR "${CMAKE_BINARY_DIR}/rust-engine")
  file(MAKE_DIRECTORY "${SRZ80_CARGO_TARGET_DIR}")

  # On Windows the C++ build is MinGW, so the Rust static and import libraries
  # must be MinGW-compatible too; on other hosts Cargo's default target is the
  # host, which is exactly what the C++ compiler targets.
  if(WIN32)
    set(SRZ80_CARGO_TARGET "x86_64-pc-windows-gnu")
    set(SRZ80_CARGO_TARGET_FLAG "--target" "x86_64-pc-windows-gnu")
    set(SRZ80_ENGINE_SUBDIR
      "${SRZ80_CARGO_TARGET_DIR}/${SRZ80_CARGO_TARGET}/${SRZ80_CARGO_PROFILE}")
  else()
    set(SRZ80_CARGO_TARGET "")
    set(SRZ80_CARGO_TARGET_FLAG "")
    set(SRZ80_ENGINE_SUBDIR "${SRZ80_CARGO_TARGET_DIR}/${SRZ80_CARGO_PROFILE}")
  endif()

  if(WIN32)
    set(SRZ80_ENGINE_LIBRARY "${SRZ80_ENGINE_SUBDIR}/${SRZ80_ENGINE_CRATE}.dll")
    set(SRZ80_ENGINE_IMPLIB
      "${SRZ80_ENGINE_SUBDIR}/lib${SRZ80_ENGINE_CRATE}.dll.a")
  elseif(APPLE)
    set(SRZ80_ENGINE_LIBRARY "${SRZ80_ENGINE_SUBDIR}/lib${SRZ80_ENGINE_CRATE}.dylib")
    set(SRZ80_ENGINE_IMPLIB "${SRZ80_ENGINE_LIBRARY}")
  else()
    set(SRZ80_ENGINE_LIBRARY "${SRZ80_ENGINE_SUBDIR}/lib${SRZ80_ENGINE_CRATE}.so")
    set(SRZ80_ENGINE_IMPLIB "${SRZ80_ENGINE_LIBRARY}")
  endif()
  get_filename_component(SRZ80_ENGINE_LIBRARY_NAME "${SRZ80_ENGINE_LIBRARY}" NAME)
  set(SRZ80_ENGINE_RUNTIME_LIBRARY
    "${SRZ80_RUNTIME_DIR}/${SRZ80_ENGINE_LIBRARY_NAME}")

  # Offline builds.  `engine/vendor` holds extracted crate sources produced by
  # `cargo vendor --versioned-dirs vendor`; when it is present the build replaces
  # crates.io with that directory and forbids network access, which is what makes
  # an air-gapped or sandboxed build reproducible.  The directory is deliberately
  # not committed (it is ~57 MiB for three dependencies), so a fresh clone with
  # network access simply resolves the pinned graph in Cargo.lock instead.
  set(SRZ80_CARGO_CONFIG_ARGS "")
  file(GLOB SRZ80_VENDOR_CRATES "${SRZ80_ENGINE_SOURCE_DIR}/vendor/*")
  if(SRZ80_VENDOR_CRATES)
    file(TO_CMAKE_PATH "${SRZ80_ENGINE_SOURCE_DIR}/vendor" SRZ80_VENDOR_PATH)
    set(SRZ80_CARGO_OFFLINE_CONFIG "${CMAKE_BINARY_DIR}/cargo-vendored.toml")
    file(GENERATE OUTPUT "${SRZ80_CARGO_OFFLINE_CONFIG}" CONTENT
"[source.crates-io]
replace-with = \"vendored-sources\"

[source.vendored-sources]
directory = \"${SRZ80_VENDOR_PATH}\"
")
    list(APPEND SRZ80_CARGO_CONFIG_ARGS --config "${SRZ80_CARGO_OFFLINE_CONFIG}")
    set(SRZ80_CARGO_NET_OFFLINE "true")
    message(STATUS "Rust engine: using vendored crate sources in ${SRZ80_VENDOR_PATH}")
  else()
    set(SRZ80_CARGO_NET_OFFLINE "false")
  endif()

  # Always re-run Cargo: it performs its own incremental checks and this keeps
  # every Rust source file of the crate a build dependency without listing them.
  add_custom_target(srz80_engine_rust ALL
    COMMAND "${CMAKE_COMMAND}" -E env
      "CARGO_TARGET_DIR=${SRZ80_CARGO_TARGET_DIR}"
      "CARGO_NET_OFFLINE=${SRZ80_CARGO_NET_OFFLINE}"
      "SRZ80_ENGINE_VERSION=${SRZ80_ENGINE_VERSION_DISPLAY}"
      "${SRZ80_CARGO_PROGRAM}" build
      --manifest-path "${SRZ80_ENGINE_SOURCE_DIR}/Cargo.toml"
      ${SRZ80_CARGO_PROFILE_FLAG}
      ${SRZ80_CARGO_TARGET_FLAG}
      ${SRZ80_CARGO_CONFIG_ARGS}
    WORKING_DIRECTORY "${SRZ80_ENGINE_SOURCE_DIR}"
    COMMENT "Building the Rust engine (${SRZ80_CARGO_PROFILE} profile)"
    VERBATIM)

  # The crate's own unit tests, run through the same offline/vendored setup as
  # the build so `cmake --build <tree> --target srz80_engine_rust_test` works
  # without network access.  `cargo test` remains available directly in engine/.
  add_custom_target(srz80_engine_rust_test
    COMMAND "${CMAKE_COMMAND}" -E env
      "CARGO_TARGET_DIR=${SRZ80_CARGO_TARGET_DIR}"
      "CARGO_NET_OFFLINE=${SRZ80_CARGO_NET_OFFLINE}"
      "${SRZ80_CARGO_PROGRAM}" test
      --manifest-path "${SRZ80_ENGINE_SOURCE_DIR}/Cargo.toml"
      ${SRZ80_CARGO_PROFILE_FLAG}
      ${SRZ80_CARGO_TARGET_FLAG}
      ${SRZ80_CARGO_CONFIG_ARGS}
    WORKING_DIRECTORY "${SRZ80_ENGINE_SOURCE_DIR}"
    COMMENT "Running the Rust engine unit tests"
    VERBATIM)

  add_library(srz80_engine SHARED IMPORTED GLOBAL)
  if(UNIX)
    # Consumers must link against the copy that is staged beside the
    # executable.  The Rust artifact remains the source for the staging
    # command below, but must not become a DT_NEEDED build-tree path.
    set(SRZ80_ENGINE_IMPORTED_LOCATION "${SRZ80_ENGINE_RUNTIME_LIBRARY}")
  else()
    set(SRZ80_ENGINE_IMPORTED_LOCATION "${SRZ80_ENGINE_LIBRARY}")
  endif()
  set_target_properties(srz80_engine PROPERTIES
    IMPORTED_LOCATION "${SRZ80_ENGINE_IMPORTED_LOCATION}"
    IMPORTED_IMPLIB "${SRZ80_ENGINE_IMPLIB}"
    INTERFACE_INCLUDE_DIRECTORIES
      "${CMAKE_SOURCE_DIR}/sdk/include;${CMAKE_CURRENT_BINARY_DIR}/generated")

  # Keep the runtime tree self-contained: every executable finds the engine
  # beside itself.  Release builds strip the staged copy like every other
  # distributed binary, but never the Cargo artifact itself, so an incremental
  # Cargo rebuild is not confused by a modified output file.  PE exports and ELF
  # dynamic symbols survive `--strip-all`, so the staged engine stays loadable
  # and the ABI export guard keeps working.
  add_custom_command(TARGET srz80_engine_rust POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${SRZ80_ENGINE_LIBRARY}" "${SRZ80_RUNTIME_DIR}/"
    COMMENT "Staging ${SRZ80_ENGINE_CRATE} in ${SRZ80_RUNTIME_DIR}"
    VERBATIM)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_STRIP)
    add_custom_command(TARGET srz80_engine_rust POST_BUILD
      COMMAND "${CMAKE_STRIP}" --strip-all
        "${SRZ80_RUNTIME_DIR}/${SRZ80_ENGINE_LIBRARY_NAME}"
      COMMENT "Stripping ${SRZ80_ENGINE_LIBRARY_NAME}"
      VERBATIM)
  endif()

  message(STATUS
    "Engine backend: rust (${SRZ80_CARGO_PROFILE} profile, target '${SRZ80_CARGO_TARGET}')")

  # An imported library target cannot carry `add_dependencies`, so consumers of
  # the Rust engine need that build-order edge declared on the consumer itself.
  # Every call site below is backend-independent; this is the only place that
  # knows a Cargo target produces the artifact.
  function(srz80_engine_dependency)
    if(ARGC EQUAL 1 AND ARGV0 STREQUAL "ALL")
      get_property(srz80_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
      set(srz80_targets ${srz80_targets} ${ARGN})
      list(REMOVE_ITEM srz80_targets ALL)
    else()
      set(srz80_targets ${ARGN})
    endif()
    foreach(srz80_target IN LISTS srz80_targets)
      if(NOT TARGET ${srz80_target})
        continue()
      endif()
      get_target_property(srz80_target_type ${srz80_target} TYPE)
      if(srz80_target_type STREQUAL "EXECUTABLE")
        add_dependencies(${srz80_target} srz80_engine_rust)
        set_target_properties(${srz80_target} PROPERTIES BUILD_RPATH_USE_ORIGIN TRUE)
      endif()
    endforeach()
  endfunction()

else()

  # Reference-only C++ implementation.  `core/` and `engine/engine.cpp` are kept
  # buildable so the differential probe can be run against the implementation the
  # Rust engine replaced, but nothing in a default build touches them.  The core
  # is a static archive that is only ever linked into this shared library, so it
  # must be position independent, and the artifact name is part of the ABI.
  add_library(srz80_core STATIC
    "${CMAKE_SOURCE_DIR}/core/core.cpp"
    "${CMAKE_SOURCE_DIR}/core/bus.cpp"
    "${CMAKE_SOURCE_DIR}/core/runtime.cpp"
    "${CMAKE_SOURCE_DIR}/core/devices.cpp"
    "${CMAKE_SOURCE_DIR}/core/providers.cpp"
    "${CMAKE_SOURCE_DIR}/core/plugin_data.cpp"
    "${CMAKE_SOURCE_DIR}/core/state.cpp"
    "${CMAKE_SOURCE_DIR}/core/project.cpp"
    "${CMAKE_SOURCE_DIR}/core/config.cpp"
    "${CMAKE_SOURCE_DIR}/core/audio.cpp")
  target_include_directories(srz80_core PUBLIC "${CMAKE_SOURCE_DIR}/core")
  target_link_libraries(srz80_core PUBLIC srz80_sdk PRIVATE ${CMAKE_DL_LIBS})
  set_target_properties(srz80_core PROPERTIES POSITION_INDEPENDENT_CODE YES)
  srz80_warnings(srz80_core)
  target_link_libraries(srz80_core PRIVATE nlohmann_json::nlohmann_json)

  add_library(srz80_engine SHARED "${CMAKE_SOURCE_DIR}/engine/engine.cpp")
  target_link_libraries(srz80_engine PUBLIC srz80_sdk PRIVATE srz80_core ${CMAKE_DL_LIBS})
  target_compile_definitions(srz80_engine PRIVATE SRZ80_ENGINE_BUILD)
  set_target_properties(srz80_engine PROPERTIES
    CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN YES
    RUNTIME_OUTPUT_DIRECTORY "${SRZ80_RUNTIME_DIR}"
    LIBRARY_OUTPUT_DIRECTORY "${SRZ80_RUNTIME_DIR}"
    OUTPUT_NAME "srz80engine")
  if(WIN32)
    set_target_properties(srz80_engine PROPERTIES PREFIX "")
  endif()
  srz80_warnings(srz80_engine)
  message(STATUS
    "Engine backend: cpp (retained C++ reference engine; not a production backend)")

  # The C++ engine is a real CMake target, so linking it already establishes the
  # build order.  The function exists so consumers stay backend-independent.
  function(srz80_engine_dependency)
  endfunction()

endif()
