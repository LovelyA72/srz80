file(READ "${SOURCE_DIR}/TextEditor.h" header)
file(READ "${SOURCE_DIR}/TextEditor.cpp" implementation)

function(replace_once variable old new)
  string(FIND "${${variable}}" "${old}" position)
  if(position EQUAL -1)
    message(FATAL_ERROR "Bundled text editor changed; cannot apply display patch")
  endif()
  string(REPLACE "${old}" "${new}" updated "${${variable}}")
  set(${variable} "${updated}" PARENT_SCOPE)
endfunction()

replace_once(header "inline void SetShowTabsEnabled(bool value) { config.showTabs = value; }"
  "inline void SetShowEndOfLinesEnabled(bool value) { config.showEndOfLines = value; }\n\tinline void SetHighlightCursorLineEnabled(bool value) { config.highlightCursorLine = value; }\n\tinline void SetShowTabsEnabled(bool value) { config.showTabs = value; }")
replace_once(header "bool showTabs = true;"
  "bool showTabs = true;\n\t\tbool showEndOfLines = false;\n\t\tbool highlightCursorLine = false;")

replace_once(implementation "renderActiveBracketBackground();\n\t\trenderSelections();"
  "if (config.highlightCursorLine && ImGui::IsWindowFocused()) {\n\t\t\tauto row = docPos2VisPos(cursors.getMain().getInteractiveEnd()).row;\n\t\t\tif (row >= firstVisibleRow && row <= lastVisibleRow) {\n\t\t\t\tauto y = cursorScreenPos.y + row * glyphSize.y;\n\t\t\t\tdrawList->AddRectFilled(ImVec2(offset + textLeftOffset, y),\n\t\t\t\t\tImVec2(offset + textRightOffset, y + glyphSize.y), IM_COL32(128, 128, 128, 32));\n\t\t\t}\n\t\t}\n\t\trenderActiveBracketBackground();\n\t\trenderSelections();")
replace_once(implementation "\t\trowScreenPos.y += glyphSize.y;\n\t}\n}\n\n\n//\n//\tTextEditor::renderCursors"
  "\t\tif (config.showEndOfLines && i == line.row + line.rows - 1 &&\n\t\t\tline.foldingState != FoldingState::folded && endColumn >= firstRenderableColumn &&\n\t\t\tendColumn <= lastVisibleColumn) {\n\t\t\tauto x = rowScreenPos.x + endColumn * glyphSize.x + glyphSize.x * 0.5f;\n\t\t\tauto y = rowScreenPos.y + fontSize * 0.5f;\n\t\t\tdrawList->AddCircleFilled(ImVec2(x, y), 2.0f, palette.get(Color::whitespace), 8);\n\t\t}\n\t\trowScreenPos.y += glyphSize.y;\n\t}\n}\n\n\n//\n//\tTextEditor::renderCursors")

file(WRITE "${OUTPUT_DIR}/TextEditor.h" "${header}")
file(WRITE "${OUTPUT_DIR}/TextEditor.cpp" "${implementation}")
