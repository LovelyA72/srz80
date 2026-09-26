#include "../gui.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>
#include <srz80/version.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace srz80::ui {

void App::open_project_dialog(uint64_t operation) {
    if (!operation) { project_session.request(ProjectSession::ActionKind::open); return; }
    static const SDL_DialogFileFilter filters[] = {
        {"SRZ80 project", "json"},
        {"All files", "*"},
    };
    auto *request = new FileDialogRequest{file_dialog_state, FileDialogAction::open, operation, file_dialog_epoch};
    auto directory = last_project_directory;
    if (directory.empty() && !project_session.path().empty())
        directory = project_session.path().parent_path();
    if (directory.empty())
        directory = base;
    const auto directory_text = directory.string();
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowOpenFileDialog(&App::file_dialog_callback, request, window, filters, 2,
                           directory_text.c_str(), false);
}

void App::save_project_as_dialog(uint64_t operation) {
    if (!operation) { project_session.request(ProjectSession::ActionKind::save_as); return; }
    auto *request = new FileDialogRequest{file_dialog_state, FileDialogAction::save_as, operation, file_dialog_epoch};
    const auto parent = project_session.path().empty() ? last_project_directory
                                                      : project_session.path().parent_path().parent_path();
    const auto default_path = parent / "New project";
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    // The picker supplies a new folder name. Persistence publishes its manifest.
    SDL_ShowSaveFileDialog(&App::file_dialog_callback, request, window, nullptr, 0,
                           default_path.string().c_str());
}

void App::show_new_project_dialog(uint64_t operation) {
    new_project_dialog_operation = operation;
    std::snprintf(new_project_name, sizeof(new_project_name), "%s", "New project");
    auto parent = last_projects_directory;
    if (parent.empty() && !recent_projects.empty())
        parent = recent_projects.front().parent_path().parent_path();
    if (parent.empty() && !last_project_directory.empty())
        parent = last_project_directory.parent_path();
    if (parent.empty()) parent = base;
    std::snprintf(new_project_parent, sizeof(new_project_parent), "%s", parent.string().c_str());
    open_new_project_dialog = true;
}

void App::browse_new_project_parent() {
    auto *request = new FileDialogRequest{file_dialog_state,
                                          FileDialogAction::select_project_parent,
                                          0, file_dialog_epoch};
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowOpenFolderDialog(&App::file_dialog_callback, request, window,
                             new_project_parent[0] ? new_project_parent : nullptr, false);
}

void App::save_state() {
    project_session.request(ProjectSession::ActionKind::save_state);
}

void App::save_state_as_dialog(uint64_t operation) {
    if (!operation) { project_session.request(ProjectSession::ActionKind::save_state_as); return; }
    static const SDL_DialogFileFilter filters[] = {
        {"SRZ80 state", "json"},
        {"All files", "*"},
    };
    auto *request = new FileDialogRequest{file_dialog_state, FileDialogAction::save_state_as, operation, file_dialog_epoch};
    auto default_path = last_project_directory /
                        (!project_session.state_path().empty() ? project_session.state_path().filename()
                                       : std::filesystem::path("untitled.state.json"));
    if (last_project_directory.empty())
        default_path = !project_session.state_path().empty() ? project_session.state_path()
                                     : std::filesystem::path("untitled.state.json");
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowSaveFileDialog(&App::file_dialog_callback, request, window, filters, 2,
                           default_path.string().c_str());
}

void App::load_state_dialog(uint64_t operation) {
    if (!operation) { project_session.request(ProjectSession::ActionKind::open_state); return; }
    static const SDL_DialogFileFilter filters[] = {
        {"SRZ80 state", "json"},
        {"All files", "*"},
    };
    auto *request = new FileDialogRequest{file_dialog_state, FileDialogAction::open_state, operation, file_dialog_epoch};
    auto default_path = last_project_directory;
    if (default_path.empty() && !project_session.state_path().empty())
        default_path = project_session.state_path().parent_path();
    if (default_path.empty() && !project_session.path().empty())
        default_path = project_session.path().parent_path();
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowOpenFileDialog(&App::file_dialog_callback, request, window, filters, 2,
                           default_path.empty() ? nullptr : default_path.string().c_str(),
                           false);
}

