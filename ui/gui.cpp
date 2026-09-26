#include "gui.hpp"
#include "theme.hpp"
#include "text_codec.hpp"
#include <thread>
#include <boundary.hpp>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <imgui_internal.h>
#include <srz80/font_awesome.h>
#include <srz80/imgui_input.hpp>
#include <string_view>
#include <stdexcept>

namespace srz80::ui {
namespace {
constexpr ImWchar font_awesome_ranges[] = {0xF000, 0xF2EE, 0xF500, 0xF500, 0};

std::filesystem::path font_awesome_path(const std::filesystem::path &base) {
    auto path = base / "resources/FontAwesome.otf";
    if (std::filesystem::exists(path))
        return path;
#ifdef SRZ80_SOURCE_DIR
    path = std::filesystem::path(SRZ80_SOURCE_DIR) / "resources/FontAwesome.otf";
    if (std::filesystem::exists(path))
        return path;
#endif
    return {};
}

std::filesystem::path preferred_font_path(const std::string &configured_path,
                                          const std::filesystem::path &base) {
    if (!configured_path.empty() && std::filesystem::exists(configured_path))
        return configured_path;

    const auto sarasa = base / "resources/SarasaMonoSC-Regular.ttf";
    if (std::filesystem::exists(sarasa))
        return sarasa;

#ifdef SRZ80_SOURCE_DIR
    const auto source_sarasa = std::filesystem::path(SRZ80_SOURCE_DIR) /
                               "resources/SarasaMonoSC-Regular.ttf";
    if (std::filesystem::exists(source_sarasa))
        return source_sarasa;
#endif

    return {};
}

void restart_audio_if_needed(App *app) {
    if (!app->loading_config)
        app->start_audio();
}

std::optional<SrzAudioResampling> audio_resampling_value(std::string_view name) {
    if (name == "None") return SRZ_AUDIO_RESAMPLE_NONE;
    if (name == "Linear") return SRZ_AUDIO_RESAMPLE_LINEAR;
    if (name == "Boxcar") return SRZ_AUDIO_RESAMPLE_BOXCAR;
    if (name == "Cosine") return SRZ_AUDIO_RESAMPLE_COSINE;
    if (name == "Sinc") return SRZ_AUDIO_RESAMPLE_SINC;
    return std::nullopt;
}
}

void App::hex_input(const char *label, uint64_t &value) {
    srz80::gui::input_hexadecimal(label, value);
}

srz80::Handle App::find_space(const std::string &name) const {
    if (!snapshot)
        return 0;
    for (const auto &[id, s] : snapshot->inspection->spaces)
        if (s.name == name)
            return id;
    return 0;
}

uint64_t App::snapshot_interval_ms() const {
    if (!snapshot)
        return static_cast<uint64_t>(snapshot_normal_ms);
    return snapshot->high_clock(static_cast<uint32_t>(snapshot_high_threshold_hz))
               ? static_cast<uint64_t>(snapshot_high_ms)
               : static_cast<uint64_t>(snapshot_normal_ms);
}

App::App() {
    controller.set_video_visible(show_video);
    controller.start();
    // Startup may wait for the initial publication. Frame rendering never waits
    // for a lifecycle command. The session thereafter uses exact generations.
    while (!controller.snapshot()->generation)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    project_session.initialize_generation(controller.snapshot()->generation);
    load_config();
    for (const auto &path : recent_projects) {
        std::error_code error;
        if (std::filesystem::is_regular_file(path, error)) recent_project_names[path] = read_project_name(path);
    }
    load_layout_from_config();
    if (auto_load_last_project && !recent_projects.empty() &&
        std::filesystem::is_regular_file(recent_projects.front())) {
        const auto last_project = recent_projects.front().string();
        if (load(last_project)) {
            show_welcome = false;
        } else {
            controller.log_message("[system] Could not restore last project: " + error);
            error.clear();
        }
    }
    apply_ui_settings();
    load_tools();
    // Card descriptors are read once, on the simulation worker.  The add-card
    // dialog shows a discovery hint until the owned metadata arrives.
    card_types_request = controller.discover_card_types_async();
}

App::~App() {
    host_input.release(window, controller);
    ++file_dialog_epoch;
    file_dialog_state.reset();
    project_session.shutdown();
    stop_audio();
    poll_layout_file_operation(true);
    if (!pending_layout_import.empty()) {
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(pending_layout_import.data(),
                                         pending_layout_import.size());
        pending_layout_import.clear();
        layout_initialized = true;
    }
    save_layout_to_config();
    save_config();
    if (welcome_banner) SDL_DestroyTexture(welcome_banner);
    if (welcome_logo) SDL_DestroyTexture(welcome_logo);
    if (rack_thumbnail_job.valid()) rack_thumbnail_job.wait();
    if (rack_thumbnail_texture) SDL_DestroyTexture(rack_thumbnail_texture);
    for (auto &tool : tools)
        if (tool.instance && tool.api && tool.api->destroy)
            tool.api->destroy(tool.instance);
    tools.clear();
    destroy_video_shader();
    controller.shutdown();
    for (auto &[id, entry] : video_textures) {
        (void)id;
        if (entry.texture)
            SDL_DestroyTexture(entry.texture);
        if (entry.cosine_texture)
            SDL_DestroyTexture(entry.cosine_texture);
    }
}

bool App::file_dialog_pending() const {
    if (file_dialog_state->pending_dialogs.load(std::memory_order_acquire) != 0)
        return true;

    for (const auto &[id, state] : tool_dialogs) {
        (void)id;
        std::lock_guard lock(state->mutex);
        if (state->pending)
            return true;
    }
    return false;
}

void App::remember_last_used_directory(const std::filesystem::path &path) {
    if (path.empty())
        return;
    std::error_code ec;
    auto normalized = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec)
        normalized = path.lexically_normal();
    last_project_directory = normalized.parent_path();
}

void App::remember_recent_project(const std::filesystem::path &path) {
    if (path.empty())
        return;
    std::error_code ec;
    auto normalized = std::filesystem::absolute(path, ec).lexically_normal();
    if (ec)
        normalized = path.lexically_normal();
    remember_last_used_directory(normalized);
    recent_project_names[normalized] = project_session.name();
    std::erase(recent_projects, normalized);
    recent_projects.insert(recent_projects.begin(), std::move(normalized));
    if (recent_projects.size() > 8)
        recent_projects.resize(8);
}

