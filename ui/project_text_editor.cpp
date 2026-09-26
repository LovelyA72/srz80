#include "project_text_editor.hpp"
#include <algorithm>
#include <cctype>

namespace srz80::ui {
namespace {
// Keep existing LF/CRLF endings outside the edited range. New lines use the
// document's first newline style. Merely editing a mixed-ending file must not
// rewrite all of its other lines.
std::string restore_line_endings(std::string_view before, const std::string &after) {
    std::string normalized;
    std::vector<size_t> offsets;
    for (size_t i = 0; i < before.size(); ++i) {
        if (before[i] == '\r') continue;
        normalized += before[i];
        offsets.push_back(i);
    }
    if (normalized == after) return std::string(before);
    size_t prefix = 0, suffix = 0;
    while (prefix < normalized.size() && prefix < after.size() && normalized[prefix] == after[prefix]) ++prefix;
    while (suffix < normalized.size() - prefix && suffix < after.size() - prefix &&
           normalized[normalized.size() - 1 - suffix] == after[after.size() - 1 - suffix]) ++suffix;
    const auto first_newline = before.find('\n');
    const bool default_crlf = first_newline != before.npos && first_newline > 0 && before[first_newline - 1] == '\r';
    std::string result;
    for (size_t i = 0; i < after.size(); ++i) {
        if (after[i] == '\n') {
            bool crlf = default_crlf;
            size_t original = normalized.size();
            if (i < prefix) original = i;
            else if (i >= after.size() - suffix) original = normalized.size() - (after.size() - i);
            if (original < offsets.size())
                crlf = offsets[original] > 0 && before[offsets[original] - 1] == '\r';
            if (crlf) result += '\r';
        }
        result += after[i];
    }
    return result;
}
}
TextEditor::Palette editor_palette(std::string_view name) {
    using P = TextEditor::Color;
    auto palette = TextEditor::GetDarkPalette();
    auto set = [&](P role, ImU32 color) { palette[static_cast<size_t>(role)] = color; };
    set(P::background, IM_COL32(15, 15, 15, 255));
    set(P::text, IM_COL32(220, 220, 220, 255));
    set(P::identifier, IM_COL32(220, 220, 220, 255));
    set(P::punctuation, IM_COL32(190, 195, 205, 255));
    set(P::cursor, IM_COL32(245, 245, 245, 255));
    set(P::lineNumber, IM_COL32(130, 135, 145, 255));
    set(P::currentLineNumber, IM_COL32(205, 210, 220, 255));
    set(P::selection, IM_COL32(75, 110, 160, 115));
    ImU32 keyword = IM_COL32(110, 175, 245, 255), string = IM_COL32(210, 160, 125, 255),
          number = IM_COL32(175, 210, 155, 255), comment = IM_COL32(120, 160, 110, 255),
          preprocessor = IM_COL32(195, 150, 220, 255);
    if (name == "Ocean") {
        keyword = IM_COL32(100, 190, 245, 255); string = IM_COL32(135, 210, 195, 255);
        number = IM_COL32(220, 180, 130, 255); comment = IM_COL32(115, 150, 165, 255);
        preprocessor = IM_COL32(180, 160, 230, 255);
    } else if (name == "Pastel") {
        keyword = IM_COL32(195, 165, 245, 255); string = IM_COL32(165, 215, 170, 255);
        number = IM_COL32(245, 190, 145, 255); comment = IM_COL32(140, 150, 175, 255);
        preprocessor = IM_COL32(245, 165, 195, 255);
    } else if (name == "Amber") {
        keyword = IM_COL32(240, 195, 100, 255); string = IM_COL32(215, 165, 120, 255);
        number = IM_COL32(245, 215, 155, 255); comment = IM_COL32(150, 160, 115, 255);
        preprocessor = IM_COL32(225, 155, 105, 255);
    }
    set(P::keyword, keyword); set(P::declaration, keyword); set(P::knownIdentifier, keyword);
    set(P::string, string); set(P::number, number); set(P::comment, comment);
    set(P::preprocessor, preprocessor);
    return palette;
}

const TextEditor::Language *editor_language(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension == ".c" || extension == ".h") return TextEditor::Language::C();
    if (extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
        extension == ".hpp" || extension == ".hh" || extension == ".hxx" || extension == ".ipp")
        return TextEditor::Language::Cpp();
    if (extension == ".json") return TextEditor::Language::Json();
    if (extension == ".lua") return TextEditor::Language::Lua();
    if (extension == ".ld" || extension == ".lds") {
        static const TextEditor::Language language = [] {
            auto value = *TextEditor::Language::C();
            value.name = "Linker Script";
            value.preprocess = 0;
            value.singleLineComment = "//";
            value.singleLineCommentAlt = "#";
            value.keywords = {"ENTRY", "EXTERN", "GROUP", "INPUT", "MEMORY", "OUTPUT",
                "OUTPUT_ARCH", "OUTPUT_FORMAT", "PHDRS", "PROVIDE", "PROVIDE_HIDDEN",
                "REGION_ALIAS", "SEARCH_DIR", "SECTIONS", "ASSERT", "KEEP", "SORT",
                "SORT_BY_NAME", "SORT_BY_ALIGNMENT", "AT", "ALIGN", "FILL", "NOLOAD"};
            value.declarations = {"ORIGIN", "LENGTH", "ADDR", "LOADADDR", "SIZEOF",
                "ALIGNOF", "DEFINED", "ABSOLUTE", "MAX", "MIN"};
            value.identifiers.clear();
            return value;
        }();
        return &language;
    }
    if (extension == ".md" || extension == ".markdown") return TextEditor::Language::Markdown();
    if (extension == ".py" || extension == ".pyw") return TextEditor::Language::Python();
    if (extension == ".sh" || extension == ".bash") {
        static const TextEditor::Language language = [] {
            auto value = *TextEditor::Language::Python();
            value.name = "Shell";
            value.otherStringStart.clear();
            value.otherStringEnd.clear();
            value.otherStringAltStart.clear();
            value.otherStringAltEnd.clear();
            value.keywords = {"case", "do", "done", "elif", "else", "esac", "fi", "for",
                "function", "if", "in", "select", "then", "time", "until", "while"};
            value.declarations = {"alias", "declare", "export", "local", "readonly", "typeset"};
            value.identifiers = {"break", "cd", "continue", "echo", "eval", "exec", "exit",
                "printf", "read", "return", "set", "shift", "source", "test", "trap", "unset"};
            return value;
        }();
        return &language;
    }
    if (extension == ".cmd" || extension == ".bat") {
        static const TextEditor::Language language = [] {
            auto value = *TextEditor::Language::C();
            value.name = "Windows Command";
            value.caseSensitive = false;
            value.preprocess = 0;
            value.singleLineComment.clear();
            value.commentStart.clear();
            value.commentEnd.clear();
            value.keywords = {"call", "echo", "else", "exit", "for", "goto", "if", "in",
                "not", "pause", "rem", "set", "setlocal", "endlocal", "shift"};
            value.declarations.clear();
            value.identifiers = {"cd", "copy", "del", "dir", "move", "path", "popd",
                "pushd", "ren", "rmdir", "start", "type"};
            return value;
        }();
        return &language;
    }
    if (extension == ".js" || extension == ".mjs" || extension == ".cjs") {
        static const TextEditor::Language language = [] {
            auto value = *TextEditor::Language::C();
            value.name = "JavaScript";
            value.preprocess = 0;
            value.keywords = {"await", "break", "case", "catch", "continue", "debugger", "default",
                "delete", "do", "else", "export", "extends", "finally", "for", "from", "if",
                "import", "in", "instanceof", "new", "of", "return", "super", "switch", "this",
                "throw", "try", "typeof", "void", "while", "with", "yield"};
            value.declarations = {"async", "class", "const", "function", "let", "var"};
            value.identifiers = {"false", "null", "true", "undefined", "NaN", "Infinity"};
            return value;
        }();
        return &language;
    }
    if (extension == ".rb" || extension == ".rake" || extension == ".gemspec") {
        static const TextEditor::Language language = [] {
            auto value = *TextEditor::Language::C();
            value.name = "Ruby";
            value.preprocess = 0;
            value.singleLineComment = "#";
            value.commentStart.clear();
            value.commentEnd.clear();
            value.keywords = {"alias", "and", "begin", "break", "case", "do", "else", "elsif",
                "end", "ensure", "for", "if", "in", "next", "not", "or", "redo", "rescue",
                "retry", "return", "super", "then", "unless", "until", "when", "while", "yield"};
            value.declarations = {"class", "def", "module"};
            value.identifiers = {"false", "nil", "self", "true"};
            return value;
        }();
        return &language;
    }
    if (extension == ".php" || extension == ".phtml") {
        static const TextEditor::Language language = [] {
            auto value = *TextEditor::Language::C();
            value.name = "PHP";
            value.preprocess = 0;
            value.singleLineCommentAlt = "#";
            value.keywords = {"as", "break", "case", "catch", "continue", "default", "do", "else",
                "elseif", "endfor", "endforeach", "endif", "endswitch", "endwhile", "extends",
                "finally", "for", "foreach", "if", "implements", "include", "include_once",
                "instanceof", "new", "require", "require_once", "return", "switch", "throw",
                "try", "use", "while", "yield"};
            value.declarations = {"abstract", "class", "const", "enum", "final", "function",
                "interface", "namespace", "private", "protected", "public", "static", "trait",
                "var"};
            value.identifiers = {"false", "null", "true"};
            return value;
        }();
        return &language;
    }
    if (extension == ".asm" || extension == ".s" || extension == ".z80") {
        static const TextEditor::Language language = [] {
            auto value = *TextEditor::Language::C();
            value.name = "Assembly";
            value.caseSensitive = false;
            value.preprocess = 0;
            value.singleLineComment = ";";
            value.singleLineCommentAlt = "#";
            value.commentStart.clear();
            value.commentEnd.clear();
            value.keywords = {
                "adc", "add", "and", "bit", "call", "ccf", "cp", "cpd", "cpdr", "cpi", "cpir",
                "cpl", "daa", "dec", "di", "djnz", "ei", "ex", "exx", "halt", "im", "in", "inc",
                "ind", "indr", "ini", "inir", "jp", "jr", "ld", "ldd", "lddr", "ldi", "ldir",
                "neg", "nop", "or", "otdr", "otir", "out", "outd", "outi", "pop", "push", "res",
                "ret", "reti", "retn", "rl", "rla", "rlc", "rlca", "rld", "rr", "rra", "rrc",
                "rrca", "rrd", "rst", "sbc", "scf", "set", "sla", "sll", "sra", "srl", "sub",
                "xor", "mov", "movzx", "movsx", "lea", "jmp", "cmp", "test", "je", "jne", "jz",
                "jnz", "int", "shl", "shr", "sar", "mul", "div", "li", "la", "lw", "lh", "lb",
                "sw", "sh", "sb", "addi", "auipc", "lui", "jal", "jalr", "beq", "bne", "blt",
                "bge", "ecall", "ebreak", "fence"};
            value.identifiers = {"a", "f", "b", "c", "d", "e", "h", "l", "i", "r", "af", "bc",
                "de", "hl", "ix", "iy", "sp", "pc", "ixh", "ixl", "iyh", "iyl", "nz", "z", "nc",
                "po", "pe", "p", "m", "eax", "ebx", "ecx", "edx", "rax", "rbx", "rcx", "rdx",
                "rsp", "rbp", "zero", "ra", "gp", "tp", "org", "equ", "db", "defb", "dw", "defw",
                "dd", "ds", "defs", "include", "incbin", "align", "end", "section", "global", "extern",
                "macro", "endm"};
            for (int i = 0; i < 32; ++i) value.identifiers.emplace("x" + std::to_string(i));
            return value;
        }();
        return &language;
    }
    return nullptr;
}

