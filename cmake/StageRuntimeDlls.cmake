# Stages the MinGW-w64 runtime DLLs that the Windows build imports into the
# runtime root, so a copied build tree starts on a machine that has neither
# MSYS2 nor MinGW installed.
#
# Invoked at build time by the srz80_runtime target:
#
#   cmake -DSRZ80_RUNTIME_DIR=<bin> -DSRZ80_CXX_COMPILER=<g++>
#         [-DSRZ80_OBJDUMP=<objdump>] -P cmake/StageRuntimeDlls.cmake
#
# Every PE image in the runtime tree is inspected with `objdump -p`, including
# the engine library in the runtime root.  A DLL is staged only when the
# compiler can resolve its name to a file inside the
# toolchain; Windows system DLLs never resolve, so they are ignored.  Staged
# DLLs are inspected in turn, which follows chains such as
# libstdc++-6.dll -> libgcc_s_seh-1.dll / libwinpthread-1.dll.
#
# The copies land in the runtime root, which is the executable's own directory.
# Windows searches that directory first for the imports of the executable and of
# every DLL it loads, so bin/plugins and bin/tools are covered by the same
# copies without duplicating them per subdirectory.

# This file runs in script mode (`cmake -P`), outside the top-level project's
# policy scope.  Require the project CMake version so `IN_LIST` below works for
# Windows-hosted and Linux-to-Windows MinGW-w64 builds alike.
cmake_minimum_required(VERSION 3.24)

if(NOT SRZ80_RUNTIME_DIR)
  message(FATAL_ERROR "SRZ80_RUNTIME_DIR is required")
endif()
if(NOT EXISTS "${SRZ80_RUNTIME_DIR}")
  message(FATAL_ERROR "Runtime directory does not exist: ${SRZ80_RUNTIME_DIR}")
endif()
if(NOT SRZ80_CXX_COMPILER OR NOT EXISTS "${SRZ80_CXX_COMPILER}")
  message(FATAL_ERROR "SRZ80_CXX_COMPILER must be an existing MinGW GCC C++ compiler")
endif()
get_filename_component(srz80_compiler_dir "${SRZ80_CXX_COMPILER}" DIRECTORY)

# Libraries a MinGW runtime may provide.  They seed the search so a tree that
# imports nothing (for example a fully static link) is still self-contained;
# entries that this toolchain does not ship are simply skipped.
set(srz80_runtime_candidates
  libgcc_s_seh-1.dll
  libgcc_s_dw2-1.dll
  libstdc++-6.dll
  libwinpthread-1.dll
  libssp-0.dll)

# A referenced library from this set must be findable, otherwise the build tree
# cannot run on a clean machine.
set(srz80_mandatory_runtime
  libgcc_s_seh-1.dll
  libgcc_s_dw2-1.dll
  libstdc++-6.dll
  libwinpthread-1.dll)

# Lists the DLL names imported by one PE image (lower case, duplicates removed).
function(srz80_imported_dlls image out_var)
  set(names "")
  if(SRZ80_OBJDUMP AND EXISTS "${SRZ80_OBJDUMP}")
    execute_process(COMMAND "${SRZ80_OBJDUMP}" -p "${image}"
      OUTPUT_VARIABLE dump ERROR_QUIET RESULT_VARIABLE dump_result)
    if(dump_result EQUAL 0)
      string(REPLACE "\r\n" "\n" dump "${dump}")
      string(REPLACE "\n" ";" lines "${dump}")
      foreach(line IN LISTS lines)
        if(line MATCHES "^[ \t]*DLL Name:[ \t]*(.+[^ \t])[ \t]*$")
          string(TOLOWER "${CMAKE_MATCH_1}" name)
          list(APPEND names "${name}")
        endif()
      endforeach()
    endif()
  endif()
  list(REMOVE_DUPLICATES names)
  set(${out_var} "${names}" PARENT_SCOPE)
endfunction()