bool App::load(const std::filesystem::path &path) {
    ProjectSession::Action action;
    action.kind = ProjectSession::ActionKind::open;
    action.path = path;
    if (!project_session.request(std::move(action))) return false;
    // This adapter is used only by startup and the command-line smoke setup.
    if (project_session.phase() == ProjectSession::Phase::awaiting_unsaved)
        project_session.resolve_unsaved(project_session.operation_id(), ProjectSession::Decision::discard);
    while (project_session.busy()) {
        poll_project_operation();
        if (project_session.phase() == ProjectSession::Phase::unresolved) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return project_session.operation_error().empty();
}

void App::invalidate_project_views(bool clear_workspace) {
    ++file_dialog_epoch;
    rack_info_initialized = false;
    show_add_card = false;
    show_card_info = false;
    info_card = selected = space = add_io_space = 0;
    memory_request = {}; console_request = {}; stack_request = {}; disasm_request = {};
    clipboard_request = {};
    memory_cache.clear(); console_text_cache.clear(); disasm_cache.clear(); stack_cache.clear();
    pending_tool_focus.clear();
    console_text_card = 0;
    tool_dialogs.clear();
    tool_provider_generation = UINT64_MAX;
    active_text = nullptr;
    active_text_capacity = 0;
    active_text_id = 0;
    undo_stack.clear(); redo_stack.clear();
    for (auto &[id, entry] : video_textures) {
        (void)id;
        if (entry.texture) SDL_DestroyTexture(entry.texture);
        if (entry.cosine_texture) SDL_DestroyTexture(entry.cosine_texture);
    }
    video_textures.clear();
    if (clear_workspace) {
        document_handlers.clear();
        active_project_document_id = 0;
        for (auto &handler : project_text_formats) handler->active_path.clear();
    }
}

void App::poll_project_operation() {
    project_session.poll();
    for (;;) {
        auto effects = project_session.take_effects();
        if (effects.empty()) break;
        for (const auto &effect : effects) {
            using Kind = ProjectSession::Effect::Kind;
            switch (effect.kind) {
            case Kind::new_project_path: show_new_project_dialog(effect.operation); break;
            case Kind::open_path: open_project_dialog(effect.operation); break;
            case Kind::save_path: save_project_as_dialog(effect.operation); break;
            case Kind::open_state_path: load_state_dialog(effect.operation); break;
            case Kind::save_state_path: save_state_as_dialog(effect.operation); break;
            case Kind::capture_state:
            case Kind::save: {
                std::string save_error;
                try {
                    if (effect.kind == Kind::save) save_tool_project_files();
                    sync_tool_project_state(true);
                }
                catch (const std::exception &exception) { save_error = exception.what(); }
                project_session.prepare_save(effect.operation, save_error);
                break;
            }
            case Kind::source_reloaded: {
                auto *document = project_session.workspace().find(effect.selected);
                if (!document || document->path != effect.path) break;
                if (const auto handler = document_handlers[document->id].lock();
                    handler && handler->active_path == document->path) {
                    if (handler->open(handler->context, document->path.string().c_str(),
                                      document->text.data(), std::strlen(document->text.data()),
                                      document->cursor) != SRH_OK)
                        error = "File reloaded, but its tool could not refresh the document";
                }
                break;
            }
            case Kind::state_saved:
                remember_last_used_directory(effect.path);
                error.clear();
                break;
            case Kind::saved:
                error.clear();
                remember_recent_project(effect.path);
                last_projects_directory = effect.path.parent_path().parent_path();
                save_config();
                welcome_project_request = false;
                for (const auto &handler : project_text_formats) {
                    if (handler->active_path.empty() || effect.previous_path.empty())
                        continue;
                    const auto relative = handler->active_path.lexically_relative(effect.previous_path);
                    if (relative.empty() || *relative.begin() == "..") continue;
                    const auto relocated = effect.path.parent_path() / relative;
                    const auto *document = project_session.workspace().find(relocated);
                    if (!document || handler->active_path == document->path) continue;
                    handler->active_path = document->path;
                    if (handler->open(handler->context, document->path.string().c_str(),
                                      document->text.data(), std::strlen(document->text.data()),
                                      document->cursor) != SRH_OK)
                        error = "Project saved, but a tool could not reopen its relocated document";
                }
                break;
            case Kind::before_replace:
                host_input.release(window, controller);
                project_operation_restarts_audio = audio_backend.opened();
                if (project_operation_restarts_audio) stop_audio();
                project_session.confirm_replace(effect.operation);
                break;
            case Kind::installed:
                try {
                    invalidate_project_views(effect.clear_workspace);
                    snapshot = controller.snapshot();
                    selected = effect.selected;
                    if (effect.clear_workspace) {
                        load_config();
                        restore_tool_project_state();
                        if (!effect.path.empty()) remember_recent_project(effect.path);
                    }
                    error.clear();
                    if (!effect.path.empty())
                        welcome_project_request = false;
                    remember_rom_file_states();
                } catch (const std::exception &exception) {
                    error = std::string("Project installed, but GUI setup failed: ") + exception.what();
                }
                break;
            case Kind::replacement_finished:
                if (project_operation_restarts_audio) start_audio();
                project_operation_restarts_audio = false;
                if (reloading_roms) {
                    reloading_roms = false;
                    if (resume_after_rom_reload && project_session.phase() != ProjectSession::Phase::unresolved)
                        pending_runtime_command = controller.request_resume();
                    resume_after_rom_reload = false;
                }
                break;
            case Kind::changed:
                snapshot = controller.snapshot();
                selected = effect.selected;
                invalidate_disassembly_cache();
                error.clear();
                break;
            case Kind::cancelled:
                error.clear();
                if (welcome_project_request) {
                    show_welcome = true;
                    welcome_project_request = false;
                }
                break;
            case Kind::error:
                error = effect.message;
                if (reloading_roms) resume_after_rom_reload = false;
                if (welcome_project_request) {
                    show_welcome = true;
                    welcome_project_request = false;
                }
                break;
            case Kind::closed:
                invalidate_project_views(true);
                snapshot = controller.snapshot();
                show_welcome = true;
                welcome_project_request = false;
                error.clear();
                break;
            case Kind::quit: quit_requested = true; break;
            }
        }
    }
}

bool App::combo_space(const char *label, srz80::Handle &current) {
    if (!snapshot)
        return false;
    if (!snapshot->inspection->spaces.contains(current))
        current = snapshot->inspection->spaces.empty() ? 0 : snapshot->inspection->spaces.begin()->first;
    bool changed = false;
    if (ImGui::BeginCombo(label,
                          current ? snapshot->inspection->spaces.at(current).name.c_str() : "No spaces")) {
        for (const auto &[id, s] : snapshot->inspection->spaces)
            if (ImGui::Selectable(s.name.c_str(), current == id)) {
                current = id;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}

void App::select_space(const char *label) {
    combo_space(label, space);
}

void App::layout() {
    auto dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
    if (!layout_initialized) {
        layout_initialized = true;
        if (!ImGui::DockBuilderGetNode(dock)->IsSplitNode()) {
            ImGui::DockBuilderRemoveNode(dock);
            ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
            ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
            auto workspace = dock;
            auto left = ImGui::DockBuilderSplitNode(workspace, ImGuiDir_Left, 0.19f, nullptr,
                                                    &workspace);
            auto rack_info =
                ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.19f, nullptr, &left);
            auto clocks =
                ImGui::DockBuilderSplitNode(left, ImGuiDir_Down, 0.63f, nullptr, &left);
            ImGui::DockBuilderDockWindow("Rack", left);
            ImGui::DockBuilderDockWindow("Project files", left);
            ImGui::DockBuilderDockWindow("Clock Control", clocks);
            ImGui::DockBuilderDockWindow("Rack Info", rack_info);

            auto bottom = ImGui::DockBuilderSplitNode(workspace, ImGuiDir_Down, 0.30f, nullptr,
                                                       &workspace);
            auto inspectors = ImGui::DockBuilderSplitNode(workspace, ImGuiDir_Right, 0.20f,
                                                           nullptr, &workspace);
            auto memory_buses = ImGui::DockBuilderSplitNode(inspectors, ImGuiDir_Down, 0.50f,
                                                            nullptr, &inspectors);
            auto breakpoint_panel = ImGui::DockBuilderSplitNode(bottom, ImGuiDir_Right, 0.20f,
                                                                 nullptr, &bottom);
            ImGui::DockBuilderDockWindow("Device inspector", inspectors);
            ImGui::DockBuilderDockWindow("Memory buses", memory_buses);
            ImGui::DockBuilderDockWindow("Memory inspector", workspace);
            ImGui::DockBuilderDockWindow("Bus monitor", bottom);
            ImGui::DockBuilderDockWindow("Log", bottom);
            ImGui::DockBuilderDockWindow("Breakpoints", breakpoint_panel);
            ImGui::DockBuilderDockWindow("Console", bottom);
            ImGui::DockBuilderDockWindow("Mixer", bottom);
            ImGui::DockBuilderDockWindow("Disassembly", workspace);
            ImGui::DockBuilderDockWindow("Video", workspace);
            ImGui::DockBuilderFinish(dock);
        }
    }
}

void App::restore_tool_config() {
    for (const auto &entry : tool_config_entries)
        if (entry.set)
            entry.set(entry.context, controller.config_get(entry.name, entry.default_value).c_str());
}

void App::load_config() {
    discard_settings_draft();
    controller.config_load();
    snapshot = controller.snapshot();
    ui_theme = controller.config_get("ui.theme", "Dark Red");
    if (!apply_ui_theme(ui_theme)) {
        ui_theme = "Dark Red";
        apply_ui_theme(ui_theme);
    }
    last_project_directory = controller.config_get("ui.last_project_directory", "");
    last_projects_directory = controller.config_get("ui.last_projects_directory", "");
    const auto auto_load = controller.config_get("ui.auto_load_last_project", "0");
    auto_load_last_project = auto_load == "1" || auto_load == "true";
    const auto reload_roms = controller.config_get("core.rom.reload_on_start", "0");
    reload_changed_roms = reload_roms == "1" || reload_roms == "true";
    try {
        disasm_align_from_zero_limit = std::stoull(
            controller.config_get("ui.disasm_align_from_zero_limit", "65536"));
    } catch (...) {
        disasm_align_from_zero_limit = 64 * 1024;
    }
    disasm_align_from_zero_limit =
        std::clamp<uint64_t>(disasm_align_from_zero_limit, 0, 1024 * 1024);
    recent_projects.clear();
    recent_project_names.clear();
    try {
        const auto recent = nlohmann::json::parse(
            controller.config_get("ui.recent_projects", "[]"));
        if (recent.is_array()) {
            for (const auto &entry : recent)
                if (entry.is_string() && !entry.get<std::string>().empty()) {
                    const auto path = std::filesystem::path(entry.get<std::string>());
                    recent_projects.push_back(path);
                    recent_project_names[path] = project_display_name(nlohmann::json::object(), path);
                } else if (entry.is_object() && entry.contains("path") && entry["path"].is_string()) {
                    const auto path = std::filesystem::path(entry["path"].get<std::string>());
                    if (path.empty()) continue;
                    recent_projects.push_back(path);
                    recent_project_names[path] = project_display_name(entry, path);
                }
        }
    } catch (...) {
        recent_projects.clear();
    }
    if (recent_projects.size() > 8)
        recent_projects.resize(8);
    {
        const bool was_loading = loading_config;
        loading_config = true;
        struct RestoreLoadingFlag {
            bool &flag;
            bool previous;
            ~RestoreLoadingFlag() { flag = previous; }
        } restore_loading{loading_config, was_loading};
        for (const auto &entry : app_config_entries)
            if (entry.set)
                entry.set(entry.context, controller.config_get(entry.name, entry.default_value).c_str());
    }
    restore_tool_config();
    show_rack = controller.config_get("ui.show_rack", "1") != "0";
    show_rack_info = controller.config_get("ui.show_rack_info", "1") != "0";
    show_memory_buses = controller.config_get("ui.show_memory_buses", "1") != "0";
    show_clocks = controller.config_get("ui.show_clocks", "1") != "0";
    show_inspector = controller.config_get("ui.show_inspector", "1") != "0";
    show_memory = controller.config_get("ui.show_memory", "1") != "0";
    show_breakpoints = controller.config_get("ui.show_breakpoints", "1") != "0";
    show_monitor = controller.config_get("ui.show_monitor", "1") != "0";
    show_console = controller.config_get("ui.show_console", "1") != "0";
    show_disassembly = controller.config_get("ui.show_disassembly", "1") != "0";
    show_video = controller.config_get("ui.show_video", "1") != "0";
    show_log = controller.config_get("ui.show_log", "1") != "0";
    show_mixer = controller.config_get("ui.show_mixer", "1") != "0";
    show_oscilloscope = controller.config_get("ui.show_oscilloscope", "0") != "0";
    show_project_files = controller.config_get("ui.show_project_files", "1") != "0";
    show_text_editor = controller.config_get("ui.show_text_editor", "0") != "0";
    editor_theme = controller.config_get("ui.editor.palette", "Dark");
    editor_show_whitespace = controller.config_get("ui.editor.show_whitespace", "0") == "1";
    editor_show_eol = controller.config_get("ui.editor.show_eol", "0") == "1";
    editor_highlight_cursor_line = controller.config_get("ui.editor.highlight_cursor_line", "0") == "1";
    editor_font_path = controller.config_get("ui.editor.font_path", "");
    try { editor_font_size = std::clamp(std::stoi(controller.config_get("ui.editor.font_size", "18")), 8, 48); }
    catch (...) { editor_font_size = 18; }
    try {
        memory_editor_columns = std::stoi(controller.config_get("memory_editor.columns", "16"));
    } catch (...) {
        memory_editor_columns = 16;
    }
    memory_editor_columns = std::clamp(memory_editor_columns, 4, 32);
    memory_editor_show_preview = controller.config_get("memory_editor.show_preview", "1") != "0";
    memory_editor_show_hexii = controller.config_get("memory_editor.show_hexii", "0") != "0";
    memory_editor_show_ascii = controller.config_get("memory_editor.show_ascii", "1") != "0";
    memory_editor_grey_zeroes = controller.config_get("memory_editor.grey_zeroes", "1") != "0";
    memory_editor_uppercase_hex = controller.config_get("memory_editor.uppercase_hex", "1") != "0";
    try { scope_timebase_ms = std::stoi(controller.config_get("scope.timebase_ms", "20")); }
    catch (...) { scope_timebase_ms = 20; }
    scope_timebase_ms = std::clamp(scope_timebase_ms, 2, 250);
    try { scope_gain = std::stof(controller.config_get("scope.gain", "1")); }
    catch (...) { scope_gain = 1.0f; }
    scope_gain = std::clamp(scope_gain, 0.1f, 8.0f);
    try { scope_trigger_level = std::stoi(controller.config_get("scope.trigger_level", "0")); }
    catch (...) { scope_trigger_level = 0; }
    scope_trigger_level = std::clamp(scope_trigger_level, -100, 100);
    scope_trigger_channel = controller.config_get("scope.trigger_channel", "0") == "1" ? 1 : 0;
    scope_trigger_rising = controller.config_get("scope.trigger_rising", "1") != "0";
    scope_show_left = controller.config_get("scope.show_left", "1") != "0";
    scope_show_right = controller.config_get("scope.show_right", "1") != "0";
    scope_xy_mode = controller.config_get("scope.xy_mode", "0") != "0";
    scope_stereo = controller.config_get("scope.stereo", "1") != "0";
    video_vsync = controller.config_get("video.vsync", "1") != "0";
    try {
        video_frame_rate_limit = std::stoi(controller.config_get("video.frame_rate_limit", "100"));
    } catch (...) {
        video_frame_rate_limit = 100;
    }
    video_frame_rate_limit = std::clamp(video_frame_rate_limit, 0, 1000);
    try {
        snapshot_normal_ms = std::stoi(controller.config_get("ui.snapshot_normal_ms", "33"));
    } catch (...) {
        snapshot_normal_ms = 33;
    }
    try {
        snapshot_high_ms = std::stoi(controller.config_get("ui.snapshot_high_ms", "66"));
    } catch (...) {
        snapshot_high_ms = 66;
    }
    try {
        snapshot_high_threshold_hz =
            std::stoi(controller.config_get("ui.snapshot_high_threshold_hz", "100"));
    } catch (...) {
        snapshot_high_threshold_hz = 100;
    }
    snapshot_normal_ms = std::clamp(snapshot_normal_ms, 16, 250);
    snapshot_high_ms = std::clamp(snapshot_high_ms, snapshot_normal_ms, 1000);
    snapshot_high_threshold_hz = std::clamp(snapshot_high_threshold_hz, 0, 50000000);
    controller.set_snapshot_cadence(static_cast<uint64_t>(snapshot_normal_ms),
                                    static_cast<uint64_t>(snapshot_high_ms),
                                    static_cast<uint32_t>(snapshot_high_threshold_hz));
    video_display_render_time = controller.config_get("video.display_render_time", "0") != "0";
    video_late_render_clear = controller.config_get("video.late_render_clear", "0") != "0";
    video_power_saving = controller.config_get("video.power_saving", "0") != "0";
    audio_enabled = controller.config_get("audio.enabled", "1") != "0";
    try {
        audio_queue_frames = std::stoi(controller.config_get("audio.queue_frames", "44100"));
    } catch (...) {
        audio_queue_frames = 44100;
    }
    audio_queue_frames = std::clamp(audio_queue_frames, 256, 441000);
    audio_backend_name = controller.config_get("audio.backend", "SDL");
    audio_driver = controller.config_get("audio.driver", "Automatic");
    audio_input_enabled = controller.config_get("audio.input_enabled", "1") == "1";
    audio_input_device = controller.config_get("audio.input_device", "<System default>");
    try { audio_input_channels = std::clamp(std::stoi(controller.config_get("audio.input_channels", "1")), 1, 8); } catch (...) { audio_input_channels = 1; }
    try { audio_input_volume = std::clamp(std::stoi(controller.config_get("audio.input_volume", "90")), 0, 200); } catch (...) { audio_input_volume = 90; }
    audio_device = controller.config_get("audio.device", "<System default>");
    try {
        audio_sample_rate = std::stoi(controller.config_get("audio.sample_rate", "44100"));
    } catch (...) {
        audio_sample_rate = 44100;
    }
    audio_sample_rate = std::clamp(audio_sample_rate, 8000, 384000);
    audio_resampling = controller.config_get("audio.resampling", "Linear");
    if (!audio_resampling_value(audio_resampling))
        audio_resampling = "Linear";
    try {
        audio_outputs = std::stoi(controller.config_get("audio.outputs", "2"));
        audio_buffer_size = std::stoi(controller.config_get("audio.buffer_size", "1024"));
    } catch (...) {
        audio_outputs = 2;
        audio_buffer_size = 1024;
    }
    audio_outputs = std::clamp(audio_outputs, 1, 2);
    audio_buffer_size = std::clamp(audio_buffer_size, 32, 4096);
    audio_low_latency = controller.config_get("audio.low_latency", "0") != "0";
    audio_force_mono = controller.config_get("audio.force_mono", "0") != "0";
    audio_software_clipping = controller.config_get("audio.software_clipping", "0") != "0";
    audio_dc_offset_correction = controller.config_get("audio.dc_offset_correction", "1") != "0";
    int master_volume = 100;
    try {
        master_volume = std::stoi(controller.config_get("audio.master_volume", "100"));
    } catch (...) {
        master_volume = 100;
    }
    controller.set_audio_master_volume(static_cast<uint32_t>(std::clamp(master_volume, 0, 100)));
    controller.set_audio_dc_offset_correction(audio_dc_offset_correction);
    controller.set_audio_software_clipping(audio_software_clipping);
    video_render_backend = controller.config_get("video.render_backend", "default");
    video_shader_path = controller.config_get("video.shader_path", "");
    try {
        ui_scale = std::stof(controller.config_get("ui.scale", "1"));
    } catch (...) {
        ui_scale = 1.0f;
    }
    ui_scale = std::clamp(ui_scale, 0.5f, 3.0f);
    ui_font_path = controller.config_get("ui.font_path", "");
    try {
        ui_font_size = std::stoi(controller.config_get("ui.font_size", "18"));
    } catch (...) {
        ui_font_size = 18;
    }
    ui_font_size = std::clamp(ui_font_size, 8, 96);
    ui_font_antialias = controller.config_get("ui.font_antialias", "1") != "0";
    try {
        ui_font_hinting = std::stoi(controller.config_get("ui.font_hinting", "0"));
    } catch (...) {
        ui_font_hinting = 0;
    }
    ui_font_hinting = std::clamp(ui_font_hinting, 0, 3);
    try {
        console_max_lines = std::stoi(controller.config_get("ui.console.max_lines", "1000"));
    } catch (...) {
        console_max_lines = 1000;
    }
    console_max_lines = std::clamp(console_max_lines, 1, 1000000);
}

void App::load_layout_from_config() {
    const auto encoded = controller.config_get("imgui");
    if (!encoded.empty()) {
        const auto text = base64_decode(encoded);
        if (!text.empty()) {
            ImGui::LoadIniSettingsFromMemory(text.c_str(), static_cast<size_t>(text.size()));
            layout_initialized = true;
        }
    }
}

void App::apply_video_settings() {
    if (renderer)
        SDL_SetRenderVSync(renderer, video_vsync ? 1 : 0);
}

void App::apply_ui_settings() {
    if (!apply_ui_theme(ui_theme)) {
        ui_theme = "Dark Red";
        apply_ui_theme(ui_theme);
    }
    auto &io = ImGui::GetIO();
    io.FontGlobalScale = ui_scale;
    const auto editor_font_file = preferred_font_path(editor_font_path, base);
    const auto ui_font_file = preferred_font_path(ui_font_path, base);

    // A configured path takes precedence. Otherwise, use the runtime Sarasa
    // asset when available. ImGui's default remains the final fallback.
    if (!editor_font && !editor_font_file.empty()) {
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 2;
        editor_font = io.Fonts->AddFontFromFileTTF(editor_font_file.string().c_str(),
                                                    static_cast<float>(editor_font_size), &cfg,
                                                    io.Fonts->GetGlyphRangesJapanese());
        if (!editor_font)
            controller.log_message("[ui] Could not load editor font from " + editor_font_file.string());
    }

    if (!ui_font_loaded && !ui_font_file.empty()) {
        ImFontConfig cfg;
        cfg.OversampleH = ui_font_antialias ? 2 : 1;
        cfg.OversampleV = ui_font_antialias ? 2 : 1;
        cfg.PixelSnapH = !ui_font_antialias;
        if (auto *font = io.Fonts->AddFontFromFileTTF(ui_font_file.string().c_str(), ui_font_size, &cfg)) {
            io.FontDefault = font;
            ui_font_loaded = true;
        }
    }

    if (!project_dirty_font && io.Fonts) {
        ImFontConfig cfg;
        cfg.OversampleH = ui_font_antialias ? 2 : 1;
        cfg.OversampleV = ui_font_antialias ? 2 : 1;
        cfg.PixelSnapH = !ui_font_antialias;
        cfg.RasterizerMultiply = 1.45f;
        if (!ui_font_file.empty())
            project_dirty_font = io.Fonts->AddFontFromFileTTF(ui_font_file.string().c_str(), ui_font_size, &cfg);
        else
            project_dirty_font = io.Fonts->AddFontDefault(&cfg);
    }

    // Font Awesome is part of the emulator UI rather than a tool-owned
    // resource.  Merge it into the active default font so every tool sees the
    // same glyphs through the shared Dear ImGui context and atlas.
    if (!font_awesome_loaded) {
        const auto path = font_awesome_path(base);
        if (!path.empty() && io.Fonts && !io.Fonts->Fonts.empty()) {
            ImFont *target = io.FontDefault ? io.FontDefault : io.Fonts->Fonts[0];
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.DstFont = target;
            cfg.PixelSnapH = true;
            cfg.OversampleH = 1;
            cfg.OversampleV = 1;
            const float size = target->LegacySize > 0.0f ? target->LegacySize : static_cast<float>(ui_font_size);
            if (io.Fonts->AddFontFromFileTTF(path.string().c_str(), size, &cfg, font_awesome_ranges))
                font_awesome_loaded = true;
            else
                controller.log_message("[ui] Could not load Font Awesome from " + path.string());
        }
    }
}

void App::start_audio() {
    AudioBackendSettings settings;
    settings.input_enabled = audio_input_enabled;
    settings.input_device = audio_input_device;
    settings.input_channels = static_cast<uint32_t>(audio_input_channels);
    settings.input_volume = static_cast<uint32_t>(audio_input_volume);
    settings.enabled = audio_enabled;
    settings.driver = audio_driver;
    settings.device = audio_device;
    settings.sample_rate = static_cast<uint32_t>(audio_sample_rate);
    settings.outputs = static_cast<uint32_t>(audio_outputs);
    settings.buffer_size = static_cast<uint32_t>(audio_buffer_size);
    settings.low_latency = audio_low_latency;
    settings.force_mono = audio_force_mono;
    // Stop the consumer before changing the producer's time base, then wait
    // for the simulation thread to reset both engine and transport queues.
    audio_backend.close();
    std::string rate_error;
    if (!controller.set_audio_sample_rate(settings.sample_rate, rate_error)) {
        controller.log_message("[audio] Disabled: " + rate_error);
        return;
    }
    const auto resampling = audio_resampling_value(audio_resampling);
    if (!resampling || !controller.set_audio_resampling(*resampling, rate_error)) {
        controller.log_message("[audio] Disabled: " + rate_error);
        return;
    }
    // The controller queue is intentionally kept independent from the device
    // buffer.  This is the bounded deterministic safety queue used by the
    // threaded pump below.
    controller.set_audio_queue_capacity(static_cast<uint64_t>(audio_queue_frames));
    audio_backend.open(controller, settings);
}

void App::stop_audio() { audio_backend.close(); }

std::string App::video_backend_info() const {
    if (!renderer)
        return "SDL Renderer (not initialized)";

    const char *backend_name = SDL_GetRendererName(renderer);

    std::string result = "current backend: ";
    result += backend_name ? backend_name : "unknown";

    if (backend_name && (std::string(backend_name).find("opengl") == 0 ||
                         std::string(backend_name).find("opengles") == 0)) {
        using GLGetString = const unsigned char *(*)(unsigned int);
        auto gl_get_string =
            reinterpret_cast<GLGetString>(SDL_GL_GetProcAddress("glGetString"));
        if (gl_get_string) {
            // GL_VENDOR, GL_RENDERER, GL_VERSION
            const auto add = [&](unsigned int name, const char *label) {
                const auto *text = gl_get_string(name);
                if (text)
                    result += std::string("\n") + label + ": " +
                              reinterpret_cast<const char *>(text);
            };
            add(0x1F00, "Vendor");
            add(0x1F01, "Device");
            add(0x1F02, "Version");
        }
    } else if (backend_name && std::string(backend_name) == "gpu") {
        auto *device = static_cast<SDL_GPUDevice *>(SDL_GetPointerProperty(
            SDL_GetRendererProperties(renderer), SDL_PROP_RENDERER_GPU_DEVICE_POINTER, nullptr));
        if (device) {
            const char *driver = SDL_GetGPUDeviceDriver(device);
            if (driver)
                result += std::string("\nDriver: ") + driver;
        }
    }

    const char *video_driver = SDL_GetCurrentVideoDriver();
    if (video_driver && *video_driver)
        result += std::string("\nVideo driver: ") + video_driver;
    return result;
}

void App::register_app_config() {
    if (!app_config_entries.empty())
        return;
    std::string current_category = "UI/General";
    auto register_one = [&](const char *name, const char *label, const char *description,
                            uint32_t type, const char *default_value, SrhConfigGet get,
                            SrhConfigSet set, const char *enum_labels = nullptr) {
        UiConfigRegistration entry;
        entry.category = current_category;
        entry.name = name;
        entry.label = label;
        entry.description = description ? description : "";
        entry.type = type;
        entry.enum_labels = enum_labels ? enum_labels : "";
        entry.default_value = default_value ? default_value : "";
        entry.context = this;
        entry.owner = 0;
        entry.get = get;
        entry.set = set;
        app_config_entries.push_back(std::move(entry));
        if (default_value && *default_value &&
            controller.config_get(name).empty())
            controller.config_set(name, default_value);
    };

    current_category = "Core/ROM";
    register_one("core.rom.reload_on_start", "Reload changed ROMs on start", "",
                 Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     if (!value || !capacity) return SRH_INVALID;
                     std::snprintf(value, capacity, "%d",
                                   static_cast<App *>(context)->reload_changed_roms ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value) return SRH_INVALID;
                     const std::string_view text(value);
                     if (text != "0" && text != "1" && text != "false" && text != "true")
                         return SRH_INVALID;
                     static_cast<App *>(context)->reload_changed_roms = text == "1" || text == "true";
                     return SRH_OK;
                 });

    current_category = "UI/Editor";
#define REGISTER_EDITOR_TOGGLE(key, label, field) \
    register_one(key, label, "", Srh_CONFIG_BOOL, "0", \
        [](void *context, char *value, uint32_t capacity) -> SrhStatus { \
            if (!value || capacity < 2) return SRH_INVALID; \
            value[0] = static_cast<App *>(context)->field ? '1' : '0'; \
            value[1] = 0; return SRH_OK; \
        }, \
        [](void *context, const char *value) -> SrhStatus { \
            if (!value) return SRH_INVALID; \
            const std::string_view text(value); \
            if (text != "0" && text != "1" && text != "false" && text != "true") return SRH_INVALID; \
            static_cast<App *>(context)->field = text == "1" || text == "true"; \
            return SRH_OK; \
        })
    REGISTER_EDITOR_TOGGLE("ui.editor.show_whitespace", "View white space", editor_show_whitespace);
    REGISTER_EDITOR_TOGGLE("ui.editor.show_eol", "View end of line", editor_show_eol);
    REGISTER_EDITOR_TOGGLE("ui.editor.highlight_cursor_line", "Highlight cursor line", editor_highlight_cursor_line);
#undef REGISTER_EDITOR_TOGGLE
    register_one("ui.editor.palette", "Color palette",
                 "Syntax colors for the editor's dark background",
                 Srh_CONFIG_ENUM, "Dark",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     if (!value || !capacity) return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", static_cast<App *>(context)->editor_theme.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value) return SRH_INVALID;
                     const std::string_view name(value);
                     if (name != "Dark" && name != "Ocean" && name != "Pastel" && name != "Amber")
                         return SRH_INVALID;
                     static_cast<App *>(context)->editor_theme = name;
                     return SRH_OK;
                 }, "Dark|Ocean|Pastel|Amber");
    register_one("ui.editor.font_size", "Text size",
                 "Editor text size, from 8 to 48. Ctrl+scroll and Ctrl++/- change it too",
                 Srh_CONFIG_INT, "18",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     if (!value || !capacity) return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", static_cast<App *>(context)->editor_font_size);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value) return SRH_INVALID;
                     try {
                         static_cast<App *>(context)->editor_font_size = std::clamp(std::stoi(value), 8, 48);
                         return SRH_OK;
                     } catch (...) { return SRH_INVALID; }
                 });
    register_one("ui.editor.font_path", "Font",
                 "TTF font used by the editor. Restart required. Set a CJK font if you see weird stuff in place your CJK chars",
                 Srh_CONFIG_PATH, "",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity) return SRH_INVALID;
                     const auto amount = std::min<size_t>(app->editor_font_path.size(), capacity - 1);
                     std::memcpy(value, app->editor_font_path.data(), amount);
                     value[amount] = 0;
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value) return SRH_INVALID;
                     static_cast<App *>(context)->editor_font_path = value;
                     return SRH_OK;
                 });
    current_category = "UI/General";

    register_one("ui.theme", "Theme", "Make it colorful~",
                 Srh_CONFIG_ENUM, "Dark Red",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->ui_theme.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !apply_ui_theme(value))
                         return SRH_INVALID;
                     app->ui_theme = value;
                     return SRH_OK;
                 }, ui_theme_names().c_str());

    video_render_backend_labels = "default";
    const int render_driver_count = SDL_GetNumRenderDrivers();
    for (int i = 0; i < render_driver_count; ++i) {
        const char *driver = SDL_GetRenderDriver(i);
        if (!driver || !*driver)
            continue;
        // Only expose render drivers that can actually be created on this
        // window.  The static SDL_GetNumRenderDrivers list often contains
        // backends that are compiled in but unusable on the current video
        // driver (e.g. OpenGL under dummy/software).
        if (!window)
            continue;
        SDL_Renderer *probe = SDL_CreateRenderer(window, driver);
        if (!probe)
            continue;
        SDL_DestroyRenderer(probe);
        const std::string driver_name = driver;
        if (video_render_backend_labels.find(driver_name) == std::string::npos)
            video_render_backend_labels += "|" + driver_name;
    }

    register_one("ui.scale", "UI scaling",
                 "Scales text and controls after you click Apply",
                 Srh_CONFIG_FLOAT, "1",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%g", app->ui_scale);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     float parsed = 1.0f;
                     try {
                         parsed = std::stof(value);
                     } catch (...) {
                         parsed = 1.0f;
                     }
                     app->ui_scale = std::clamp(parsed, 0.5f, 3.0f);
                     app->apply_ui_settings();
                     return SRH_OK;
                 });

    register_one("ui.auto_load_last_project", "Automatically load last project",
                 "Opens the last project when SRZ80 starts",
                 Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->auto_load_last_project ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->auto_load_last_project = value &&
                                                   (std::strcmp(value, "1") == 0 ||
                                                    std::strcmp(value, "true") == 0);
                     return SRH_OK;
                 });

    register_one("ui.font_path", "Font",
                 "TTF or OTF font used by the UI. Restart required",
                 Srh_CONFIG_STRING, "",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     const auto &path = app->ui_font_path;
                     const auto amount = std::min<size_t>(path.size(), capacity - 1);
                     std::memcpy(value, path.data(), amount);
                     value[amount] = 0;
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value)
                         return SRH_INVALID;
                     app->ui_font_path = value;
                     return SRH_OK;
                 });

    register_one("ui.font_size", "Font size",
                 "UI font size. Restart required",
                 Srh_CONFIG_INT, "18",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->ui_font_size);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 18;
                     try {
                         parsed = std::stoi(value);
                     } catch (...) {
                         parsed = 18;
                     }
                     app->ui_font_size = std::clamp(parsed, 8, 96);
                     return SRH_OK;
                 });

    register_one("ui.font_antialias", "Font anti-aliasing",
                 "Uses oversampling when loading a custom font",
                 Srh_CONFIG_BOOL, "1",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->ui_font_antialias ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->ui_font_antialias = std::strcmp(value, "1") == 0;
                     return SRH_OK;
                 });

    register_one("ui.font_hinting", "Font hinting",
                 "FreeType only: 0 none, 1 slight, 2 normal, 3 full",
                 Srh_CONFIG_INT, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->ui_font_hinting);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 0;
                     try {
                         parsed = std::stoi(value);
                     } catch (...) {
                         parsed = 0;
                     }
                     app->ui_font_hinting = std::clamp(parsed, 0, 3);
                     return SRH_OK;
                 });

    register_one("ui.snapshot_normal_ms", "Snapshot interval (normal)",
                 "Snapshot interval while all active clocks are below the high-clock threshold",
                 Srh_CONFIG_INT, "33",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->snapshot_normal_ms);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 33;
                     try { parsed = std::stoi(value); } catch (...) {}
                     app->snapshot_normal_ms = std::clamp(parsed, 16, 250);
                     app->snapshot_high_ms = std::max(app->snapshot_high_ms, app->snapshot_normal_ms);
                     app->controller.set_snapshot_cadence(
                         static_cast<uint64_t>(app->snapshot_normal_ms),
                         static_cast<uint64_t>(app->snapshot_high_ms),
                         static_cast<uint32_t>(app->snapshot_high_threshold_hz));
                     return SRH_OK;
                 });

    register_one("ui.snapshot_high_ms", "Snapshot interval (high clock)",
                 "Snapshot interval when any active clock is over the threshold",
                 Srh_CONFIG_INT, "66",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->snapshot_high_ms);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 66;
                     try { parsed = std::stoi(value); } catch (...) {}
                     app->snapshot_high_ms = std::clamp(parsed, app->snapshot_normal_ms, 1000);
                     app->controller.set_snapshot_cadence(
                         static_cast<uint64_t>(app->snapshot_normal_ms),
                         static_cast<uint64_t>(app->snapshot_high_ms),
                         static_cast<uint32_t>(app->snapshot_high_threshold_hz));
                     return SRH_OK;
                 });

    register_one("ui.snapshot_high_threshold_hz", "High-clock threshold (Hz)",
                 "Above this speed, SRZ80 uses the high-clock snapshot interval",
                 Srh_CONFIG_INT, "100",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->snapshot_high_threshold_hz);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 100;
                     try { parsed = std::stoi(value); } catch (...) {}
                     app->snapshot_high_threshold_hz = std::clamp(parsed, 0, 50000000);
                     app->controller.set_snapshot_cadence(
                         static_cast<uint64_t>(app->snapshot_normal_ms),
                         static_cast<uint64_t>(app->snapshot_high_ms),
                         static_cast<uint32_t>(app->snapshot_high_threshold_hz));
                     return SRH_OK;
                 });

    current_category = "UI/Debug";
    register_one("ui.disasm_align_from_zero_limit", "Disassembly align-from-zero limit",
                 "For smaller spaces, decode from 0 when instruction alignment is unknown.\nLarger spaces start where asked and show a warning. 0 turns this off",
                 Srh_CONFIG_INT, "65536",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%llu",
                                   static_cast<unsigned long long>(
                                       app->disasm_align_from_zero_limit));
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value)
                         return SRH_INVALID;
                     unsigned long long parsed = 0;
                     try {
                         parsed = std::stoull(value);
                     } catch (...) {
                         parsed = 0;
                     }
                     app->disasm_align_from_zero_limit =
                         std::clamp<uint64_t>(parsed, 0, 1024 * 1024);
                     app->invalidate_disassembly_cache();
                     return SRH_OK;
                 });

    current_category = "UI/Console";
    register_one("ui.console.max_lines", "Retained lines", "",
                 Srh_CONFIG_INT, "1000",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->console_max_lines);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value)
                         return SRH_INVALID;
                     try {
                         app->console_max_lines = std::clamp(std::stoi(value), 1, 1000000);
                     } catch (...) {
                         return SRH_INVALID;
                     }
                     return SRH_OK;
                 });

    current_category = "UI/Video";
    register_one("video.render_backend", "Render backend",
                 "SDL render driver. Restart required",
                 Srh_CONFIG_ENUM, "default",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->video_render_backend.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value)
                         return SRH_INVALID;
                     app->video_render_backend = value;
                     return SRH_OK;
                 },
                 video_render_backend_labels.c_str());

    register_one("video.render_backend_info", "Render backend information",
                 "Current renderer, GPU, and video driver. Read only",
                 Srh_CONFIG_STRING, nullptr,
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     const std::string info = app->video_backend_info();
                     const auto amount = std::min<size_t>(info.size(), capacity - 1);
                     std::memcpy(value, info.data(), amount);
                     value[amount] = 0;
                     return SRH_OK;
                  },
                  [](void *, const char *) -> SrhStatus { return SRH_INVALID; });

    register_one("video.scale", "Video scale",
                 "Draw video at 0.5x to 4x its native size",
                 Srh_CONFIG_FLOAT, "1",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%g", app->video_scale);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value)
                         return SRH_INVALID;
                     try {
                         auto *app = static_cast<App *>(context);
                         app->video_scale = std::clamp(std::stof(value), 0.5f, 4.0f);
                         return SRH_OK;
                     } catch (...) {
                         return SRH_INVALID;
                     }
                 });

    register_one("video.scale_filter", "Video scaling filter",
                 "How pixels are filtered when video is scaled",
                 Srh_CONFIG_ENUM, "Nearest integer",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->video_scale_filter.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value)
                         return SRH_INVALID;
                     const std::string_view filter(value);
                     if (filter != "Nearest integer" && filter != "Linear" && filter != "Cosine")
                         return SRH_INVALID;
                     static_cast<App *>(context)->video_scale_filter = filter;
                     return SRH_OK;
                 },
                 "Nearest integer|Linear|Cosine");

    register_one("video.shader_path", "Video shader",
                 "SDL GPU fragment shader description. Takes effect right away on the GPU renderer",
                 Srh_CONFIG_PATH, "",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity) return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->video_shader_path.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value) return SRH_INVALID;
                     static_cast<App *>(context)->video_shader_path = value;
                     return SRH_OK;
                 });

    register_one("video.vsync", "VSync", "Sync rendering to the display refresh",
                 Srh_CONFIG_BOOL, "1",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->video_vsync ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->video_vsync = std::strcmp(value, "1") == 0;
                     app->apply_video_settings();
                     return SRH_OK;
                 });

    register_one("video.frame_rate_limit", "Frame rate limit",
                 "Maximum GUI frame rate. 0 means no limit",
                 Srh_CONFIG_INT, "100",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->video_frame_rate_limit);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 0;
                     try {
                         parsed = std::stoi(value);
                     } catch (...) {
                         parsed = 0;
                     }
                     app->video_frame_rate_limit = std::clamp(parsed, 0, 1000);
                     return SRH_OK;
                 });

    register_one("video.display_render_time", "Display FPS",
                 "Shows the actual frame rate in the status bar", Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->video_display_render_time ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->video_display_render_time = std::strcmp(value, "1") == 0;
                     return SRH_OK;
                 });

    register_one("video.late_render_clear", "Late render clear",
                 "Clears the SDL renderer after presenting instead of before",
                 Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->video_late_render_clear ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->video_late_render_clear = std::strcmp(value, "1") == 0;
                     return SRH_OK;
                 });

    register_one("video.power_saving", "Power-saving mode",
                 "Lowers the frame rate while the emulator is paused",
                 Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->video_power_saving ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->video_power_saving = std::strcmp(value, "1") == 0;
                     return SRH_OK;
                 });

    current_category = "UI/Audio";
    audio_driver_labels = "Automatic";
    for (int i = 0; i < SDL_GetNumAudioDrivers(); ++i) {
        const char *driver = SDL_GetAudioDriver(i);
        if (driver && *driver && audio_driver_labels.find(driver) == std::string::npos)
            audio_driver_labels += "|" + std::string(driver);
    }
    audio_device_labels = "<System default>";
    int playback_count = 0;
    SDL_AudioDeviceID *playback_devices = SDL_GetAudioPlaybackDevices(&playback_count);
    for (int i = 0; playback_devices && i < playback_count; ++i) {
        const char *device = SDL_GetAudioDeviceName(playback_devices[i]);
        if (device && *device && audio_device_labels.find(device) == std::string::npos)
            audio_device_labels += "|" + std::string(device);
    }
    SDL_free(playback_devices);

    register_one("audio.backend", "Backend", "", Srh_CONFIG_ENUM, "SDL",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->audio_backend_name.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || std::strcmp(value, "SDL") != 0)
                         return SRH_INVALID;
                     app->audio_backend_name = value;
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 },
                 "SDL");
    register_one("audio.driver", "Driver",
                 "",
                 Srh_CONFIG_ENUM, "Automatic",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->audio_driver.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value)
                         return SRH_INVALID;
                     app->audio_driver = value;
                     app->audio_device = "<System default>";
                     app->audio_input_device = "<System default>";
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 },
                 audio_driver_labels.c_str());
    register_one("audio.device", "Device", "",
                 Srh_CONFIG_ENUM, "<System default>",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->audio_device.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value)
                         return SRH_INVALID;
                     app->audio_device = value;
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 },
                 audio_device_labels.c_str());
    register_one("audio.input_enabled", "Enable audio input", "", Srh_CONFIG_BOOL, "1",
        [](void *context, char *value, uint32_t capacity) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value || !capacity) return SRH_INVALID;
            std::snprintf(value, capacity, "%d", app->audio_input_enabled ? 1 : 0);
            return SRH_OK;
        },
        [](void *context, const char *value) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value) return SRH_INVALID;
            app->audio_input_enabled = std::strcmp(value, "1") == 0;
            restart_audio_if_needed(app);
            return SRH_OK;
        });
    register_one("audio.input_device", "Audio input device", "", Srh_CONFIG_ENUM, "<System default>",
        [](void *context, char *value, uint32_t capacity) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value || !capacity) return SRH_INVALID;
            std::snprintf(value, capacity, "%s", app->audio_input_device.c_str());
            return SRH_OK;
        },
        [](void *context, const char *value) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value) return SRH_INVALID;
            app->audio_input_device = value;
            restart_audio_if_needed(app);
            return SRH_OK;
        }, "<System default>");
    register_one("audio.input_channels", "Recording channels", "", Srh_CONFIG_INT, "1",
        [](void *context, char *value, uint32_t capacity) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value || !capacity) return SRH_INVALID;
            std::snprintf(value, capacity, "%d", app->audio_input_channels);
            return SRH_OK;
        },
        [](void *context, const char *value) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value) return SRH_INVALID;
            try { app->audio_input_channels = std::clamp(std::stoi(value), 1, 8); } catch (...) { return SRH_INVALID; }
            restart_audio_if_needed(app);
            return SRH_OK;
        });
    register_one("audio.input_volume", "Record volume", "", Srh_CONFIG_INT, "90",
        [](void *context, char *value, uint32_t capacity) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value || !capacity) return SRH_INVALID;
            std::snprintf(value, capacity, "%d", app->audio_input_volume);
            return SRH_OK;
        },
        [](void *context, const char *value) -> SrhStatus {
            auto *app = static_cast<App *>(context);
            if (!value) return SRH_INVALID;
            try { app->audio_input_volume = std::clamp(std::stoi(value), 0, 200); } catch (...) { return SRH_INVALID; }
            app->audio_backend.set_input_volume(app->audio_input_volume);
            return SRH_OK;
        });
    register_one("audio.sample_rate", "Sample rate", "Host mixing and SDL playback sample rate",
                 Srh_CONFIG_INT, "44100",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_sample_rate);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 44100;
                     try { parsed = std::stoi(value); } catch (...) {}
                     app->audio_sample_rate = std::clamp(parsed, 8000, 384000);
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 });
    register_one("audio.resampling", "Resampling interpolation",
                 "How card-native audio is converted to the host rate\nSome are pretty good, some are more colorful",
                 Srh_CONFIG_ENUM, "Linear",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%s", app->audio_resampling.c_str());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     if (!value)
                         return SRH_INVALID;
                     auto *app = static_cast<App *>(context);
                     const auto method = audio_resampling_value(value);
                     if (!method)
                         return SRH_INVALID;
                     std::string error;
                     if (!app->controller.set_audio_resampling(*method, error))
                         return SRH_ERROR;
                     app->audio_resampling = value;
                     return SRH_OK;
                 },
                 "None|Linear|Boxcar|Cosine|Sinc");
    register_one("audio.outputs", "Outputs", "Playback channels. Use 1 or 2", Srh_CONFIG_INT, "2",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_outputs);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 2;
                     try { parsed = std::stoi(value); } catch (...) {}
                     app->audio_outputs = std::clamp(parsed, 1, 2);
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 });
    register_one("audio.buffer_size", "Buffer size", "SDL device buffer size in sample frames", Srh_CONFIG_INT, "1024",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_buffer_size);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 1024;
                     try { parsed = std::stoi(value); } catch (...) {}
                     app->audio_buffer_size = std::clamp(parsed, 32, 4096);
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 });
    register_one("audio.low_latency", "Low-latency mode", "Asks SDL for a smaller device buffer", Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_low_latency ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->audio_low_latency = std::strcmp(value, "1") == 0;
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 });
    register_one("audio.force_mono", "Force mono audio", "Uses one output channel. The mixer stays stereo",
                 Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_force_mono ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->audio_force_mono = std::strcmp(value, "1") == 0;
                     restart_audio_if_needed(app);
                     return SRH_OK;
                 });
    register_one("audio.enabled", "Enable audio",
                 "", Srh_CONFIG_BOOL, "1",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_enabled ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->audio_enabled = std::strcmp(value, "1") == 0;
                     if (!app->loading_config && (app->audio_backend.opened() || app->audio_enabled))
                         app->start_audio();
                     return SRH_OK;
                 });
    register_one("audio.queue_frames", "Audio queue frames",
                 "Most mixed PCM frames SDL can keep queued", Srh_CONFIG_INT, "44100",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_queue_frames);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 44100;
                     try {
                         parsed = std::stoi(value);
                     } catch (...) {
                     }
                     app->audio_queue_frames = std::clamp(parsed, 256, 441000);
                     if (!app->loading_config && app->audio_backend.opened())
                         app->start_audio();
                     return SRH_OK;
                 });
    register_one("audio.software_clipping", "Software clipping",
                 "Runs a soft limiter before the final 16-bit clamp", Srh_CONFIG_BOOL, "0",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_software_clipping ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->audio_software_clipping = std::strcmp(value, "1") == 0;
                     app->controller.set_audio_software_clipping(app->audio_software_clipping);
                     return SRH_OK;
                 });
    register_one("audio.dc_offset_correction", "DC offset correction",
                 "Removes DC offset in the host mixer", Srh_CONFIG_BOOL, "1",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%d", app->audio_dc_offset_correction ? 1 : 0);
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     app->audio_dc_offset_correction = std::strcmp(value, "1") == 0;
                     app->controller.set_audio_dc_offset_correction(app->audio_dc_offset_correction);
                     return SRH_OK;
                 });
    register_one("audio.master_volume", "Master volume", "Final mixer gain as a percentage", Srh_CONFIG_INT, "100",
                 [](void *context, char *value, uint32_t capacity) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     if (!value || !capacity)
                         return SRH_INVALID;
                     std::snprintf(value, capacity, "%u",
                                   app->controller.current_audio_master_volume());
                     return SRH_OK;
                 },
                 [](void *context, const char *value) -> SrhStatus {
                     auto *app = static_cast<App *>(context);
                     int parsed = 100;
                     try { parsed = std::stoi(value); } catch (...) {}
                     app->controller.set_audio_master_volume(
                         static_cast<uint32_t>(std::clamp(parsed, 0, 100)));
                     return SRH_OK;
                 });
}