TextEditor::DocPos editor_coordinates(std::string_view text, uint64_t offset) {
    TextEditor::DocPos result;
    for (size_t i = 0; i < std::min<uint64_t>(offset, text.size()); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c == '\n') { ++result.line; result.index = 0; }
        else if (c != '\r' && (c & 0xc0) != 0x80) ++result.index;
    }
    return result;
}
uint64_t editor_offset(std::string_view text, TextEditor::DocPos position) {
    size_t line = 0, index = 0;
    size_t i = 0;
    for (; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (line == position.line && (index >= position.index || c == '\n' || c == '\r')) break;
        if (c == '\n') { ++line; index = 0; }
        else if ((c & 0xc0) != 0x80 && c != '\r') ++index;
    }
    // Never return a position inside a UTF-8 continuation sequence.
    while (i < text.size() && (static_cast<unsigned char>(text[i]) & 0xc0) == 0x80) ++i;
    return i;
}

ProjectTextEditor::ProjectTextEditor() {
    editor_.SetShowWhitespacesEnabled(false);
    editor_.SetShowMiniMapEnabled(false);
    editor_.SetShowScrollbarMiniMapEnabled(false);
    editor_.SetChangeCallback([this] { editor_changed_ = true; });
    editor_.SetTextContextMenuCallback([this](TextEditor::PopupData &) { context_menu(); });
}
void ProjectTextEditor::context_menu() {
    if (!context_document_ || !context_workspace_ || !context_error_)
        return;
    if (ImGui::MenuItem("Cut", "Ctrl+X"))
        command(Command::Cut, *context_document_, *context_workspace_, *context_error_);
    if (ImGui::MenuItem("Copy", "Ctrl+C"))
        command(Command::Copy, *context_document_, *context_workspace_, *context_error_);
    if (ImGui::MenuItem("Paste", "Ctrl+V"))
        command(Command::Paste, *context_document_, *context_workspace_, *context_error_);
    if (ImGui::MenuItem("Select All", "Ctrl+A"))
        command(Command::SelectAll, *context_document_, *context_workspace_, *context_error_);
}
void ProjectTextEditor::sync(const ProjectDocument &document, bool restore_cursor) {
    const bool switched = document_id_ != document.id;
    if (switched || extension_ != document.path.extension().string()) {
        extension_ = document.path.extension().string();
        editor_.SetLanguage(editor_language(document.path));
    }
    if (switched || text_ != document.text.data()) {
        document_id_ = document.id;
        text_ = document.text.data();
        editor_.SetText(text_);
        editor_changed_ = false;
        restore_cursor = true;
    }
    if (restore_cursor) {
        auto cursor = editor_coordinates(text_, document.cursor);
        editor_.SetCursor(cursor);
    }
}
void ProjectTextEditor::commit(ProjectDocument &document, ProjectWorkspace &workspace, std::string &error) {
    auto text = restore_line_endings(text_, editor_.GetText());
    if (text.size() >= document.text.size() || text.find('\0') != std::string::npos) {
        error = "The text editor supports up to 65535 bytes without embedded NUL characters. The edit was not applied.";
        editor_.SetText(text_);
        editor_changed_ = false;
        const auto cursor = editor_coordinates(text_, document.cursor);
        editor_.SetCursor(cursor);
        return;
    }
    const auto cursor = editor_offset(text, editor_.GetMainCursorPosition());
    workspace.update(document.path, text, cursor);
    text_ = std::move(text);
    editor_changed_ = false;
}
bool ProjectTextEditor::render(ProjectDocument &document, ProjectWorkspace &workspace,
                               std::string_view palette, ImFont *font, int &font_size, bool focus,
                               bool show_whitespace, bool show_eol, bool highlight_cursor_line,
                               std::string &error) {
    if (palette_ != palette) { palette_ = palette; editor_.SetPalette(editor_palette(palette)); }
    editor_.SetShowWhitespacesEnabled(show_whitespace);
    editor_.SetShowEndOfLinesEnabled(show_eol);
    editor_.SetHighlightCursorLineEnabled(highlight_cursor_line);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(15, 15, 15, 255));
    ImGui::BeginChild("##project-text", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (focus) ImGui::SetWindowFocus();
    const auto &io = ImGui::GetIO();
    if (ImGui::IsWindowHovered() && io.KeyCtrl && io.MouseWheel != 0.0f)
        font_size = std::clamp(font_size + (io.MouseWheel > 0 ? 1 : -1), 8, 48);
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
        if (ImGui::IsKeyPressed(ImGuiKey_Equal, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadAdd, false))
            font_size = std::clamp(font_size + 1, 8, 48);
        if (ImGui::IsKeyPressed(ImGuiKey_Minus, false) ||
            ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false))
            font_size = std::clamp(font_size - 1, 8, 48);
    }
    ImGui::PushFont(font, static_cast<float>(font_size));
    // The workspace owns undo/redo. The application handles these shortcuts
    // before rendering, so make the widget read-only for this frame to prevent
    // its private history from applying the same command a second time.
    const bool workspace_history_shortcut = io.KeyCtrl &&
        (ImGui::IsKeyPressed(ImGuiKey_Z, false) || ImGui::IsKeyPressed(ImGuiKey_Y, false));
    editor_.SetReadOnlyEnabled(workspace_history_shortcut);
    context_document_ = &document;
    context_workspace_ = &workspace;
    context_error_ = &error;
    editor_.Render("source", ImVec2(0, 0), ImGuiChildFlags_None,
                   ImGuiWindowFlags_NoNavInputs | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoSavedSettings);
    context_document_ = nullptr;
    context_workspace_ = nullptr;
    context_error_ = nullptr;
    editor_.SetReadOnlyEnabled(false);
    const bool targeted = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) ||
        (ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows) &&
         ImGui::IsMouseClicked(ImGuiMouseButton_Right));
    if (editor_changed_) commit(document, workspace, error);
    else document.cursor = editor_offset(text_, editor_.GetMainCursorPosition());
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    return targeted;
}
void ProjectTextEditor::command(Command command, ProjectDocument &document,
                                ProjectWorkspace &workspace, std::string &error) {
    sync(document);
    switch (command) {
    case Command::Copy: editor_.Copy(); break;
    case Command::Cut: editor_.Cut(); break;
    case Command::Paste: editor_.Paste(); break;
    case Command::Delete: {
        const auto selection = editor_.GetMainCursorSelection();
        if (selection.start != selection.end) {
            editor_.ReplaceSectionText(selection, "");
        } else {
            const auto offset = editor_offset(text_, selection.end);
            auto next = offset;
            if (next < text_.size()) {
                if (text_[next] == '\r' && next + 1 < text_.size() && text_[next + 1] == '\n')
                    next += 2;
                else {
                    ++next;
                    while (next < text_.size() &&
                           (static_cast<unsigned char>(text_[next]) & 0xc0) == 0x80)
                        ++next;
                }
                editor_.ReplaceSectionText(selection.start, editor_coordinates(text_, next), "");
            }
        }
        break;
    }
    case Command::SelectAll: editor_.SelectAll(); break;
    }
    if (command == Command::Cut || command == Command::Paste || command == Command::Delete)
        commit(document, workspace, error);
}
}
