include(FetchContent)
# FetchContent's FETCHCONTENT_SOURCE_DIR_<NAME> overrides support offline sources.
FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
  URL_HASH SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(json)
if(SRZ80_BUILD_GUI)
  FetchContent_Declare(SDL3 URL https://www.libsdl.org/release/SDL3-3.4.0.tar.gz
    URL_HASH SHA256=082cbf5f429e0d80820f68dc2b507a94d4cc1b4e70817b119bbb8ec6a69584b8 DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_TESTS OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(SDL3)
  FetchContent_Declare(imgui URL https://github.com/ocornut/imgui/archive/refs/tags/v1.92.9b-docking.tar.gz
    URL_HASH SHA256=90ded916bd57db2e0e171b6b098940a47c6f5042725dcdc67fb19940ca8bfdcc DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui)
  add_library(srz80_imgui STATIC ${imgui_SOURCE_DIR}/imgui.cpp ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
  target_include_directories(srz80_imgui PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
  target_link_libraries(srz80_imgui PUBLIC SDL3::SDL3)
  set_target_properties(srz80_imgui PROPERTIES POSITION_INDEPENDENT_CODE ON)
  FetchContent_Declare(imgui_color_text_edit
    URL https://github.com/goossens/ImGuiColorTextEdit/archive/refs/tags/v1.92.9.tar.gz
    URL_HASH SHA256=1fe29ec5945dc31783bfc380b51891b03a168008b3de2615c93a0e1aacaf343d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(imgui_color_text_edit)
  set(srz80_editor_patched_dir ${CMAKE_CURRENT_BINARY_DIR}/srz80_text_editor)
  file(MAKE_DIRECTORY ${srz80_editor_patched_dir})
  execute_process(COMMAND ${CMAKE_COMMAND}
    -DSOURCE_DIR=${imgui_color_text_edit_SOURCE_DIR}
    -DOUTPUT_DIR=${srz80_editor_patched_dir}
    -P ${CMAKE_CURRENT_LIST_DIR}/PatchTextEditor.cmake
    RESULT_VARIABLE srz80_editor_patch_result)
  if(NOT srz80_editor_patch_result EQUAL 0)
    message(FATAL_ERROR "Could not patch bundled text editor")
  endif()
  add_library(srz80_text_editor STATIC ${srz80_editor_patched_dir}/TextEditor.cpp)
  target_include_directories(srz80_text_editor PUBLIC ${srz80_editor_patched_dir})
  target_link_libraries(srz80_text_editor PUBLIC srz80_imgui)
  set_target_properties(srz80_text_editor PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