void App::post_config_if_changed(const std::string &key, const std::string &value) {
    auto it = posted_config.find(key);
    if (it != posted_config.end() && it->second == value)
        return;
    posted_config[key] = value;
    controller.post_config_set(key, value);
}

void App::sync_window_states() {
    post_config_if_changed("ui.show_rack", show_rack ? "1" : "0");
    post_config_if_changed("ui.show_rack_info", show_rack_info ? "1" : "0");
    post_config_if_changed("ui.show_memory_buses", show_memory_buses ? "1" : "0");
    post_config_if_changed("ui.show_clocks", show_clocks ? "1" : "0");
    post_config_if_changed("ui.show_inspector", show_inspector ? "1" : "0");
    post_config_if_changed("ui.show_memory", show_memory ? "1" : "0");
    post_config_if_changed("ui.show_breakpoints", show_breakpoints ? "1" : "0");
    post_config_if_changed("ui.show_monitor", show_monitor ? "1" : "0");
    post_config_if_changed("ui.show_console", show_console ? "1" : "0");
    post_config_if_changed("ui.show_disassembly", show_disassembly ? "1" : "0");
    post_config_if_changed("ui.show_video", show_video ? "1" : "0");
    post_config_if_changed("ui.show_log", show_log ? "1" : "0");
    post_config_if_changed("ui.show_mixer", show_mixer ? "1" : "0");
    post_config_if_changed("ui.show_oscilloscope", show_oscilloscope ? "1" : "0");
    post_config_if_changed("scope.timebase_ms", std::to_string(scope_timebase_ms));
    post_config_if_changed("scope.gain", std::to_string(scope_gain));
    post_config_if_changed("scope.trigger_level", std::to_string(scope_trigger_level));
    post_config_if_changed("scope.trigger_channel", std::to_string(scope_trigger_channel));
    post_config_if_changed("scope.trigger_rising", scope_trigger_rising ? "1" : "0");
    post_config_if_changed("scope.show_left", scope_show_left ? "1" : "0");
    post_config_if_changed("scope.show_right", scope_show_right ? "1" : "0");
    post_config_if_changed("scope.xy_mode", scope_xy_mode ? "1" : "0");
    post_config_if_changed("scope.stereo", scope_stereo ? "1" : "0");
    post_config_if_changed("memory_editor.columns", std::to_string(memory_editor_columns));
    post_config_if_changed("memory_editor.show_preview", memory_editor_show_preview ? "1" : "0");
    post_config_if_changed("memory_editor.show_hexii", memory_editor_show_hexii ? "1" : "0");
    post_config_if_changed("memory_editor.show_ascii", memory_editor_show_ascii ? "1" : "0");
    post_config_if_changed("memory_editor.grey_zeroes", memory_editor_grey_zeroes ? "1" : "0");
    post_config_if_changed("memory_editor.uppercase_hex", memory_editor_uppercase_hex ? "1" : "0");
    post_config_if_changed("video.vsync", video_vsync ? "1" : "0");
    post_config_if_changed("video.frame_rate_limit", std::to_string(video_frame_rate_limit));
    post_config_if_changed("ui.snapshot_normal_ms", std::to_string(snapshot_normal_ms));
    post_config_if_changed("ui.snapshot_high_ms", std::to_string(snapshot_high_ms));
    post_config_if_changed("ui.snapshot_high_threshold_hz",
                           std::to_string(snapshot_high_threshold_hz));
    post_config_if_changed("video.display_render_time", video_display_render_time ? "1" : "0");
    post_config_if_changed("video.late_render_clear", video_late_render_clear ? "1" : "0");
    post_config_if_changed("video.power_saving", video_power_saving ? "1" : "0");
    post_config_if_changed("audio.enabled", audio_enabled ? "1" : "0");
    post_config_if_changed("audio.queue_frames", std::to_string(audio_queue_frames));
    post_config_if_changed("audio.backend", audio_backend_name);
    post_config_if_changed("audio.driver", audio_driver);
    post_config_if_changed("audio.device", audio_device);
    post_config_if_changed("audio.input_enabled", audio_input_enabled ? "1" : "0");
    post_config_if_changed("audio.input_device", audio_input_device);
    post_config_if_changed("audio.input_channels", std::to_string(audio_input_channels));
    post_config_if_changed("audio.input_volume", std::to_string(audio_input_volume));
    post_config_if_changed("audio.sample_rate", std::to_string(audio_sample_rate));
    post_config_if_changed("audio.resampling", audio_resampling);
    post_config_if_changed("audio.outputs", std::to_string(audio_outputs));
    post_config_if_changed("audio.buffer_size", std::to_string(audio_buffer_size));
    post_config_if_changed("audio.low_latency", audio_low_latency ? "1" : "0");
    post_config_if_changed("audio.force_mono", audio_force_mono ? "1" : "0");
    post_config_if_changed("audio.software_clipping", audio_software_clipping ? "1" : "0");
    post_config_if_changed("audio.dc_offset_correction", audio_dc_offset_correction ? "1" : "0");
    post_config_if_changed("audio.master_volume",
                           std::to_string(controller.current_audio_master_volume()));
    post_config_if_changed("video.render_backend", video_render_backend);
    post_config_if_changed("video.scale", std::to_string(video_scale));
    post_config_if_changed("video.scale_filter", video_scale_filter);
    post_config_if_changed("video.shader_path", video_shader_path);
    post_config_if_changed("ui.scale", std::to_string(ui_scale));
    post_config_if_changed("ui.theme", ui_theme);
    post_config_if_changed("ui.font_path", ui_font_path);
    post_config_if_changed("ui.editor.font_path", editor_font_path);
    post_config_if_changed("ui.font_size", std::to_string(ui_font_size));
    post_config_if_changed("ui.font_antialias", ui_font_antialias ? "1" : "0");
    post_config_if_changed("ui.font_hinting", std::to_string(ui_font_hinting));
    for (const auto &tool : tools)
        post_config_if_changed("tool." + std::string(tool.api->id) + ".open", tool.open ? "1" : "0");
}