void App::select_rom_file_dialog(bool for_card_info, uint32_t image_slot) {
    static const SDL_DialogFileFilter filters[] = {{"All files", "*"}};
    auto &paths = for_card_info ? info_rom_paths : rom_paths;
    if (image_slot >= paths.size())
        return;
    const char *current_path = paths[image_slot].data();
    auto directory = last_project_directory;
    if (directory.empty() && current_path[0]) {
        const auto selected = std::filesystem::path(current_path);
        if (!selected.parent_path().empty())
            directory = selected.parent_path();
    }
    if (directory.empty())
        directory = base;
    const auto directory_text = directory.string();
    const auto action = for_card_info ? FileDialogAction::select_info_rom
                                      : FileDialogAction::select_add_rom;
    auto *request = new FileDialogRequest{file_dialog_state, action, 0, file_dialog_epoch, image_slot};
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowOpenFileDialog(&App::file_dialog_callback, request, window, filters, 1,
                           directory_text.c_str(), false);
}

std::string App::validated_project_file_path(const char *path) const {
    if (!path || !*path)
        throw std::runtime_error("Choose a project file");
    if (project_session.path().empty())
        throw std::runtime_error("Open a project before choosing a file");
    std::error_code ec;
    const auto root = std::filesystem::canonical(project_session.path().parent_path(), ec);
    if (ec)
        throw std::runtime_error("The active project folder is unavailable");
    auto selected = std::filesystem::path(reinterpret_cast<const char8_t *>(path));
    if (!selected.is_absolute())
        selected = root / selected;
    selected = std::filesystem::canonical(selected, ec);
    if (ec || !std::filesystem::is_regular_file(selected, ec) || ec)
        throw std::runtime_error("Project file cannot be opened");
    const auto relative = selected.lexically_relative(root);
    const bool inside_project = !relative.empty() && !relative.is_absolute() &&
                                *relative.begin() != "..";
    const auto utf8 = (inside_project ? relative : selected).u8string();
    std::string result(reinterpret_cast<const char *>(utf8.data()), utf8.size());
    if (result.size() >= sizeof(SrhValue::text))
        throw std::runtime_error("Project file path exceeds 127 UTF-8 bytes");
    return result;
}

void App::select_project_file_dialog(FileDialogAction action, std::string config_key,
                                    srz80::Handle card, uint32_t property_index) {
    if (project_session.path().empty())
        return;
    static const SDL_DialogFileFilter filters[] = {{"All files", "*"}};
    const auto root = project_session.path().parent_path().u8string();
    auto *request = new FileDialogRequest{file_dialog_state, action, 0, file_dialog_epoch,
                                          property_index, card, std::move(config_key)};
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowOpenFileDialog(&App::file_dialog_callback, request, window, filters, 1,
                           reinterpret_cast<const char *>(root.c_str()), false);
}

void App::select_video_shader_file_dialog() {
    static const SDL_DialogFileFilter filters[] = {
        {"SRZ80 video shader", "sdlshader"}, {"All files", "*"}};
    auto directory = last_project_directory;
    if (directory.empty() && !video_shader_path.empty())
        directory = std::filesystem::path(video_shader_path).parent_path();
    if (directory.empty()) directory = base;
    auto *request = new FileDialogRequest{file_dialog_state, FileDialogAction::select_video_shader,
                                          0, file_dialog_epoch};
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    const auto directory_text = directory.string();
    SDL_ShowOpenFileDialog(&App::file_dialog_callback, request, window, filters, 2,
                           directory_text.c_str(), false);
}

void App::select_rack_thumbnail_file_dialog() {
    if (rack_thumbnail_job.valid()) return;
    static const SDL_DialogFileFilter filters[] = {
        {"Image", "png;jpg;jpeg;bmp"}, {"All files", "*"}};
    auto directory = last_project_directory.empty() ? base : last_project_directory;
    auto *request = new FileDialogRequest{file_dialog_state,
                                          FileDialogAction::select_rack_thumbnail,
                                          0, file_dialog_epoch};
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    const auto directory_text = directory.string();
    SDL_ShowOpenFileDialog(&App::file_dialog_callback, request, window, filters, 2,
                           directory_text.c_str(), false);
}

void App::import_layout_dialog() {
    if (layout_dialog_pending || layout_file_operation.valid() || layout_import_step)
        return;
    static const SDL_DialogFileFilter filters[] = {
        {"ImGui layout", "ini"},
        {"All files", "*"},
    };
    layout_dialog_pending = true;
    auto *request = new FileDialogRequest{file_dialog_state, FileDialogAction::import_layout,
                                          0, file_dialog_epoch};
    const auto directory = base.string();
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowOpenFileDialog(&App::file_dialog_callback, request, window, filters, 2,
                           directory.c_str(), false);
}

