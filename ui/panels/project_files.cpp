#include "../gui.hpp"
#include "../platform_file_actions.hpp"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <iterator>
#include <imgui_internal.h>
#include <stdexcept>

namespace srz80::ui {
namespace {
std::filesystem::path new_file_directory;
std::string new_file_extension;
char new_file_name[512]{};
bool open_new_file_formats = false;
bool open_new_file_name = false;

std::string normalized_extension(const std::filesystem::path &path) {
    auto extension = path.extension().string();
    if (!extension.empty() && extension.front() == '.')
        extension.erase(extension.begin());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension;
}
}

void App::open_project_file(const std::filesystem::path &path) {
    try {
        const auto normalized = std::filesystem::absolute(path).lexically_normal();
        if (!project_session.path().empty() &&
            normalized == std::filesystem::absolute(project_session.path()).lexically_normal()) {
            open_project_settings();
            return;
        }
        const auto extension = normalized_extension(normalized);
        const auto handler = std::find_if(project_text_formats.begin(), project_text_formats.end(),
                                          [&](const auto &candidate) {
                                              return candidate->enabled &&
                                                     candidate->extension == extension;
                                          });
        if (handler != project_text_formats.end() && (*handler)->binary) {
            // A binary handler owns its file: the host hands over the path and
            // never loads or saves the contents.
            const auto &selected = *handler;
            const auto status = selected->open(selected->context, normalized.string().c_str(),
                                               "", 0, 0);
            if (status != SRH_OK)
                throw std::runtime_error("Plugin could not open " + normalized.filename().string());
            selected->active_path = normalized;
            open_tool(selected->plugin_id);
            pending_tool_focus = selected->plugin_id;
            error.clear();
            return;
        }
        auto &document = project_session.workspace().open(normalized);
        document_handlers.erase(document.id);
        if (handler != project_text_formats.end()) {
            const auto &selected = *handler;
            document_handlers[document.id] = selected;
            const auto status = selected->open(selected->context, normalized.string().c_str(),
                                               document.text.data(), std::strlen(document.text.data()),
                                               document.cursor);
            if (status != SRH_OK)
                throw std::runtime_error("Plugin could not open " + normalized.filename().string());
            selected->active_path = normalized;
            open_tool(selected->plugin_id);
            pending_tool_focus = selected->plugin_id;
            error.clear();
            return;
        }
        active_project_document_id = document.id;
        focus_project_text = true;
        restore_project_text_cursor = true;
        show_text_editor = true;
        error.clear();
    } catch (const std::exception &exception) {
        error = exception.what();
    }
}

void App::update_project_document(void *handler_context, const std::filesystem::path &path,
                                  std::string_view text, uint64_t cursor, bool record) {
    auto *document = project_session.workspace().find(path);
    if (!document)
        return;
    project_session.workspace().update(path, text, cursor, record);
    const auto handler = document_handlers[document->id].lock();
    if (handler_context && (!handler || handler->context != handler_context))
        document_handlers.erase(document->id);
}

void App::request_project_file(const std::filesystem::path &directory, std::string extension) {
    new_file_directory = directory;
    new_file_extension = std::move(extension);
    if (new_file_extension.empty()) {
        open_new_file_formats = true;
        return;
    }
    const auto suggestion = "new-file." + new_file_extension;
    std::snprintf(new_file_name, sizeof(new_file_name), "%s", suggestion.c_str());
    open_new_file_name = true;
}



void App::project_files() {
    static std::filesystem::path clipboard_path, rename_path, delete_path;
    static bool clipboard_cut = false, rename_directory = false, delete_directory = false,
                delete_permanently = false;
    static char rename_name[256]{};
    bool open_delete_confirmation = false;
    bool open_rename_dialog = false;
    if (!show_project_files)
        return;
    ImGui::SetNextWindowSize(ImVec2(280, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Project files", &show_project_files)) {
        ImGui::End();
        return;
    }
    if (project_session.path().empty()) {
        ImGui::TextDisabled("Save this project to create its workspace.");
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(project_session.busy());
    const auto root = project_session.path().parent_path();
    auto draw_new_file_formats = [&](const std::filesystem::path &directory) {
        if (ImGui::MenuItem("Text file"))
            request_project_file(directory, "txt");
        std::vector<std::string> plugins;
        for (const auto &format : project_text_formats)
            if (format->enabled && !format->binary &&
                std::find(plugins.begin(), plugins.end(), format->plugin_id) == plugins.end())
                plugins.push_back(format->plugin_id);
        std::sort(plugins.begin(), plugins.end());
        for (const auto &plugin : plugins) {
            if (!ImGui::BeginMenu(plugin.c_str()))
                continue;
            for (const auto &format : project_text_formats) {
                if (!format->enabled || format->binary || format->plugin_id != plugin)
                    continue;
                const auto label = format->label + " (*." + format->extension + ")";
                if (ImGui::MenuItem(label.c_str()))
                    request_project_file(directory, format->extension);
            }
            ImGui::EndMenu();
        }
    };
    ImGui::TextDisabled("%s", root.filename().string().c_str());
    ImGui::SameLine();
    if (ImGui::SmallButton("New file"))
        ImGui::OpenPopup("new-file-formats");
    if (ImGui::BeginPopup("new-file-formats")) {
        draw_new_file_formats(root);
        ImGui::EndPopup();
    }
    ImGui::Separator();
    std::function<void(const std::filesystem::path &)> draw_directory;
    draw_directory = [&](const std::filesystem::path &directory) {
        std::vector<std::filesystem::directory_entry> entries;
        std::error_code error_code;
        for (std::filesystem::directory_iterator it(directory, error_code), end; !error_code && it != end;
             it.increment(error_code))
            entries.push_back(*it);
        std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
            const bool left_directory = left.is_directory();
            const bool right_directory = right.is_directory();
            if (left_directory != right_directory)
                return left_directory > right_directory;
            return left.path().filename().string() < right.path().filename().string();
        });
        for (const auto &entry : entries) {
            const auto name = entry.path().filename().string();
            if (entry.is_directory(error_code)) {
                if (ImGui::TreeNodeEx(name.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                    draw_directory(entry.path());
                    ImGui::TreePop();
                }
                if (ImGui::BeginPopupContextItem()) {
                    if (ImGui::BeginMenu("New file")) {
                        draw_new_file_formats(entry.path());
                        ImGui::EndMenu();
                    }
                    if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboard_path.empty())) {
                        std::error_code copy_error;
                        const auto destination = entry.path() / clipboard_path.filename();
                        if (clipboard_cut)
                            std::filesystem::rename(clipboard_path, destination, copy_error);
                        else
                            std::filesystem::copy(clipboard_path, destination,
                                                  std::filesystem::copy_options::recursive, copy_error);
                        if (copy_error)
                            error = "Cannot paste project file";
                        else if (clipboard_cut)
                            clipboard_path.clear();
                    }
                    if (ImGui::MenuItem("Copy Path"))
                        ImGui::SetClipboardText(entry.path().string().c_str());
                    if (ImGui::MenuItem("Copy Relative Path"))
                        ImGui::SetClipboardText(entry.path().lexically_relative(root).string().c_str());
                    if (ImGui::MenuItem("Reveal in File Explorer")) {
                        std::string reveal_error;
                        if (!platform::reveal_in_file_manager(entry.path(), reveal_error))
                            error = std::move(reveal_error);
                    }
                    if (ImGui::MenuItem("Rename...", "F2")) {
                        rename_path = entry.path();
                        rename_directory = true;
                        std::snprintf(rename_name, sizeof(rename_name), "%s", name.c_str());
                        open_rename_dialog = true;
                    }
                    const bool shift_delete = ImGui::GetIO().KeyShift;
                    if (ImGui::MenuItem(shift_delete ? "Delete Permanently" : "Move to Recycle Bin",
                                        shift_delete ? "Shift+Del" : "Del")) {
                        delete_path = entry.path();
                        delete_directory = true;
                        delete_permanently = shift_delete;
                        open_delete_confirmation = true;
                    }
                    ImGui::EndPopup();
                }
            } else if (entry.is_regular_file(error_code)) {
                const auto normalized = std::filesystem::absolute(entry.path()).lexically_normal();
                const auto *document = project_session.workspace().find(normalized);
                const bool dirty = document && document->dirty();
                if (dirty && project_dirty_font)
                    ImGui::PushFont(project_dirty_font);
                const bool project_manifest = normalized == std::filesystem::absolute(project_session.path()).lexically_normal();
                if (ImGui::Selectable((name + "##" + normalized.string()).c_str()))
                    open_project_file(entry.path());
                if (dirty && project_dirty_font)
                    ImGui::PopFont();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%s", entry.path().lexically_relative(root).string().c_str());
                if (ImGui::BeginPopupContextItem()) {
                    if (!project_manifest) {
                        if (ImGui::MenuItem("Cut", "Ctrl+X")) {
                            clipboard_path = entry.path();
                            clipboard_cut = true;
                            ImGui::SetClipboardText(entry.path().string().c_str());
                        }
                        if (ImGui::MenuItem("Copy", "Ctrl+C")) {
                            clipboard_path = entry.path();
                            clipboard_cut = false;
                            ImGui::SetClipboardText(entry.path().string().c_str());
                        }
                    }
                    if (ImGui::MenuItem("Copy Path", "Ctrl+Alt+C"))
                        ImGui::SetClipboardText(entry.path().string().c_str());
                    if (ImGui::MenuItem("Copy Relative Path", "Ctrl+Shift+Alt+C"))
                        ImGui::SetClipboardText(entry.path().lexically_relative(root).string().c_str());
                    if (ImGui::MenuItem("Reveal in File Explorer")) {
                        std::string reveal_error;
                        if (!platform::reveal_in_file_manager(entry.path(), reveal_error))
                            error = std::move(reveal_error);
                    }
                    if (!project_manifest) {
                        ImGui::Separator();
                        if (ImGui::MenuItem("Rename...", "F2")) {
                            rename_path = entry.path();
                            rename_directory = false;
                            std::snprintf(rename_name, sizeof(rename_name), "%s", name.c_str());
                            open_rename_dialog = true;
                        }
                        const bool shift_delete = ImGui::GetIO().KeyShift;
                        if (ImGui::MenuItem(shift_delete ? "Delete Permanently" : "Move to Recycle Bin",
                                            shift_delete ? "Shift+Del" : "Del")) {
                            delete_path = normalized;
                            delete_directory = false;
                            delete_permanently = shift_delete;
                            open_delete_confirmation = true;
                        }
                    }
                    ImGui::EndPopup();
                }
            }
            error_code.clear();
        }
    };
    draw_directory(root);
    if (open_new_file_formats) {
        open_new_file_formats = false;
        ImGui::OpenPopup("new-file-formats");
    }
    if (open_new_file_name) {
        open_new_file_name = false;
        ImGui::OpenPopup("Create project file");
    }
    if (ImGui::BeginPopupModal("Create project file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Path", new_file_name, sizeof(new_file_name));
        const auto relative = std::filesystem::path(new_file_name).lexically_normal();
        const bool valid = new_file_name[0] && relative.is_relative() && relative != "." &&
                           *relative.begin() != "..";
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Create")) {
            try {
                const auto path = (new_file_directory / relative).lexically_normal();
                if (std::filesystem::exists(path))
                    throw std::runtime_error("File already exists: " + path.string());
                std::filesystem::create_directories(path.parent_path());
                std::ofstream output(path, std::ios::binary);
                if (!output) throw std::runtime_error("Cannot create file: " + path.string());
                output.close();
                open_project_file(path);
                ImGui::CloseCurrentPopup();
            } catch (const std::exception &exception) {
                error = exception.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (open_delete_confirmation)
        ImGui::OpenPopup("Confirm project item deletion");
    if (open_rename_dialog)
        ImGui::OpenPopup("Rename project file");
    if (ImGui::BeginPopupModal("Confirm project item deletion", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (delete_permanently) {
            ImGui::Text("Permanently delete '%s'?", delete_path.filename().string().c_str());
            ImGui::TextDisabled("This action cannot be undone.");
        } else {
            ImGui::Text("Move '%s' to the Recycle Bin?", delete_path.filename().string().c_str());
        }
        if (ImGui::Button(delete_permanently ? "Delete Permanently" : "Move to Recycle Bin")) {
            bool deleted = false;
            std::string delete_error;
            if (delete_permanently) {
                std::error_code filesystem_error;
                if (delete_directory)
                    std::filesystem::remove_all(delete_path, filesystem_error);
                else
                    std::filesystem::remove(delete_path, filesystem_error);
                deleted = !filesystem_error;
                if (filesystem_error)
                    delete_error = "Cannot permanently delete " + delete_path.filename().string() +
                                   ": " + filesystem_error.message();
            } else {
                deleted = platform::move_to_trash(delete_path, delete_error);
            }
            if (deleted) {
                const auto normalized_delete = std::filesystem::absolute(delete_path).lexically_normal();
                const auto active_relative = active_project_document()
                    ? active_project_document()->path.lexically_relative(normalized_delete)
                    : std::filesystem::path{};
                if (active_project_document() &&
                    (active_project_document()->path == normalized_delete ||
                     (delete_directory && !active_relative.empty() && *active_relative.begin() != ".."))) {
                    active_project_document_id = 0;
                    active_text = nullptr;
                }
                for (const auto &handler : project_text_formats) {
                    const auto relative = handler->active_path.lexically_relative(normalized_delete);
                    if (handler->active_path == normalized_delete ||
                        (delete_directory && !relative.empty() && *relative.begin() != ".."))
                        handler->active_path.clear();
                }
                if (delete_directory)
                    project_session.workspace().forget_under(normalized_delete);
                else
                    project_session.workspace().forget(normalized_delete);
                error.clear();
            } else {
                error = std::move(delete_error);
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Rename project file", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing())
            ImGui::SetKeyboardFocusHere();
        ImGui::InputText("Name", rename_name, sizeof(rename_name));
        const bool valid_name = rename_name[0] && !std::filesystem::path(rename_name).has_parent_path();
        ImGui::BeginDisabled(!valid_name);
        if (ImGui::Button("Rename")) {
            std::error_code rename_error;
            const auto destination = rename_path.parent_path() / rename_name;
            std::filesystem::rename(rename_path, destination, rename_error);
            if (rename_error)
                error = "Cannot rename " + rename_path.filename().string();
            else {
                if (rename_directory)
                    project_session.workspace().relocate(rename_path, destination);
                else
                    project_session.workspace().rename(rename_path, destination);
                for (const auto &handler : project_text_formats) {
                    const auto relative = handler->active_path.lexically_relative(rename_path);
                    if (handler->active_path.empty() || relative.empty() || *relative.begin() == "..")
                        continue;
                    handler->active_path = rename_directory ? (destination / relative).lexically_normal()
                                                            : destination;
                    if (auto *document = project_session.workspace().find(handler->active_path))
                        handler->open(handler->context, handler->active_path.string().c_str(),
                                      document->text.data(), std::strlen(document->text.data()),
                                      document->cursor);
                }
            }
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel"))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::EndDisabled();
    ImGui::End();
}

void App::text_editor() {
    if (!show_text_editor)
        return;
    ImGui::SetNextWindowSize(ImVec2(820, 580), ImGuiCond_FirstUseEver);
    if (focus_project_text)
        ImGui::SetNextWindowFocus();
    const ProjectDocument *active = active_project_document();
    const bool dirty = active && active->dirty();
    std::string title = "Text editor";
    if (active) {
        auto name = active->path.filename().string();
        if (name.empty())
            name = active->path.string();
        if (!name.empty())
            title += " - " + name;
    }
    title += dirty ? " *" : "";
    title += "###project_text_editor";
    if (!ImGui::Begin(title.c_str(), &show_text_editor)) {
        ImGui::End();
        return;
    }
    ImGui::BeginDisabled(!active_project_document());
    if (active_project_document()) {
        auto &document = *active_project_document();
        if (document.external_change)
            ImGui::TextWrapped("This file changed outside SRZ80. Your edits are preserved; saving will check for conflicts.");
        const int previous_font_size = editor_font_size;
        project_text_view.sync(document, focus_project_text && restore_project_text_cursor);
        if (project_text_view.render(document, project_session.workspace(), editor_theme, editor_font,
                                     editor_font_size, focus_project_text, editor_show_whitespace,
                                     editor_show_eol, editor_highlight_cursor_line, error)) {
            active_text = document.text.data();
            active_text_capacity = document.text.size();
            active_text_id = 0;
        }
        if (editor_font_size != previous_font_size) {
            controller.post_config_set("ui.editor.font_size", std::to_string(editor_font_size));
            editor_font_save_at = ImGui::GetTime() + 0.5;
            if (settings_draft_active) {
                settings_draft["ui.editor.font_size"] = std::to_string(editor_font_size);
                settings_original["ui.editor.font_size"] = std::to_string(editor_font_size);
            }
        }
        focus_project_text = false;
        restore_project_text_cursor = false;
    } else {
        ImGui::TextDisabled("Open a project file to edit it.");
    }

    ImGui::EndDisabled();
    ImGui::End();
}
} // namespace srz80::ui