void App::save_config() {
    auto set = [&](const std::string &key, const std::string &value) {
        posted_config[key] = value;
        controller.config_set(key, value);
    };
    set("ui.last_project_directory", last_project_directory.string());
    set("ui.last_projects_directory", last_projects_directory.string());
    auto recent = nlohmann::json::array();
    for (const auto &path : recent_projects)
        recent.push_back({{"path", path.string()}, {"name", recent_project_names.at(path)}});
    set("ui.recent_projects", recent.dump());
    set("ui.show_rack", show_rack ? "1" : "0");
    set("ui.show_rack_info", show_rack_info ? "1" : "0");
    set("ui.show_memory_buses", show_memory_buses ? "1" : "0");
    set("ui.show_clocks", show_clocks ? "1" : "0");
    set("ui.show_inspector", show_inspector ? "1" : "0");
    set("ui.show_memory", show_memory ? "1" : "0");
    set("ui.show_breakpoints", show_breakpoints ? "1" : "0");
    set("ui.show_monitor", show_monitor ? "1" : "0");
    set("ui.show_console", show_console ? "1" : "0");
    set("ui.show_disassembly", show_disassembly ? "1" : "0");
    set("ui.show_video", show_video ? "1" : "0");
    set("ui.show_log", show_log ? "1" : "0");
    set("ui.show_mixer", show_mixer ? "1" : "0");
    set("ui.show_oscilloscope", show_oscilloscope ? "1" : "0");
    set("scope.timebase_ms", std::to_string(scope_timebase_ms));
    set("scope.gain", std::to_string(scope_gain));
    set("scope.trigger_level", std::to_string(scope_trigger_level));
    set("scope.trigger_channel", std::to_string(scope_trigger_channel));
    set("scope.trigger_rising", scope_trigger_rising ? "1" : "0");
    set("scope.show_left", scope_show_left ? "1" : "0");
    set("scope.show_right", scope_show_right ? "1" : "0");
    set("scope.xy_mode", scope_xy_mode ? "1" : "0");
    set("scope.stereo", scope_stereo ? "1" : "0");
    set("ui.show_project_files", show_project_files ? "1" : "0");
    set("ui.show_text_editor", show_text_editor ? "1" : "0");
    set("ui.editor.font_size", std::to_string(editor_font_size));
    set("ui.editor.show_whitespace", editor_show_whitespace ? "1" : "0");
    set("ui.editor.show_eol", editor_show_eol ? "1" : "0");
    set("ui.editor.highlight_cursor_line", editor_highlight_cursor_line ? "1" : "0");
    set("ui.editor.font_path", editor_font_path);
    set("memory_editor.columns", std::to_string(memory_editor_columns));
    set("memory_editor.show_preview", memory_editor_show_preview ? "1" : "0");
    set("memory_editor.show_hexii", memory_editor_show_hexii ? "1" : "0");
    set("memory_editor.show_ascii", memory_editor_show_ascii ? "1" : "0");
    set("memory_editor.grey_zeroes", memory_editor_grey_zeroes ? "1" : "0");
    set("memory_editor.uppercase_hex", memory_editor_uppercase_hex ? "1" : "0");
    set("video.vsync", video_vsync ? "1" : "0");
    set("video.frame_rate_limit", std::to_string(video_frame_rate_limit));
    set("ui.snapshot_normal_ms", std::to_string(snapshot_normal_ms));
    set("ui.snapshot_high_ms", std::to_string(snapshot_high_ms));
    set("ui.snapshot_high_threshold_hz", std::to_string(snapshot_high_threshold_hz));
    set("video.display_render_time", video_display_render_time ? "1" : "0");
    set("video.late_render_clear", video_late_render_clear ? "1" : "0");
    set("video.power_saving", video_power_saving ? "1" : "0");
    set("audio.enabled", audio_enabled ? "1" : "0");
    set("audio.queue_frames", std::to_string(audio_queue_frames));
    set("audio.backend", audio_backend_name);
    set("audio.driver", audio_driver);
    set("audio.device", audio_device);
    set("audio.input_enabled", audio_input_enabled ? "1" : "0");
    set("audio.input_device", audio_input_device);
    set("audio.input_channels", std::to_string(audio_input_channels));
    set("audio.input_volume", std::to_string(audio_input_volume));
    set("audio.sample_rate", std::to_string(audio_sample_rate));
    set("audio.resampling", audio_resampling);
    set("audio.outputs", std::to_string(audio_outputs));
    set("audio.buffer_size", std::to_string(audio_buffer_size));
    set("audio.low_latency", audio_low_latency ? "1" : "0");
    set("audio.force_mono", audio_force_mono ? "1" : "0");
    set("audio.software_clipping", audio_software_clipping ? "1" : "0");
    set("audio.dc_offset_correction", audio_dc_offset_correction ? "1" : "0");
    set("audio.master_volume", std::to_string(controller.current_audio_master_volume()));
    set("video.render_backend", video_render_backend);
    set("video.scale", std::to_string(video_scale));
    set("video.scale_filter", video_scale_filter);
    set("video.shader_path", video_shader_path);
    set("ui.scale", std::to_string(ui_scale));
    set("ui.theme", ui_theme);
    set("ui.font_path", ui_font_path);
    set("ui.font_size", std::to_string(ui_font_size));
    set("ui.font_antialias", ui_font_antialias ? "1" : "0");
    set("ui.font_hinting", std::to_string(ui_font_hinting));
    controller.config_save();
}

