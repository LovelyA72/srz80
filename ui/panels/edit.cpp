#include "../gui.hpp"
#include <algorithm>
#include <cstring>
#include <imgui_internal.h>
#include <string>

namespace srz80::ui {

void App::note_text_input(char *buffer, size_t capacity) {
    const bool context_click = ImGui::IsItemHovered() &&
        (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
         ImGui::IsMouseReleased(ImGuiMouseButton_Right));
    if (ImGui::IsItemActive() || context_click) {
        active_text = buffer;
        active_text_capacity = capacity;
        active_text_id = ImGui::GetItemID();
    }
}

static void set_text_buffer(char *buffer, size_t capacity, const std::string &value) {
    if (!buffer || capacity == 0)
        return;
    auto length = std::min(value.size(), capacity - 1);
    std::memcpy(buffer, value.data(), length);
    buffer[length] = 0;
}

static ImGuiInputTextState *active_state(ImGuiID id) {
    return ImGui::GetInputTextState(id);
}

void App::replace_path_buffers(std::vector<PathBuffer> &buffers, size_t count) {
    const auto belongs = [&](const char *buffer) {
        return std::any_of(buffers.begin(), buffers.end(),
                           [&](const auto &path) { return buffer == path.data(); });
    };
    std::erase_if(undo_stack, [&](const auto &entry) { return belongs(entry.buffer); });
    std::erase_if(redo_stack, [&](const auto &entry) { return belongs(entry.buffer); });
    if (belongs(active_text)) {
        active_text = nullptr;
        active_text_capacity = 0;
        active_text_id = 0;
    }
    buffers.assign(count, {});
}

bool App::text_box_available(char *buffer) const {
    if (!buffer)
        return false;
    if (buffer == console_input)
        return show_console && console_card() != 0;
    for (auto &path : rom_paths) if (buffer == path.data())
        return show_add_card && add_type >= 0 && add_type < static_cast<int>(card_types.size()) &&
               !card_types[static_cast<size_t>(add_type)].image_slots.empty();
    for (auto &path : info_rom_paths) if (buffer == path.data())
        return show_card_info;
    for (const auto &[path, document] : project_session.workspace().documents()) {
        (void)path;
        if (buffer == document->text.data())
            return true;
    }
    return true;
}

void App::push_text_undo(char *buffer, size_t capacity, const std::string &value) {
    if (!buffer || capacity == 0)
        return;
    for (const auto &[path, document] : project_session.workspace().documents()) {
        (void)path;
        if (buffer == document->text.data())
            return;
    }
    undo_stack.push_back({buffer, capacity, value});
    if (undo_stack.size() > 64)
        undo_stack.erase(undo_stack.begin());
    redo_stack.clear();
}

bool App::has_text_undo() const {
    if (project_session.workspace().can_undo())
        return true;
    for (auto it = undo_stack.rbegin(); it != undo_stack.rend(); ++it)
        if (text_box_available(it->buffer))
            return true;
    return false;
}

bool App::has_text_redo() const {
    if (project_session.workspace().can_redo())
        return true;
    for (auto it = redo_stack.rbegin(); it != redo_stack.rend(); ++it)
        if (text_box_available(it->buffer))
            return true;
    return false;
}

void App::request_focus_for_text_box(char *buffer, bool preserve_selection) {
    focus_console_input = false;
    focus_rom_path = false;
    if (buffer == console_input)
        focus_console_input = true;
    else for (auto &path : rom_paths) if (buffer == path.data()) focus_rom_path = true;
    for (const auto &[path, document] : project_session.workspace().documents()) {
        (void)path;
        if (buffer == document->text.data()) {
            active_project_document_id = document->id;
            show_text_editor = true;
            focus_project_text = true;
            restore_project_text_cursor = !preserve_selection;
            return;
        }
    }
}

void App::refocus_text_command_target() {
    request_focus_for_text_box(active_text, true);
}

void App::edit_copy() {
    if (auto *document = active_project_document(); document &&
        active_text == document->text.data()) {
        project_text_view.command(ProjectTextEditor::Command::Copy, *document,
                                  project_session.workspace(), error);
        return;
    }
    if (!active_text)
        return;
    std::string copied;
    if (auto *state = active_state(active_text_id)) {
        if (state->HasSelection()) {
            int start = state->GetSelectionStart();
            int end = state->GetSelectionEnd();
            if (start > end)
                std::swap(start, end);
            copied.assign(active_text + start, active_text + end);
        }
    }
    if (copied.empty())
        copied = active_text;
    ImGui::SetClipboardText(copied.c_str());
}

void App::edit_cut() {
    if (auto *document = active_project_document(); document &&
        active_text == document->text.data()) {
        project_text_view.command(ProjectTextEditor::Command::Cut, *document,
                                  project_session.workspace(), error);
        return;
    }
    if (!active_text)
        return;
    edit_copy();
    push_text_undo(active_text, active_text_capacity, active_text);
    if (auto *state = active_state(active_text_id)) {
        if (state->HasSelection()) {
            int start = state->GetSelectionStart();
            int end = state->GetSelectionEnd();
            if (start > end)
                std::swap(start, end);
            std::memmove(active_text + start, active_text + end,
                         std::strlen(active_text + end) + 1);
            state->ReloadUserBufAndMoveToEnd();
            return;
        }
    }
    active_text[0] = 0;
    if (auto *state = active_state(active_text_id))
        state->ReloadUserBufAndMoveToEnd();
}

void App::edit_paste() {
    if (auto *document = active_project_document(); document &&
        active_text == document->text.data()) {
        project_text_view.command(ProjectTextEditor::Command::Paste, *document,
                                  project_session.workspace(), error);
        return;
    }
    if (!active_text)
        return;
    const char *clipboard = ImGui::GetClipboardText();
    if (!clipboard || !*clipboard)
        return;
    push_text_undo(active_text, active_text_capacity, active_text);
    std::string combined = active_text;
    combined += clipboard;
    set_text_buffer(active_text, active_text_capacity, combined);
    if (auto *state = active_state(active_text_id))
        state->ReloadUserBufAndMoveToEnd();
}

void App::edit_select_all() {
    if (auto *document = active_project_document(); document &&
        active_text == document->text.data()) {
        project_text_view.command(ProjectTextEditor::Command::SelectAll, *document,
                                  project_session.workspace(), error);
        return;
    }
    if (auto *state = active_state(active_text_id))
        state->SelectAll();
}

void App::edit_delete() {
    if (auto *document = active_project_document(); document &&
        active_text == document->text.data()) {
        project_text_view.command(ProjectTextEditor::Command::Delete, *document,
                                  project_session.workspace(), error);
        return;
    }
    if (!active_text)
        return;
    push_text_undo(active_text, active_text_capacity, active_text);
    if (auto *state = active_state(active_text_id)) {
        if (state->HasSelection()) {
            int start = state->GetSelectionStart();
            int end = state->GetSelectionEnd();
            if (start > end)
                std::swap(start, end);
            std::memmove(active_text + start, active_text + end,
                         std::strlen(active_text + end) + 1);
            state->ReloadUserBufAndMoveToEnd();
            return;
        }
    }
    active_text[0] = 0;
    if (auto *state = active_state(active_text_id))
        state->ReloadUserBufAndMoveToEnd();
}

void App::edit_undo() {
    if (project_session.workspace().can_undo()) {
        ProjectEdit entry;
        if (!project_session.workspace().undo(entry)) return;
        auto &document = *project_session.workspace().find(entry.path);
        if (const auto handler = document_handlers[document.id].lock()) {
            handler->active_path = entry.path;
            handler->open(handler->context, entry.path.string().c_str(),
                                   document.text.data(), std::strlen(document.text.data()),
                                   document.cursor);
            open_tool(handler->plugin_id);
            pending_tool_focus = handler->plugin_id;
        } else {
            active_project_document_id = document.id;
            show_text_editor = true;
            focus_project_text = true;
            restore_project_text_cursor = true;
        }
        return;
    }
    while (!undo_stack.empty() && !text_box_available(undo_stack.back().buffer))
        undo_stack.pop_back();
    if (undo_stack.empty())
        return;
    auto entry = undo_stack.back();
    undo_stack.pop_back();

    redo_stack.push_back({entry.buffer, entry.capacity, entry.buffer ? entry.buffer : ""});
    if (redo_stack.size() > 64)
        redo_stack.erase(redo_stack.begin());

    set_text_buffer(entry.buffer, entry.capacity, entry.value);
    bool reloaded = false;
    if (entry.buffer == active_text) {
        if (auto *state = active_state(active_text_id)) {
            state->ReloadUserBufAndKeepSelection();
            reloaded = true;
        }
    }
    if (!reloaded)
        request_focus_for_text_box(entry.buffer);
}

void App::edit_redo() {
    if (project_session.workspace().can_redo()) {
        ProjectEdit entry;
        if (!project_session.workspace().redo(entry)) return;
        auto &document = *project_session.workspace().find(entry.path);
        if (const auto handler = document_handlers[document.id].lock()) {
            handler->active_path = entry.path;
            handler->open(handler->context, entry.path.string().c_str(),
                                   document.text.data(), std::strlen(document.text.data()),
                                   document.cursor);
            open_tool(handler->plugin_id);
            pending_tool_focus = handler->plugin_id;
        } else {
            active_project_document_id = document.id;
            show_text_editor = true;
            focus_project_text = true;
            restore_project_text_cursor = true;
        }
        return;
    }
    while (!redo_stack.empty() && !text_box_available(redo_stack.back().buffer))
        redo_stack.pop_back();
    if (redo_stack.empty())
        return;
    auto entry = redo_stack.back();
    redo_stack.pop_back();

    undo_stack.push_back({entry.buffer, entry.capacity, entry.buffer ? entry.buffer : ""});
    if (undo_stack.size() > 64)
        undo_stack.erase(undo_stack.begin());

    set_text_buffer(entry.buffer, entry.capacity, entry.value);
    bool reloaded = false;
    if (entry.buffer == active_text) {
        if (auto *state = active_state(active_text_id)) {
            state->ReloadUserBufAndKeepSelection();
            reloaded = true;
        }
    }
    if (!reloaded)
        request_focus_for_text_box(entry.buffer);
}

} // namespace srz80::ui