void App::export_layout_dialog() {
    if (layout_dialog_pending || layout_file_operation.valid() || layout_import_step)
        return;
    static const SDL_DialogFileFilter filters[] = {
        {"ImGui layout", "ini"},
        {"All files", "*"},
    };
    layout_dialog_pending = true;
    auto *request = new FileDialogRequest{file_dialog_state, FileDialogAction::export_layout,
                                          0, file_dialog_epoch};
    const auto default_path = (base / "layout.ini").string();
    file_dialog_state->pending_dialogs.fetch_add(1, std::memory_order_release);
    SDL_ShowSaveFileDialog(&App::file_dialog_callback, request, window, filters, 2,
                           default_path.c_str());
}

void App::process_file_dialog_results() {
    std::vector<PendingFileDialog> results;
    {
        std::lock_guard lock(file_dialog_state->mutex);
        results = std::exchange(file_dialog_state->results, {});
    }
    for (const auto &pending : results) {
        if (pending.epoch != file_dialog_epoch) continue;
        if (pending.action == FileDialogAction::select_add_project_file ||
            pending.action == FileDialogAction::select_info_project_file ||
            pending.action == FileDialogAction::select_inspector_project_file) {
            if (!pending.error.empty()) { error = pending.error; continue; }
            if (pending.path.empty() || project_session.busy()) continue;
            try {
                const auto path = validated_project_file_path(pending.path.c_str());
                if (pending.action == FileDialogAction::select_add_project_file) {
                    auto &buffer = add_project_file_paths.at(pending.config_key);
                    std::snprintf(buffer.data(), buffer.size(), "%s", path.c_str());
                } else if (pending.action == FileDialogAction::select_info_project_file) {
                    auto &buffer = info_project_file_paths.at(pending.config_key);
                    std::snprintf(buffer.data(), buffer.size(), "%s", path.c_str());
                } else {
                    const auto found = std::find_if(project_session.active_cards().begin(),
                                                     project_session.active_cards().end(),
                                                     [&](const auto &card) { return card.first == pending.card; });
                    if (found == project_session.active_cards().end() || !snapshot)
                        throw std::runtime_error("Card is no longer available");
                    const auto properties = snapshot->inspection->properties.find(pending.card);
                    if (properties == snapshot->inspection->properties.end() ||
                        pending.image_slot >= properties->second.size() ||
                        properties->second[pending.image_slot].name != pending.config_key ||
                        !project_file_config_key(pending.config_key))
                        throw std::runtime_error("Project file property is no longer available");
                    auto config = found->second.value("config", nlohmann::json::object());
                    if (!config.is_object())
                        config = nlohmann::json::object();
                    config[pending.config_key] = path;
                    SrhValue value{};
                    value.abi_version = SRH_ABI;
                    value.struct_size = sizeof(value);
                    std::snprintf(value.text, sizeof(value.text), "%s", path.c_str());
                    ProjectSession::Action action;
                    action.kind = ProjectSession::ActionKind::property;
                    action.card = pending.card;
                    action.selected = selected;
                    action.index = pending.image_slot;
                    action.value = value;
                    action.config = std::move(config);
                    project_session.request(std::move(action));
                }
            } catch (const std::exception &exception) { error = exception.what(); }
            continue;
        }
        if (pending.action == FileDialogAction::select_project_parent) {
            if (!pending.error.empty()) error = pending.error;
            else if (!pending.path.empty())
                std::snprintf(new_project_parent, sizeof(new_project_parent), "%s",
                              pending.path.c_str());
            continue;
        }
        if (pending.action == FileDialogAction::select_video_shader) {
            if (!pending.error.empty()) error = pending.error;
            else if (!pending.path.empty()) {
                remember_last_used_directory(pending.path);
                if (settings_draft_active) {
                    settings_draft["video.shader_path"] = pending.path;
                    settings_dirty = true;
                } else {
                    video_shader_path = pending.path;
                    controller.config_set("video.shader_path", video_shader_path);
                    save_config();
                }
            }
            continue;
        }
        if (pending.action == FileDialogAction::select_rack_thumbnail) {
            if (!pending.error.empty()) error = pending.error;
            else if (!pending.path.empty() && !rack_thumbnail_job.valid()) {
                remember_last_used_directory(pending.path);
                rack_thumbnail_import = true;
                rack_thumbnail_generation = project_session.generation();
                rack_thumbnail_job = std::async(std::launch::async,
                    [path = std::filesystem::path(pending.path)] { return load_rack_thumbnail(path); });
            }
            continue;
        }
        if (pending.action == FileDialogAction::import_layout ||
            pending.action == FileDialogAction::export_layout) {
            layout_dialog_pending = false;
            if (!pending.error.empty()) {
                error = pending.error;
            } else if (!pending.path.empty()) {
                if (pending.action == FileDialogAction::import_layout)
                    begin_layout_import(pending.path);
                else
                    begin_layout_export(pending.path);
            }
            continue;
        }
        if (pending.operation) {
            if (welcome_project_request) {
                if (pending.path.empty() || !pending.error.empty()) {
                    show_welcome = true;
                    welcome_project_request = false;
                } else {
                    show_welcome = false;
                }
            }
            project_session.resolve_path(pending.operation, pending.path, pending.error);
            continue;
        }
        if (project_session.busy()) continue;
        if (!pending.error.empty()) { error = pending.error; continue; }
        if (pending.path.empty()) continue;
        try {
            remember_last_used_directory(pending.path);
            if (pending.action == FileDialogAction::select_add_rom) {
                if (pending.image_slot >= rom_paths.size())
                    throw std::runtime_error("ROM field is no longer available");
                auto &path = rom_paths[pending.image_slot];
                if (pending.path.size() >= path.size()) throw std::runtime_error("Selected ROM path is too long");
                std::snprintf(path.data(), path.size(), "%s", pending.path.c_str());
                focus_rom_path = true;
            } else if (pending.action == FileDialogAction::select_info_rom) {
                if (pending.image_slot >= info_rom_paths.size())
                    throw std::runtime_error("ROM field is no longer available");
                auto &path = info_rom_paths[pending.image_slot];
                if (pending.path.size() >= path.size()) throw std::runtime_error("Selected ROM path is too long");
                std::snprintf(path.data(), path.size(), "%s", pending.path.c_str());

            }
        } catch (const std::exception &exception) { error = exception.what(); }
    }
}