void App::save_layout_to_config() {
    size_t size = 0;
    const char *memory = ImGui::SaveIniSettingsToMemory(&size);
    if (memory && size) {
        const auto encoded = base64_encode(std::string(memory, size));
        posted_config["imgui"] = encoded;
        controller.config_set("imgui", encoded);
    }
}

void App::invalidate_disassembly_cache() {
    // The disassembly window is decoded lazily around the current view.  Clear
    // it so the next frame re-decodes from the current anchor (PC when
    // following, otherwise the requested address) after a memory mutation.
    disasm_cache.clear();
}

void App::sync_disassembly_memory_revision() {
    // Memory writes are batched by the controller (one revision per
    // WriteMemory command), so clearing once here covers a multi-byte write
    // such as an assembler ROM load without invalidating per byte.
    const uint64_t revision = controller.memory_revision();
    if (revision == disasm_memory_revision)
        return;
    disasm_memory_revision = revision;
    disasm_cache.clear();
}

void App::draw_modal_windows() {
    // Project lifecycle dialogs must own modal focus. In particular, a
    // window-close request may arrive while a card dialog is open. Submitting
    // both popup windows in the same frame leaves the older popup blocking
    // input to the unsaved-project prompt. Skipping the lower-priority dialog
    // lets ImGui replace it in the popup stack with the project prompt.
    draw_project_modals();
    if (project_session.transitioning() || project_session.phase() == ProjectSession::Phase::awaiting_conflict ||
        (project_session.phase() == ProjectSession::Phase::awaiting_unsaved)) {
        return;
    }

    if (show_add_card)
        add_card_modal();
    if (show_card_info)
        card_info_modal();
}

