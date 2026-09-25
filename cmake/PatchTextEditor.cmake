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

# The upstream editor treats every non-tab codepoint as one narrow cell. Use
# the active font's advance everywhere columns are calculated, so fullwidth
# glyphs occupy the same two cells in layout, drawing, and hit testing.
replace_once(header [=[		// word wrap limits
		size_t wordWrapColumns = 0;]=]
  [=[		// font metrics for visual columns
		ImFont* layoutFont = nullptr;
		float layoutFontSize = 0.0f;
		float layoutCellWidth = 0.0f;

		// word wrap limits
		size_t wordWrapColumns = 0;]=])
replace_once(header [=[		// current state
		size_t tabSize = 0;]=]
  [=[		// current state
		ImFont* layoutFont = nullptr;
		float layoutFontSize = 0.0f;
		float layoutCellWidth = 0.0f;
		size_t tabSize = 0;]=])

replace_once(implementation [=[//
//	TextEditor::TextEditor]=]
  [=[namespace {
size_t nextColumn(size_t column, ImWchar codepoint, size_t tabSize,
                  ImFont* font, float fontSize, float cellWidth) {
	if (codepoint == '\t')
		return ((column / tabSize) + 1) * tabSize;
	if (!font || cellWidth <= 0.0f)
		return column + 1;
	const float advance = font->GetFontBaked(fontSize)->GetCharAdvance(codepoint);
	const auto cells = static_cast<size_t>(std::max(1l, std::lround(advance / cellWidth)));
	return column + cells;
}
}


//
//	TextEditor::TextEditor]=])
replace_once(implementation [=[	glyphSize = ImVec2(ImGui::CalcTextSize("#").x, ImGui::GetTextLineHeightWithSpacing() * config.lineSpacing);]=]
  [=[	glyphSize = ImVec2(ImGui::CalcTextSize("#").x, ImGui::GetTextLineHeightWithSpacing() * config.lineSpacing);
	config.layoutFont = font;
	config.layoutFontSize = fontSize;
	config.layoutCellWidth = glyphSize.x;]=])
replace_once(implementation [=[				column++;
			}
		}

		// draw ellipsis at the end of folded lines]=]
  [=[				column = nextColumn(column, codepoint, config.tabSize,
					font, fontSize, glyphSize.x);
			}
		}

		// draw ellipsis at the end of folded lines]=])

replace_once(implementation [=[		column += (glyph.codepoint == '\t') ? (config.tabSize - (column % config.tabSize)) : 1;]=]
  [=[		column = nextColumn(column, glyph.codepoint, config.tabSize,
			config.layoutFont, config.layoutFontSize, config.layoutCellWidth);]=])
replace_once(implementation [=[					indent = (codepoint == '\t') ? ((indent / tabSize) + 1) * tabSize : indent + 1;]=]
  [=[					indent = nextColumn(indent, codepoint, tabSize,
						layoutFont, layoutFontSize, layoutCellWidth);]=])
replace_once(implementation [=[			columns = (codepoint == '\t') ? ((columns / tabSize) + 1) * tabSize : columns + 1;]=]
  [=[			const auto next = nextColumn(columns, codepoint, tabSize,
				layoutFont, layoutFontSize, layoutCellWidth);
			// Keep a wide glyph together when it would cross the wrap limit.
			// A glyph wider than an empty row must still be consumed below.
			if (next > wordWrapColumns && columns > (isFirstRow ? 0 : indent) &&
				lastBreakableIndex == lastBreakIndex) {
				sections.emplace_back(lastBreakIndex, i, columns, isFirstRow ? 0 : indent);
				line.rows++;
				columns = indent;
				lastBreakIndex = i;
				lastBreakableIndex = i;
				lastBreakableColumn = indent;
				isFirstRow = false;
				continue;
			}
			columns = next;]=])
replace_once(implementation [=[			line.columns = (glyph.codepoint == '\t') ? ((line.columns / tabSize) + 1) * tabSize : line.columns + 1;]=]
  [=[			line.columns = nextColumn(line.columns, glyph.codepoint, tabSize,
				layoutFont, layoutFontSize, layoutCellWidth);]=])
replace_once(implementation [=[		tabSize != config.tabSize ||
		wordWrap != config.wordWrap ||]=]
  [=[		tabSize != config.tabSize ||
		layoutFont != config.layoutFont ||
		layoutFontSize != config.layoutFontSize ||
		layoutCellWidth != config.layoutCellWidth ||
		wordWrap != config.wordWrap ||]=])
replace_once(implementation [=[		tabSize = config.tabSize;
		wordWrap = config.wordWrap;]=]
  [=[		tabSize = config.tabSize;
		layoutFont = config.layoutFont;
		layoutFontSize = config.layoutFontSize;
		layoutCellWidth = config.layoutCellWidth;
		wordWrap = config.wordWrap;]=])
replace_once(implementation [=[					visPos.column = (glyph->codepoint == '\t') ? ((visPos.column / tabSize) + 1) * tabSize : visPos.column + 1;]=]
  [=[					visPos.column = nextColumn(visPos.column, glyph->codepoint, tabSize,
						layoutFont, layoutFontSize, layoutCellWidth);]=])
replace_once(implementation [=[			visPos.column = (glyph->codepoint == '\t') ? ((visPos.column / tabSize) + 1) * tabSize : visPos.column + 1;]=]
  [=[			visPos.column = nextColumn(visPos.column, glyph->codepoint, tabSize,
				layoutFont, layoutFontSize, layoutCellWidth);]=])
replace_once(implementation [=[				rightColumn = (glyph->codepoint == '\t') ? ((rightColumn / tabSize) + 1) * tabSize : rightColumn + 1;]=]
  [=[				rightColumn = nextColumn(rightColumn, glyph->codepoint, tabSize,
						layoutFont, layoutFontSize, layoutCellWidth);]=])
replace_once(implementation [=[		rightColumn = (glyph->codepoint == '\t') ? ((rightColumn / tabSize) + 1) * tabSize : rightColumn + 1;]=]
  [=[		rightColumn = nextColumn(rightColumn, glyph->codepoint, tabSize,
			layoutFont, layoutFontSize, layoutCellWidth);]=])

file(WRITE "${OUTPUT_DIR}/TextEditor.h" "${header}")
file(WRITE "${OUTPUT_DIR}/TextEditor.cpp" "${implementation}")
