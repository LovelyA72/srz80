#include "../gui.hpp"
#include "../theme.hpp"
#include <fstream>
#include <imgui_internal.h>
#include <iterator>
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace srz80::ui {
namespace {

std::string committed_entry_value(App *app, const SettingsEntry &entry) {
    if (entry.simulation_owned)
        return app->controller.config_entry_get(entry.name, entry.default_value);
    std::array<char, 1024> raw{};
    if (entry.get && entry.get(entry.context, raw.data(), static_cast<uint32_t>(raw.size())) == SRH_OK)
        return raw.data();
    return app->controller.config_get(entry.name, entry.default_value);
}

bool set_entry_value(App *app, const SettingsEntry &entry, const std::string &value) {
    if ((!entry.set && !entry.simulation_owned) || !app->settings_draft_active)
        return false;
    auto it = app->settings_draft.find(entry.name);
    if (it != app->settings_draft.end() && it->second == value)
        return false;
    app->settings_draft[entry.name] = value;
    return true;
}

std::string entry_value(App *app, const SettingsEntry &entry) {
    if (app->settings_draft_active) {
        const auto it = app->settings_draft.find(entry.name);
        if (it != app->settings_draft.end())
            return it->second;
    }
    return committed_entry_value(app, entry);
}

bool matches(const std::string &text, const char *search) {
    if (!search || !*search)
        return true;
    std::string needle = search;
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::string haystack = text;
    std::transform(haystack.begin(), haystack.end(), haystack.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return haystack.find(needle) != std::string::npos;
}

bool has_tooltip(const std::string &text) {
    return std::any_of(text.begin(), text.end(),
                       [](unsigned char c) { return !std::isspace(c); });
}

bool render_entry(App *app, const SettingsEntry &entry, const char *search) {
    bool changed = false;
    const std::string current = entry_value(app, entry);
    const std::string label = entry.label.empty() ? entry.name : entry.label;
    if (!matches(entry.label, search) && !matches(entry.name, search) &&
        !matches(entry.description, search) && !matches(entry.category, search))
        return false;

    switch (entry.type) {
    case Srh_CONFIG_BOOL: {
        bool value = current == "1" || current == "true";
        if (ImGui::Checkbox(label.c_str(), &value)) {
            changed = set_entry_value(app, entry, value ? "1" : "0");
        }
        break;
    }
    case Srh_CONFIG_INT: {
        int value = 0;
        try {
            if (!current.empty())
                value = std::stoi(current);
        } catch (...) {
            value = 0;
        }
        const bool input_volume = entry.name == "audio.input_volume";
        const bool percentage = entry.name == "audio.master_volume" || input_volume;
        const int maximum = input_volume ? 200 : 100;
        if (percentage)
            value = std::clamp(value, 0, maximum);
        const bool edited = percentage ? ImGui::SliderInt(label.c_str(), &value, 0, maximum, "%d%%")
                                       : ImGui::InputInt(label.c_str(), &value);
        if (edited) {
            if (entry.name == "audio.input_channels") value = std::clamp(value, 1, 8);
            changed = set_entry_value(app, entry, std::to_string(value));
            if (input_volume) app->preview_input_volume(value);
        }
        if (input_volume) {
            app->monitor_audio_input();
            const float peak = app->audio_input_level();
            ImGui::PushStyleColor(ImGuiCol_PlotHistogram, peak >= 1.0f
                ? ImVec4(1.0f, 0.0f, 0.0f, 1.0f) : mixer_meter_color());
            ImGui::ProgressBar(std::clamp(peak, 0.0f, 1.0f), ImVec2(ImGui::CalcItemWidth(), 10), "");
            ImGui::PopStyleColor();
            if (!app->audio_input_error().empty()) {
                ImGui::TextWrapped("%s", app->audio_input_error().c_str());
                if (ImGui::Button("Retry audio input")) app->retry_audio_input();
            }
            else if (!app->audio_input_enabled || !app->audio_enabled)
                ImGui::TextDisabled("Audio input is disabled.");
        }
        break;
    }
    case Srh_CONFIG_FLOAT: {
        float value = 0.0f;
        try {
            if (!current.empty())
                value = std::stof(current);
        } catch (...) {
            value = 0.0f;
        }
        const bool video_scale = entry.name == "video.scale";
        if (video_scale)
            value = std::clamp(value, 0.5f, 4.0f);
        const bool edited = video_scale
                                ? ImGui::SliderFloat(label.c_str(), &value, 0.5f, 4.0f, "%.1fx")
                                : ImGui::InputFloat(label.c_str(), &value, 0.0f, 0.0f, "%g");
        if (edited) {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%g", value);
            changed = set_entry_value(app, entry, buffer);
        }
        break;
    }
    case Srh_CONFIG_ENUM: {
        std::vector<std::string> labels;
        std::string item;
        std::istringstream stream(entry.enum_labels);
        while (std::getline(stream, item, '|'))
            labels.push_back(item);
        if (entry.name == "audio.input_device") {
            labels = {"<System default>"};
            int count = 0;
            auto *devices = SDL_GetAudioRecordingDevices(&count);
            for (int i = 0; devices && i < count; ++i) {
                const char *name = SDL_GetAudioDeviceName(devices[i]);
                if (name) labels.emplace_back(name);
            }
            SDL_free(devices);
            if (std::find(labels.begin(), labels.end(), current) == labels.end())
                labels.push_back(current);
        }
        if (labels.empty())
            labels.push_back(entry.default_value);
        std::vector<const char *> label_ptrs;
        label_ptrs.reserve(labels.size());
        for (const auto &item : labels)
            label_ptrs.push_back(item.c_str());
        int selected = 0;
        for (size_t i = 0; i < labels.size(); ++i)
            if (labels[i] == current) {
                selected = static_cast<int>(i);
                break;
            }
        if (ImGui::Combo(label.c_str(), &selected, label_ptrs.data(),
                         static_cast<int>(label_ptrs.size()))) {
            changed = set_entry_value(app, entry, labels[static_cast<size_t>(selected)]);
        }
        break;
    }
    case Srh_CONFIG_STRING:
    case Srh_CONFIG_PATH: {
        if (entry.name == "video.render_backend_info") {
            ImGui::TextWrapped("%s", current.c_str());
            break;
        }
        std::array<char, 1024> buffer{};
        std::snprintf(buffer.data(), buffer.size(), "%s", current.c_str());
        bool edited = false;
        if (entry.name == "video.shader_path") {
            ImGui::PushID(entry.name.c_str());
            edited = ImGui::InputText("##video-shader-path", buffer.data(), buffer.size());
            ImGui::SameLine();
            if (ImGui::Button("Browse...")) app->select_video_shader_file_dialog();
            ImGui::SameLine();
            ImGui::BeginDisabled(app->video_shader_path.empty());
            if (ImGui::Button("Reload")) {
                app->video_shader_loaded_path.clear();
                app->video_shader_error.clear();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextUnformatted(label.c_str());
            ImGui::PopID();
        } else {
            edited = ImGui::InputText(label.c_str(), buffer.data(), buffer.size());
        }
        if (edited) {
            changed = set_entry_value(app, entry, buffer.data());
        }
        break;
    }
    default:
        break;
    }

    if (has_tooltip(entry.description) && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", entry.description.c_str());
    return changed;
}

} // namespace

void App::initialize_settings_draft(const std::vector<SettingsEntry> &entries) {
    if (settings_draft_active)
        return;
    settings_draft.clear();
    settings_original.clear();
    for (const auto &entry : entries) {
        const auto value = committed_entry_value(this, entry);
        settings_draft[entry.name] = value;
        settings_original[entry.name] = value;
    }
    settings_draft_active = true;
    settings_dirty = false;
}

bool App::apply_settings_draft(const std::vector<SettingsEntry> &entries) {
    if (!settings_draft_active)
        return true;

    bool applied = true;
    for (const auto &entry : entries) {
        const auto draft = settings_draft.find(entry.name);
        const auto original = settings_original.find(entry.name);
        if (draft == settings_draft.end() ||
            (original != settings_original.end() && draft->second == original->second))
            continue;
        const auto status = entry.simulation_owned
                                ? controller.config_entry_set(entry.name, draft->second)
                                : (entry.set ? entry.set(entry.context, draft->second.c_str())
                                             : SRH_INVALID);
        if (status != SRH_OK) {
            applied = false;
            continue;
        }
        if (!entry.simulation_owned)
            controller.config_set(entry.name, draft->second);
    }
    if (!applied)
        return false;

    save_config();
    discard_settings_draft();
    return true;
}

void App::discard_settings_draft() {
    audio_backend.set_input_volume(audio_input_volume);
    settings_draft.clear();
    settings_original.clear();
    settings_draft_active = false;
    settings_dirty = false;
}

void App::begin_layout_import(std::filesystem::path path) {
    layout_file_operation = std::async(std::launch::async, [path = std::move(path)] {
        LayoutFileResult result;
        result.action = FileDialogAction::import_layout;
        try {
            std::ifstream input(path, std::ios::binary);
            if (!input)
                throw std::runtime_error("Could not open " + path.string());
            input.seekg(0, std::ios::end);
            const auto size = input.tellg();
            if (size <= 0 || size > 16 * 1024 * 1024)
                throw std::runtime_error("Layout file must be between 1 byte and 16 MiB");
            result.data.resize(static_cast<size_t>(size));
            input.seekg(0, std::ios::beg);
            if (!input.read(result.data.data(), static_cast<std::streamsize>(result.data.size())))
                throw std::runtime_error("Could not read " + path.string());
        } catch (const std::exception &exception) {
            result.error = exception.what();
        }
        return result;
    });
}

void App::begin_layout_export(std::filesystem::path path) {
    size_t size = 0;
    const char *memory = ImGui::SaveIniSettingsToMemory(&size);
    std::string data = memory && size ? std::string(memory, size) : std::string();
    layout_file_operation = std::async(
        std::launch::async, [path = std::move(path), data = std::move(data)] {
            LayoutFileResult result;
            result.action = FileDialogAction::export_layout;
            try {
                std::ofstream output(path, std::ios::binary | std::ios::trunc);
                if (!output || !output.write(data.data(), static_cast<std::streamsize>(data.size())))
                    throw std::runtime_error("Could not write " + path.string());
            } catch (const std::exception &exception) {
                result.error = exception.what();
            }
            return result;
        });
}

void App::poll_layout_file_operation(bool finish) {
    if (!layout_file_operation.valid())
        return;
    if (!finish && layout_file_operation.wait_for(std::chrono::seconds(0)) !=
                       std::future_status::ready)
        return;
    auto result = layout_file_operation.get();
    if (!result.error.empty()) {
        if (finish)
            controller.log_message("[ui] Layout operation failed: " + result.error);
        else
            error = "Layout operation failed: " + result.error;
        return;
    }
    if (result.action != FileDialogAction::import_layout)
        return;
    if (finish) {
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(result.data.data(), result.data.size());
        layout_initialized = true;
    } else {
        pending_layout_import = std::move(result.data);
        layout_import_step = 1;
    }
}

void App::advance_layout_import() {
    if (!layout_import_step)
        return;
    if (layout_import_step == 1) {
        layout_windows_to_reopen.clear();
        const auto close = [&](bool &open) {
            if (open) {
                open = false;
                layout_windows_to_reopen.push_back(&open);
            }
        };
        close(show_rack); close(show_clocks); close(show_inspector); close(show_memory);
        close(show_breakpoints); close(show_monitor); close(show_console);
        close(show_disassembly); close(show_video); close(show_log); close(show_mixer);
        close(show_project_files); close(show_text_editor); close(show_license); close(show_about);
        close(show_settings); close(show_add_card); close(show_card_info);
        for (auto &tool : tools)
            close(tool.open);
    } else if (layout_import_step == 3) {
        ImGui::ClearIniSettings();
        ImGui::LoadIniSettingsFromMemory(pending_layout_import.data(),
                                         pending_layout_import.size());
        layout_initialized = true;
    } else if (layout_import_step == 4) {
        for (bool *open : layout_windows_to_reopen)
            *open = true;
        layout_windows_to_reopen.clear();
    } else if (layout_import_step == 5) {
        pending_layout_import.clear();
        layout_import_step = 0;
        return;
    }
    ++layout_import_step;
}

void App::reset_layout() {
    ImGui::ClearIniSettings();
    show_rack = true;
    show_project_files = true;
    show_clocks = true;
    show_rack_info = true;
    show_inspector = true;
    show_memory_buses = true;
    show_memory = true;
    show_disassembly = true;
    show_video = true;
    show_monitor = true;
    show_console = true;
    show_log = true;
    show_mixer = true;
    show_breakpoints = true;
    show_text_editor = false;
    show_oscilloscope = false;
    layout_initialized = false;
}

void App::settings() {
    if (!show_settings)
        return;

    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Settings", &show_settings, ImGuiWindowFlags_NoDocking)) {
        if (!show_settings)
            discard_settings_draft();
        ImGui::End();
        return;
    }

    std::vector<SettingsEntry> entries;
    const auto snapshot = controller.snapshot();
    entries.reserve(app_config_entries.size() + snapshot->inspection->config_entries.size() +
                    tool_config_entries.size());
    for (const auto &entry : app_config_entries)
        entries.emplace_back(entry);
    for (const auto &entry : snapshot->inspection->config_entries)
        entries.emplace_back(entry, true);
    for (const auto &entry : tool_config_entries)
        entries.emplace_back(entry);
    initialize_settings_draft(entries);

    static char search[128] = {};
    static std::string selected_category = "UI/General";

    std::map<std::string, std::vector<std::string>> children;
    std::vector<std::string> top_levels;
    std::set<std::string> leaves;

    auto add_top = [&](const std::string &name) {
        if (std::find(top_levels.begin(), top_levels.end(), name) == top_levels.end())
            top_levels.push_back(name);
    };
    auto add_leaf = [&](const std::string &name) { leaves.insert(name); };

    for (const char *group : {"UI", "Core", "Cards", "Tools", "Plugins"})
        add_top(group);

    children["UI"].push_back("Layout");
    add_leaf("UI/Layout");

    for (const auto &entry : entries) {
        const auto slash = entry.category.find('/');
        const std::string top = slash == std::string::npos ? entry.category : entry.category.substr(0, slash);
        add_top(top);
        if (slash == std::string::npos) {
            add_leaf(entry.category);
        } else {
            const std::string sub = entry.category.substr(slash + 1);
            auto &list = children[top];
            if (std::find(list.begin(), list.end(), sub) == list.end())
                list.push_back(sub);
            add_leaf(entry.category);
        }
    }

    // Empty groups are leaves too.
    for (const auto &top : top_levels)
        if (children.find(top) == children.end())
            add_leaf(top);

    if (!leaves.contains(selected_category)) {
        if (leaves.empty())
            selected_category.clear();
        else
            selected_category = *leaves.begin();
    }

    ImGui::InputTextWithHint("##settings-search", "Search...", search, sizeof(search));

    ImGui::BeginChild("settings-sidebar", ImVec2(220.0f, -ImGui::GetFrameHeightWithSpacing() - 8.0f));
    for (const auto &top : top_levels) {
        const auto it = children.find(top);
        if (it == children.end()) {
            if (ImGui::Selectable(top.c_str(), selected_category == top))
                selected_category = top;
        } else {
            bool open = ImGui::TreeNodeEx(top.c_str(),
                                          ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
            if (open) {
                ImGui::Indent();
                for (const auto &sub : it->second) {
                    const std::string path = top + "/" + sub;
                    if (ImGui::Selectable(sub.c_str(), selected_category == path))
                        selected_category = path;
                }
                ImGui::Unindent();
                ImGui::TreePop();
            }
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("settings-content", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 8.0f));
    if (!selected_category.empty()) {
        if (selected_category == "Plugins") {
            if (project_text_formats.empty()) {
                ImGui::TextDisabled("No text formats");
            } else {
                for (const auto &handler : project_text_formats) {
                    std::string label = handler->label + " (*." + handler->extension + ")##" +
                                        handler->plugin_id + "." + handler->extension;
                    if (ImGui::Checkbox(label.c_str(), &handler->enabled)) {
                        controller.config_set("plugin.text_format." + handler->plugin_id + "." +
                                                  handler->extension,
                                              handler->enabled ? "1" : "0");
                        save_config();
                    }
                    if (!handler->plugin_id.empty() && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Comes from %s", handler->plugin_id.c_str());
                }
            }
        }
        bool any_entry = false;
        if (selected_category == "UI/Layout") {
            any_entry = true;
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted("Workspace layout:");
            ImGui::SameLine();
            const bool layout_busy = layout_dialog_pending || layout_file_operation.valid() ||
                                     layout_import_step != 0;
            ImGui::BeginDisabled(layout_busy);
            if (ImGui::Button("Import##workspace-layout"))
                import_layout_dialog();
            ImGui::SameLine();
            if (ImGui::Button("Export##workspace-layout"))
                export_layout_dialog();
            ImGui::SameLine();
            if (ImGui::Button("Reset##workspace-layout"))
                ImGui::OpenPopup("Reset workspace layout?");
            ImGui::EndDisabled();
            if (ImGui::BeginPopupModal("Reset workspace layout?", nullptr,
                                       ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::TextUnformatted("Reset the workspace layout to its default?");
                if (ImGui::Button("Reset")) {
                    reset_layout();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel"))
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        for (const auto &entry : entries) {
            if (entry.category != selected_category)
                continue;
            any_entry = true;
            if (render_entry(this, entry, search))
                settings_dirty = true;
            ImGui::Spacing();
        }
        if (!any_entry)
            ImGui::TextDisabled("No settings in this group.");
    }
    ImGui::EndChild();

    ImGui::Separator();
    bool close_settings = false;
    if (ImGui::Button("OK")) {
        if (apply_settings_draft(entries)) {
            show_settings = false;
            close_settings = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        discard_settings_draft();
        show_settings = false;
        close_settings = true;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!settings_dirty);
    if (ImGui::Button("Apply")) {
        (void)apply_settings_draft(entries);
    }
    ImGui::EndDisabled();

    ImGui::End();
    if (!show_settings && !close_settings)
        discard_settings_draft();
}

} // namespace srz80::ui