void App::draw(bool draw_when_hidden) {
    audio_input_monitor_visible = false;
    if (editor_font_save_at != 0 && ImGui::GetTime() >= editor_font_save_at) {
        controller.post_config_save();
        editor_font_save_at = 0;
    }
    process_file_dialog_results();
    poll_layout_file_operation();
    advance_layout_import();
    poll_project_operation();
    poll_card_types();
    if (window) {
        const auto name = !project_session.loaded()
                              ? std::string("Welcome")
                          : project_session.name();
        SDL_SetWindowTitle(window, ("SRZ80 - " + name +
                                    ((project_session.dirty() || project_session.workspace().dirty()) ? " *" : "")).c_str());
    }
    // Lifecycle dialogs keep focus while the replacement rack is loading.
    // Tool APIs can still require synchronous replies, so defer their drawing.
    if (project_session.transitioning()) {
        draw_modal_windows();
        sync_window_states();
        return;
    }
    snapshot = controller.snapshot();
    const bool visible = !window || !(SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED));
    const bool tools_visible = std::any_of(tools.begin(), tools.end(), [](const auto &tool) { return tool.open; });
    controller.set_ui_visible(visible);
    controller.set_video_visible(visible && show_video);
    controller.set_inspection_interest(
        ((show_inspector || show_disassembly || tools_visible) ? SimulationController::InspectProperties : 0u) |
        (show_monitor ? SimulationController::InspectTrace : 0u) |
        (show_log ? SimulationController::InspectLogs : 0u));
    controller.set_inspection_visible(visible && (show_rack || show_inspector || show_memory ||
        show_breakpoints || show_monitor || show_console || show_disassembly || show_log ||
        show_settings || show_rack_info || show_video || tools_visible));
    for (auto &failure : controller.take_errors())
        error = "Command " + std::to_string(failure.seq) + ": " + failure.message;
    poll_clipboard();
    tick_tools(visible);
    if (!visible && !draw_when_hidden)
        return;
    controller.set_trace_capture(show_monitor && record_trace,
                                 filter_op ? static_cast<uint32_t>(filter_op) : SRH_READ | SRH_WRITE);
    auto &io = ImGui::GetIO();
    if (!host_input.active() && io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false))
        edit_undo();
    if (!host_input.active() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
        edit_redo();
    menu();
    layout();
    if (show_welcome) {
        welcome();
        license();
        about();
        draw_modal_windows();
        sync_window_states();
        return;
    }
    license();
    about();
    if (show_settings)
        settings();
    if (show_rack)
        rack();
    if (show_rack_info)
        rack_info();
    if (show_memory_buses)
        memory_buses();
    if (show_clocks)
        clocks();
    if (show_inspector)
        inspector();
    if (show_memory)
        memory();
    if (show_breakpoints)
        breakpoints();
    if (show_monitor)
        monitor();
    if (show_console)
        console();
    if (show_disassembly)
        disassembly();
    if (show_video)
        video();
    if (show_mixer)
        mixer();
    if (show_oscilloscope)
        oscilloscope();
    if (show_log)
        log();
    if (show_project_files)
        project_files();
    if (show_text_editor)
        text_editor();
    draw_tools();
    project_settings();

    // File shortcuts belong to the application shell, except when a focused
    // GUI tool owns the keyboard (for example, the Z80 assembler). Tool names
    // are used as the window-name prefix. The suffix may contain an ImGui ID.
    bool tool_focused = false;
    if (auto *nav = ImGui::GetCurrentContext()->NavWindow) {
        const std::string_view window_name(nav->Name);
        for (const auto &tool : tools) {
            if (tool.open && tool.api && tool.api->name &&
                srz80::sdk::has_field(tool.api, &SrhToolPlugin::flags) &&
                (tool.api->flags & Srh_TOOL_CLAIM_FILE_SHORTCUTS) &&
                window_name.starts_with(tool.api->name)) {
                tool_focused = true;
                break;
            }
        }
    }
    if (!project_session.transitioning() && project_session.phase() != ProjectSession::Phase::awaiting_conflict && !(project_session.phase() == ProjectSession::Phase::awaiting_unsaved) && !tool_focused && !host_input.active() && io.KeyCtrl && !io.KeyAlt) {
        if (io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
            save_project_as_dialog();
        else if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_S, false))
            project_session.request(ProjectSession::ActionKind::save);
        else if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_O, false))
            open_project_dialog();
        else if (!io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_N, false))
            project_session.request(ProjectSession::ActionKind::new_project);
    }
    draw_modal_windows();
    sync_window_states();
}

} // namespace srz80::ui
