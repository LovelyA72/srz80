#include "../gui.hpp"
#include <misc/cpp/imgui_stdlib.h>
#include <algorithm>

namespace srz80::ui {

void App::open_project_settings() {
    if (!project_session.loaded() || project_session.busy()) return;
    project_settings_draft = project_session.rack_info();
    project_input_draft = project_session.input_routes();
    host_input.release(window, controller);
    project_settings_generation = project_session.generation();
    project_settings_filter.Clear();
    project_settings_category = 0;
    open_project_settings_requested = true;
}

void App::project_settings() {
    constexpr auto title = "Project Settings###project-settings";
    if (open_project_settings_requested) {
        ImGui::OpenPopup(title);
        open_project_settings_requested = false;
    }
    const auto *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(920.0f, viewport->WorkSize.x * 0.9f),
                                  std::min(620.0f, viewport->WorkSize.y * 0.9f)), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal(title, &open, ImGuiWindowFlags_NoDocking)) return;
    if (!project_session.loaded() || project_settings_generation != project_session.generation()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputTextWithHint("##filter", "Filter settings", project_settings_filter.InputBuf,
                                 IM_ARRAYSIZE(project_settings_filter.InputBuf)))
        project_settings_filter.Build();

    const char *categories[] = {"Config", "Notes", "Workspace", "Input"};
    const char *search_terms[] = {"Application Config Name Author Version", "Application Notes",
                                  "Project Workspace Manifest Directory", "Input Video Keyboard Mouse Mode Relative Absolute"};
    const bool filtering = project_settings_filter.IsActive();
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    ImGui::BeginChild("categories", ImVec2(180, -footer));
    for (int i = 0; i < 4; ++i) {
        if (filtering && !project_settings_filter.PassFilter(search_terms[i])) continue;
        if (ImGui::Selectable(categories[i], project_settings_category == i))
            project_settings_category = i;
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("properties", ImVec2(0, -footer));
    bool any = false;
    ImGui::BeginDisabled(project_session.busy());
    for (int category = 0; category < 4; ++category) {
        if (!filtering && project_settings_category != category) continue;
        if (filtering && !project_settings_filter.PassFilter(search_terms[category])) continue;
        if (category == 3) {
            any = true;
            const auto devices = snapshot ? input_devices(*snapshot) : std::vector<InputDevice>{};
            auto rows = project_input_draft;
            if (snapshot) {
                std::map<Handle, uint32_t> ordinals;
                for (const auto &surface : snapshot->inspection->video_surfaces) {
                    const auto ordinal = ordinals[surface.owner]++;
                    const auto key = project_session.input_card_id(surface.owner);
                    if (std::none_of(rows.begin(), rows.end(), [&](const auto &r) {
                        return r.video_card == key && r.surface == ordinal;
                    })) rows.push_back({key, ordinal, {}, {}, false});
                }
            }
            auto card_label = [&](const std::string &key) {
                if (key.empty()) return std::string("None");
                const auto owner = project_session.input_card_owner(key);
                if (snapshot) for (const auto &card : snapshot->inspection->cards)
                    if (card.id == owner) return card.display_name() + " (" + key + ")";
                return std::string("Missing: ") + key;
            };
            if (rows.empty()) ImGui::TextDisabled("No video surfaces.");
            for (auto row : rows) {
                ImGui::PushID(row.video_card.c_str()); ImGui::PushID(static_cast<int>(row.surface));
                const auto title = card_label(row.video_card) + " / Surface " + std::to_string(row.surface + 1);
                ImGui::TextUnformatted(title.c_str());
                bool edited = false;
                if (ImGui::BeginTable("input-fields", 2, ImGuiTableFlags_BordersInnerV)) {
                    ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 110);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
                    auto field = [&](const char *label) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(label); ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-FLT_MIN);
                    };
                    auto select = [&](const char *label, std::string &key, bool mouse) {
                        field(label);
                        auto preview = card_label(key);
                        const auto owner = project_session.input_card_owner(key);
                        const auto compatible = [&](const auto &d) {
                            return mouse ? !d.mouse.empty() && (row.relative ? d.relative : d.absolute) : !d.keyboard.empty();
                        };
                        if (owner && std::none_of(devices.begin(), devices.end(), [&](const auto &d) {
                            return d.owner == owner && compatible(d);
                        })) preview += " — unavailable";
                        if (ImGui::BeginCombo((std::string("##") + label).c_str(), preview.c_str())) {
                            if (ImGui::Selectable("None", key.empty())) { key.clear(); edited = true; }
                            for (const auto &device : devices) {
                                if (!compatible(device)) continue;
                                const auto id = project_session.input_card_id(device.owner);
                                if (id.empty()) continue;
                                if (ImGui::Selectable(card_label(id).c_str(), key == id)) { key = id; edited = true; }
                            }
                            ImGui::EndCombo();
                        }
                    };
                    select("Keyboard", row.keyboard, false);
                    field("Mouse mode");
                    int mode = row.relative ? 1 : 0;
                    if (ImGui::Combo("##mode", &mode, "Absolute\0Relative\0")) { row.relative = mode == 1; edited = true; }
                    select("Mouse", row.mouse, true);
                    ImGui::EndTable();
                }
                if (edited) {
                    auto found = std::find_if(project_input_draft.begin(), project_input_draft.end(), [&](const auto &r) {
                        return r.video_card == row.video_card && r.surface == row.surface;
                    });
                    if (found == project_input_draft.end()) project_input_draft.push_back(std::move(row));
                    else *found = std::move(row);
                }
                ImGui::PopID(); ImGui::PopID();
            }
            continue;
        }
        ImGui::PushID(category);
        if (ImGui::BeginTable("fields", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Property", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
            auto row = [&](const char *label, const char *search) {
                if (filtering && !project_settings_filter.PassFilter(search)) return false;
                any = true;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(label);
                ImGui::TableNextColumn();
                ImGui::SetNextItemWidth(-FLT_MIN);
                return true;
            };
            if (category == 0) {
                if (row("Name", "Application Config Name"))
                    ImGui::InputText("##name", &project_settings_draft.name);
                if (row("Author", "Application Config Author"))
                    ImGui::InputText("##author", &project_settings_draft.author);
                if (row("Version", "Application Config Version"))
                    ImGui::InputText("##version", &project_settings_draft.version);
            } else if (category == 1) {
                if (row("Notes", "Application Notes"))
                    ImGui::InputTextMultiline("##notes", &project_settings_draft.notes, ImVec2(-FLT_MIN, 240));
            } else {
                if (row("Manifest", "Project Workspace Manifest"))
                    ImGui::TextWrapped("%s", project_session.path().empty() ? "Unsaved project" :
                                       project_session.path().filename().string().c_str());
                if (row("Directory", "Project Workspace Directory"))
                    ImGui::TextWrapped("%s", project_session.path().empty() ? "Not saved yet" :
                                       project_session.path().parent_path().string().c_str());
            }
            ImGui::EndTable();
        }
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    if (!any) ImGui::TextDisabled("No matching settings.");
    ImGui::EndChild();

    auto current = project_session.rack_info();
    // Thumbnail imports can finish while this dialog is open. Preserve their result.
    project_settings_draft.thumbnail = current.thumbnail;
    const bool changed = project_settings_draft != current || project_input_draft != project_session.input_routes();
    auto apply = [&] {
        host_input.release(window, controller);
        project_session.commit_input_routes(project_settings_generation, project_input_draft);
        if (project_session.commit_rack_info(project_settings_draft))
            rack_info_initialized = false;
    };
    if (changed) {
        ImGui::TextDisabled("Unapplied changes");
        ImGui::SameLine();
    }
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetWindowContentRegionMax().x - 286));
    if (ImGui::Button("Cancel", ImVec2(90, 0))) ImGui::CloseCurrentPopup();
    ImGui::SameLine();
    ImGui::BeginDisabled(project_session.busy() || !changed);
    if (ImGui::Button("Apply", ImVec2(90, 0))) apply();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(project_session.busy());
    if (ImGui::Button("OK", ImVec2(90, 0))) {
        apply();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace srz80::ui
