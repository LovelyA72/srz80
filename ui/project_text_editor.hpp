#pragma once
#include <TextEditor.h>
#include "project_workspace.hpp"
#include <string_view>

namespace srz80::ui {
TextEditor::Palette editor_palette(std::string_view name);
const TextEditor::Language *editor_language(const std::filesystem::path &path);
TextEditor::DocPos editor_coordinates(std::string_view text, uint64_t offset);
uint64_t editor_offset(std::string_view text, TextEditor::DocPos position);

// GUI view cache only. ProjectWorkspace remains authoritative for text and history.
class ProjectTextEditor {
public:
    enum class Command { Copy, Cut, Paste, Delete, SelectAll };
    ProjectTextEditor();
    void sync(const ProjectDocument &document, bool restore_cursor = false);
    bool render(ProjectDocument &document, ProjectWorkspace &workspace,
                std::string_view palette, ImFont *font, int &font_size, bool focus,
                bool show_whitespace, bool show_eol, bool highlight_cursor_line,
                std::string &error);
    void command(Command command, ProjectDocument &document,
                 ProjectWorkspace &workspace, std::string &error);
private:
    void context_menu();
    void commit(ProjectDocument &document, ProjectWorkspace &workspace, std::string &error);
    TextEditor editor_;
    bool editor_changed_ = false;
    ProjectDocument *context_document_ = nullptr;
    ProjectWorkspace *context_workspace_ = nullptr;
    std::string *context_error_ = nullptr;
    uint64_t document_id_ = 0;
    std::string text_, extension_, palette_;
};
}