# Resolves a DLL name to a file inside the toolchain; empty when it is a system
# DLL or is not shipped with this compiler.
function(srz80_locate_dll name out_var)
  execute_process(COMMAND "${SRZ80_CXX_COMPILER}" "-print-file-name=${name}"
    OUTPUT_VARIABLE answer OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(IS_ABSOLUTE "${answer}" AND EXISTS "${answer}")
    set(${out_var} "${answer}" PARENT_SCOPE)
    return()
  endif()
  foreach(directory "${srz80_compiler_dir}" "${srz80_compiler_dir}/..")
    if(EXISTS "${directory}/${name}")
      set(${out_var} "${directory}/${name}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${out_var} "" PARENT_SCOPE)
endfunction()

set(srz80_pending ${srz80_runtime_candidates})
set(srz80_referenced "")
file(GLOB srz80_images
  "${SRZ80_RUNTIME_DIR}/*.exe"
  "${SRZ80_RUNTIME_DIR}/*.dll"
  "${SRZ80_RUNTIME_DIR}/plugins/*.dll"
  "${SRZ80_RUNTIME_DIR}/tools/*.dll")
foreach(image IN LISTS srz80_images)
  srz80_imported_dlls("${image}" imports)
  list(APPEND srz80_referenced ${imports})
  list(APPEND srz80_pending ${imports})
endforeach()

set(srz80_seen "")
set(srz80_staged "")
set(srz80_missing "")
while(srz80_pending)
  list(POP_FRONT srz80_pending name)
  if(NOT name OR name IN_LIST srz80_seen)
    continue()
  endif()
  list(APPEND srz80_seen "${name}")
  srz80_locate_dll("${name}" source)
  if(NOT source)
    if(name MATCHES "^lib.*\\.dll$" AND name IN_LIST srz80_referenced)
      list(APPEND srz80_missing "${name}")
    endif()
    continue()
  endif()
  get_filename_component(file_name "${source}" NAME)
  if(NOT "${source}" STREQUAL "${SRZ80_RUNTIME_DIR}/${file_name}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
      "${source}" "${SRZ80_RUNTIME_DIR}/${file_name}"
      RESULT_VARIABLE copy_result)
    if(NOT copy_result EQUAL 0)
      message(FATAL_ERROR "Failed to stage ${file_name} from ${source}")
    endif()
  endif()
  list(APPEND srz80_staged "${file_name}")
  srz80_imported_dlls("${SRZ80_RUNTIME_DIR}/${file_name}" nested)
  list(APPEND srz80_referenced ${nested})
  list(APPEND srz80_pending ${nested})
endwhile()

if(srz80_staged)
  list(REMOVE_DUPLICATES srz80_staged)
  list(SORT srz80_staged)
  message(STATUS "Staged MinGW runtime DLLs in ${SRZ80_RUNTIME_DIR}: ${srz80_staged}")
  if(SRZ80_STRIP)
    if(NOT EXISTS "${SRZ80_STRIP}")
      message(FATAL_ERROR "SRZ80_STRIP must be an existing strip program: ${SRZ80_STRIP}")
    endif()
    foreach(file_name IN LISTS srz80_staged)
      execute_process(COMMAND "${SRZ80_STRIP}" --strip-all
        "${SRZ80_RUNTIME_DIR}/${file_name}"
        RESULT_VARIABLE strip_result)
      if(NOT strip_result EQUAL 0)
        message(FATAL_ERROR "Failed to strip staged MinGW runtime DLL: ${file_name}")
      endif()
    endforeach()
    message(STATUS "Stripped staged MinGW runtime DLLs in ${SRZ80_RUNTIME_DIR}")
  endif()
else()
  message(STATUS "No MinGW runtime DLLs staged in ${SRZ80_RUNTIME_DIR}")
endif()

if(srz80_missing)
  list(REMOVE_DUPLICATES srz80_missing)
  list(SORT srz80_missing)
  set(srz80_mandatory_missing "")
  foreach(name IN LISTS srz80_missing)
    if(name IN_LIST srz80_mandatory_runtime)
      list(APPEND srz80_mandatory_missing "${name}")
    endif()
  endforeach()
  if(srz80_mandatory_missing)
    message(FATAL_ERROR
      "The build imports MinGW runtime libraries that ${SRZ80_CXX_COMPILER} cannot resolve: "
      "${srz80_mandatory_missing}. The runtime tree would not start without MSYS2/MinGW on PATH.")
  endif()
  message(WARNING
    "Imported libraries were not found next to ${SRZ80_CXX_COMPILER}: ${srz80_missing}. "
    "Copy them into ${SRZ80_RUNTIME_DIR} before distributing the build.")
endif()