void App::request_quit() { project_session.request(ProjectSession::ActionKind::quit); }

void App::draw_project_modals() {
    const bool unsaved = project_session.phase() == ProjectSession::Phase::awaiting_unsaved;
    if (open_new_project_dialog) {
        open_new_project_dialog = false;
        ImGui::OpenPopup("Create New Project");
    }
    if (ImGui::BeginPopupModal("Create New Project", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(440.0f);
        ImGui::InputText("Project name", new_project_name, sizeof(new_project_name));
        ImGui::SetNextItemWidth(360.0f);
        ImGui::InputText("Projects folder", new_project_parent, sizeof(new_project_parent));
        ImGui::SameLine();
        if (ImGui::Button("Browse...")) browse_new_project_parent();
        const auto name = std::filesystem::path(new_project_name);
        const auto parent = std::filesystem::path(new_project_parent);
        const bool valid_name = new_project_name[0] && !name.has_parent_path() && name != "." && name != "..";
        const bool valid_parent = new_project_parent[0] && !parent.empty();
        const auto destination = valid_name && valid_parent ? (parent / name).lexically_normal()
                                                            : std::filesystem::path{};
        if (!destination.empty()) ImGui::TextDisabled("%s", destination.string().c_str());
        std::error_code destination_error;
        const bool destination_exists = !destination.empty() &&
                                        std::filesystem::exists(destination, destination_error);
        ImGui::BeginDisabled(!valid_name || !valid_parent || destination_exists || destination_error);
        if (ImGui::Button("Create & Edit")) {
            last_projects_directory = std::filesystem::absolute(parent).lexically_normal();
            save_config();
            const auto operation = new_project_dialog_operation;
            new_project_dialog_operation = 0;
            ImGui::CloseCurrentPopup();
            project_session.resolve_path(operation, destination);
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            const auto operation = new_project_dialog_operation;
            new_project_dialog_operation = 0;
            ImGui::CloseCurrentPopup();
            project_session.resolve_path(operation, {});
        }
        ImGui::EndPopup();
    }
    if (project_session.blocks_simulation()) {
        const auto operation = project_session.operation_id();
        if (snapshot && !snapshot->paused() && !snapshot->stopped() &&
            project_operation_pause_operation != operation) {
            host_input.release(window, controller);
            pending_runtime_command = controller.request_pause();
            project_operation_pause_operation = operation;
        }
    } else {
        project_operation_pause_operation = 0;
    }
    if (unsaved) {
        ImGui::OpenPopup("Unsaved project changes");
    }
    if (ImGui::BeginPopupModal("Unsaved project changes", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("This project has unsaved changes.");
        ImGui::TextDisabled("Save them before continuing?");
        const auto id = project_session.operation_id();
        if (ImGui::Button("Save")) {
            project_session.resolve_unsaved(id, ProjectSession::Decision::save);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't save")) {
            project_session.resolve_unsaved(id, ProjectSession::Decision::discard);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            project_session.resolve_unsaved(id, ProjectSession::Decision::cancel);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const bool conflict = project_session.phase() == ProjectSession::Phase::awaiting_conflict;
    if (conflict) ImGui::OpenPopup("File save conflict");
    ImGui::SetNextWindowSize(ImVec2(620, 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("File save conflict", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("These files changed on disk. Choose which version to keep before saving.");
        ImGui::TextWrapped("%s", project_session.operation_error().c_str());
        bool can_reload = true, can_overwrite = true;
        ImGui::BeginChild("conflicting-files", ImVec2(0, 150), ImGuiChildFlags_Borders);
        for (const auto &file : project_session.save_conflicts()) {
            ImGui::TextWrapped("%s", file.path.string().c_str());
            using FileKind = ProjectFileState::Kind;
            if (file.observed.kind == FileKind::missing) ImGui::TextDisabled("Deleted or moved on disk");
            if (file.observed.kind == FileKind::unreadable) ImGui::TextWrapped("%s", file.observed.error.c_str());
            can_reload &= file.observed.kind == FileKind::present;
            can_overwrite &= file.observed.kind != FileKind::unreadable;
        }
        ImGui::EndChild();
        ImGui::TextWrapped("Keep my edits saves your current versions. Reload disk discards edits in the listed files and cancels this save. Cancel keeps your edits without saving.");
        const auto id = project_session.operation_id();
        using Decision = ProjectSession::ConflictDecision;
        ImGui::BeginDisabled(!can_overwrite);
        if (ImGui::Button("Keep my edits and save")) {
            project_session.resolve_conflicts(id, Decision::overwrite);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!can_reload);
        if (ImGui::Button("Reload disk")) {
            project_session.resolve_conflicts(id, Decision::reload);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            project_session.resolve_conflicts(id, Decision::cancel);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    const bool working = project_session.transitioning() && !unsaved && !conflict &&
                         project_session.phase() != ProjectSession::Phase::awaiting_path;
    if (working) ImGui::OpenPopup("Loading project");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(360.0f, 120.0f), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Loading project", nullptr, ImGuiWindowFlags_NoMove)) {
        if (working) {
            if (project_session.phase() == ProjectSession::Phase::unresolved)
                ImGui::TextWrapped("%s", project_session.operation_error().c_str());
            else ImGui::TextUnformatted(reloading_roms ? "Reloading ROMs..." : "Applying project change…");
        } else ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void App::file_dialog_callback(void *userdata, const char *const *filelist, int) {
    std::unique_ptr<FileDialogRequest> request(static_cast<FileDialogRequest *>(userdata));
    if (!request) return;
    request->state->pending_dialogs.fetch_sub(1, std::memory_order_release);
    PendingFileDialog pending{request->action, request->operation, request->epoch, {}, {},
                              request->image_slot, request->card, request->config_key};
    if (!filelist) {
        const char *sdl_error = SDL_GetError();
        pending.error = sdl_error ? sdl_error : "File dialog failed";
    } else if (filelist[0]) pending.path = filelist[0];
    std::lock_guard lock(request->state->mutex);
    request->state->results.push_back(std::move(pending));
}

void App::menu() {
    if (!ImGui::BeginMainMenuBar())
        return;

    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New Project", "Ctrl+N")) {
            if (project_session.request(ProjectSession::ActionKind::new_project)) {
                welcome_project_request = show_welcome;
                show_welcome = false;
            }
        }
        if (!show_welcome && ImGui::MenuItem("Create New File")) {
            show_project_files = true;
            request_project_file(project_session.path().parent_path(), {});
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Load project...", "Ctrl+O"))
            open_project_dialog();
        if (!recent_projects.empty()) {
            if (ImGui::BeginMenu("Recent projects")) {
                for (size_t index = 0; index < recent_projects.size(); ++index) {
                    const auto &path = recent_projects[index];
                    std::error_code ec;
                    const bool available = std::filesystem::is_regular_file(path, ec);
                    ec.clear();
                    auto absolute = std::filesystem::absolute(path, ec).lexically_normal();
                    if (ec)
                        absolute = path.lexically_normal();
                    auto label = recent_project_names.at(path);
                    if (!available)
                        label += " (missing)";
                    label += "##recent_project_" + std::to_string(index);
                    if (ImGui::MenuItem(label.c_str(), nullptr, false, available)) {
                        show_welcome = false;
                        project_session.request(ProjectSession::ActionKind::open, path);
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip("%s", absolute.string().c_str());
                }
                ImGui::EndMenu();
            }
        }
        if (!show_welcome) {
            ImGui::Separator();
            if (ImGui::MenuItem("Save", "Ctrl+S"))
                project_session.request(ProjectSession::ActionKind::save);
            if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
                save_project_as_dialog();
            if (ImGui::MenuItem("Close Project"))
                project_session.request(ProjectSession::ActionKind::close_project);
            ImGui::Separator();
            if (ImGui::MenuItem("Save State"))
                save_state();
            if (ImGui::MenuItem("Save State As..."))
                save_state_as_dialog();
            if (ImGui::MenuItem("Load State..."))
                load_state_dialog();
            ImGui::Separator();
            if (ImGui::MenuItem("Project Settings...", nullptr, false,
                    project_session.loaded() && !project_session.busy()))
                open_project_settings();
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit"))
            request_quit();
        ImGui::EndMenu();
    }

    if (!show_welcome && ImGui::BeginMenu("Edit")) {
        const bool text_target_available = text_box_available(active_text);
        const auto run_text_command = [&](auto command) {
            command();
            refocus_text_command_target();
        };
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, has_text_undo()))
            edit_undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, has_text_redo()))
            edit_redo();
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", nullptr, false, text_target_available))
            run_text_command([&] { edit_copy(); });
        if (ImGui::MenuItem("Cut", nullptr, false, text_target_available))
            run_text_command([&] { edit_cut(); });
        if (ImGui::MenuItem("Paste", nullptr, false, text_target_available))
            run_text_command([&] { edit_paste(); });
        if (ImGui::MenuItem("Select All", nullptr, false, text_target_available))
            run_text_command([&] { edit_select_all(); });
        if (ImGui::MenuItem("Delete", nullptr, false, text_target_available))
            run_text_command([&] { edit_delete(); });
        ImGui::Separator();
        if (ImGui::MenuItem("Options...", nullptr, &show_settings))
            discard_settings_draft();
        ImGui::Separator();
        if (ImGui::MenuItem("Copy console text", nullptr, false, console_card() != 0))
            copy_console_text();
        ImGui::EndMenu();
    }

    if (!show_welcome && ImGui::BeginMenu("Rack")) {
        ImGui::MenuItem("Rack Info", nullptr, &show_rack_info);
        ImGui::MenuItem("Memory buses", nullptr, &show_memory_buses);
        ImGui::Separator();
        if (ImGui::MenuItem("Add card"))
            show_add_card = true;
        bool selected_alive = false;
        if (snapshot)
            selected_alive = std::any_of(snapshot->inspection->all_cards.begin(), snapshot->inspection->all_cards.end(),
                                         [&](const auto &c) { return c.id == selected; });
        if (ImGui::MenuItem("Remove selected card", nullptr, false, selected_alive))
            remove_card(selected);
        if (ImGui::MenuItem("Clear selection", nullptr, false, selected != 0))
            selected = 0;
        ImGui::EndMenu();
    }

    if (!show_welcome && !tools.empty()) {
        if (ImGui::BeginMenu("Tools")) {
            std::map<std::string, std::vector<LoadedTool *>> by_category;
            for (auto &tool : tools)
                by_category[tool.category].push_back(&tool);
            for (auto &[category, category_tools] : by_category) {
                if (ImGui::BeginMenu(category.c_str())) {
                    for (auto *tool : category_tools)
                        ImGui::MenuItem(tool->api->name, nullptr, &tool->open);
                    ImGui::EndMenu();
                }
            }
            ImGui::EndMenu();
        }
    }

    if (!show_welcome && ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Project files", nullptr, &show_project_files);
        ImGui::MenuItem("Text editor", nullptr, &show_text_editor);
        ImGui::Separator();
        ImGui::MenuItem("Rack", nullptr, &show_rack);
        ImGui::MenuItem("Rack Info", nullptr, &show_rack_info);
        ImGui::MenuItem("Memory buses", nullptr, &show_memory_buses);
        ImGui::MenuItem("Clock control", nullptr, &show_clocks);
        ImGui::MenuItem("Device inspector", nullptr, &show_inspector);
        ImGui::MenuItem("Memory inspector", nullptr, &show_memory);
        ImGui::MenuItem("Breakpoints", nullptr, &show_breakpoints);
        ImGui::MenuItem("Bus monitor", nullptr, &show_monitor);
        ImGui::MenuItem("Console", nullptr, &show_console);
        ImGui::MenuItem("Disassembly", nullptr, &show_disassembly);
        ImGui::MenuItem("Video", nullptr, &show_video);
        ImGui::MenuItem("Log", nullptr, &show_log);
        ImGui::MenuItem("Mixer", nullptr, &show_mixer);
        ImGui::MenuItem("Audio Scope", nullptr, &show_oscilloscope);
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu("Help")) {
        if (ImGui::MenuItem("License"))
            show_license = true;
        if (ImGui::MenuItem("About"))
            show_about = true;
        ImGui::EndMenu();
    }

    if (!show_welcome) {
        // Run/stop status in the top-right of the menu bar.
        const auto run_state = snapshot ? snapshot->run_state : UiRunState::paused;
        const char *status = run_state == UiRunState::running
                                 ? "Running"
                             : run_state == UiRunState::stopped ? "Stopped"
                                                              : "Paused";
        const ImVec4 status_color = run_state == UiRunState::running
                                        ? ImVec4(0.30f, 0.75f, 0.32f, 1.0f)
                                    : run_state == UiRunState::stopped
                                        ? ImVec4(0.80f, 0.25f, 0.20f, 1.0f)
                                        : ImVec4(0.90f, 0.72f, 0.18f, 1.0f);
        const float status_x = ImGui::GetWindowContentRegionMax().x -
                               ImGui::CalcTextSize(status).x - ImGui::GetStyle().FramePadding.x;
        float left_of_status = status_x;
        if (audio_backend.recording()) {
            const float width = ImGui::CalcTextSize("Recording Audio").x + 28.0f;
            left_of_status -= width + 12.0f;
            ImGui::SameLine(left_of_status);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float text_height = ImGui::GetTextLineHeight();
            const float bar_top = ImGui::GetWindowPos().y;
            const float bar_bottom = bar_top + ImGui::GetWindowHeight();
            auto *draw = ImGui::GetWindowDrawList();
            draw->AddRectFilled(ImVec2(p.x, bar_top + 2.0f), ImVec2(p.x + width, bar_bottom - 2.0f),
                                IM_COL32(255, 0, 0, 255));
            const float center_y = (bar_top + bar_bottom) * 0.5f;
            draw->AddCircleFilled(ImVec2(p.x + 9, center_y), 3.5f, IM_COL32_WHITE);
            draw->AddText(ImVec2(p.x + 20, center_y - text_height * 0.5f), IM_COL32_WHITE,
                          "Recording Audio");
            ImGui::Dummy(ImVec2(width, text_height));
        }
        if (video_display_render_time) {
            const double fps = video_display_frame_time_ms > 0.0
                                   ? 1000.0 / video_display_frame_time_ms
                                   : 0.0;
            char fps_label[32]{};
            std::snprintf(fps_label, sizeof(fps_label), "FPS: %.1f", fps);
            ImGui::SameLine(left_of_status - ImGui::CalcTextSize(fps_label).x - 12.0f);
            ImGui::TextUnformatted(fps_label);
        }
        ImGui::SameLine(status_x);
        ImGui::TextColored(status_color, "%s", status);
    }

    ImGui::EndMainMenuBar();
}

void App::license() {
    if (!show_license)
        return;

    struct LibraryLicense {
        const char *library;
        const char *license;
        const char *source;
    };
    static constexpr std::array libraries{
        LibraryLicense{"SDL 3.4.0", "zlib", "https://github.com/libsdl-org/SDL"},
        LibraryLicense{"Dear ImGui 1.92.9b docking", "MIT", "https://github.com/ocornut/imgui"},
        LibraryLicense{"nlohmann/json 3.12.0", "MIT", "https://github.com/nlohmann/json"},
        LibraryLicense{"ImGuiColorTextEdit 1.92.9", "MIT", "https://github.com/goossens/ImGuiColorTextEdit"},
        LibraryLicense{"ImGui Memory Editor 0.59", "MIT", "https://github.com/ocornut/imgui_club"},
        LibraryLicense{"Sarasa Mono SC 1.0.41", "SIL OFL 1.1", "https://github.com/be5invis/Sarasa-Gothic"},
        LibraryLicense{"Tabler Icons 3.35.0", "MIT", "https://github.com/tabler/tabler-icons"},
        LibraryLicense{"cfg-if 1.0.4", "MIT OR Apache-2.0", "https://github.com/rust-lang/cfg-if"},
        LibraryLicense{"itoa 1.0.15", "MIT OR Apache-2.0", "https://github.com/dtolnay/itoa"},
        LibraryLicense{"libloading 0.8.6", "ISC", "https://github.com/nagisa/rust_libloading"},
        LibraryLicense{"memchr 2.7.6", "Unlicense OR MIT", "https://github.com/BurntSushi/memchr"},
        LibraryLicense{"proc-macro2 1.0.106", "MIT OR Apache-2.0", "https://github.com/dtolnay/proc-macro2"},
        LibraryLicense{"quote 1.0.46", "MIT OR Apache-2.0", "https://github.com/dtolnay/quote"},
        LibraryLicense{"ryu 1.0.20", "Apache-2.0 OR BSL-1.0", "https://github.com/dtolnay/ryu"},
        LibraryLicense{"serde 1.0.228", "MIT OR Apache-2.0", "https://github.com/serde-rs/serde"},
        LibraryLicense{"serde_core 1.0.228", "MIT OR Apache-2.0", "https://github.com/serde-rs/serde"},
        LibraryLicense{"serde_derive 1.0.228", "MIT OR Apache-2.0", "https://github.com/serde-rs/serde"},
        LibraryLicense{"serde_json 1.0.145", "MIT OR Apache-2.0", "https://github.com/serde-rs/json"},
        LibraryLicense{"syn 2.0.119", "MIT OR Apache-2.0", "https://github.com/dtolnay/syn"},
        LibraryLicense{"unicode-ident 1.0.24", "(MIT OR Apache-2.0) AND Unicode-3.0", "https://github.com/dtolnay/unicode-ident"},
        LibraryLicense{"windows-targets 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_aarch64_gnullvm 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_aarch64_msvc 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_i686_gnu 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_i686_gnullvm 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_i686_msvc 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_x86_64_gnu 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_x86_64_gnullvm 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
        LibraryLicense{"windows_x86_64_msvc 0.52.6", "MIT OR Apache-2.0", "https://github.com/microsoft/windows-rs"},
    };

    ImGui::SetNextWindowSize(ImVec2(940.0f, 560.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("License", &show_license)) {
        constexpr auto flags = ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
                               ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                               ImGuiTableFlags_SizingStretchProp;
        if (ImGui::BeginTable("open-source-libraries", 3, flags)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Library", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("License", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Project source", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableHeadersRow();
            for (size_t index = 0; index < libraries.size(); ++index) {
                const auto &library = libraries[index];
                ImGui::PushID(static_cast<int>(index));
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(library.library);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(library.license);
                ImGui::TableNextColumn();
                ImGui::TextLinkOpenURL(library.source, library.source);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }
    ImGui::End();
}

void App::about() {
    if (!show_about)
        return;
    ImGui::Begin("About SRZ80", &show_about, ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::TextUnformatted("SRZ80 - Virtual Computer Rack");
    ImGui::Separator();
    ImGui::Text("GUI version: %s", SRZ80_GUI_VERSION_DISPLAY);
    ImGui::Text("Engine version: %s", snapshot && !snapshot->engine_version.empty()
                                          ? snapshot->engine_version.c_str()
                                          : "unknown");
    ImGui::TextUnformatted("(c) 2026 LovelyA72  All rights reserved");
    ImGui::Separator();
    ImGui::TextUnformatted("Greets to: tildearrow, nukeykt, src3453");
    ImGui::End();
}

} // namespace srz80::ui
