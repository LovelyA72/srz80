#include "../gui.hpp"
#include <algorithm>
#include <cctype>
#include <srz80/imgui_input.hpp>
#include <string>
#include <vector>

namespace srz80::ui {

void App::inspector() {
    ImGui::Begin("Device inspector");
    if (!snapshot) {
        ImGui::End();
        return;
    }
    auto cards = snapshot->inspection->cards;
    const char *label = "Select card";
    for (auto &c : cards)
        if (c.id == selected)
            label = c.display_name().c_str();
    if (ImGui::BeginCombo("Card", label)) {
        for (const auto &c : cards) {
            auto name = c.display_name() + " #" + std::to_string(c.id);
            if (ImGui::Selectable(name.c_str(), selected == c.id))
                selected = c.id;
        }
        ImGui::EndCombo();
    }

    auto properties_it = snapshot->inspection->properties.find(selected);
    if (properties_it == snapshot->inspection->properties.end() || properties_it->second.empty()) {
        ImGui::TextUnformatted("This card has no register properties.");
        ImGui::End();
        return;
    }
    const auto &properties = properties_it->second;

    // Group the generic plugin properties by their metadata group, so Z80
    // registers appear as a compact table instead of one long flat list.
    std::vector<std::string> groups;
    std::vector<std::vector<size_t>> group_indices;
    for (size_t i = 0; i < properties.size(); ++i) {
        // Hidden/advanced properties are reserved for dedicated tools/debuggers.
        if (properties[i].ui_flags & SRH_PROPERTY_HIDE_UI)
            continue;
        const std::string &group = properties[i].group.empty() ? "General" : properties[i].group;
        auto found = std::find(groups.begin(), groups.end(), group);
        size_t group_index = 0;
        if (found == groups.end()) {
            group_index = groups.size();
            groups.push_back(group);
            group_indices.emplace_back();
        } else {
            group_index = static_cast<size_t>(std::distance(groups.begin(), found));
        }
        group_indices[group_index].push_back(i);
    }

    auto draw_property = [&](const UiProperty &p, size_t index) {
        auto value = p.value;
        bool changed = false;
        const bool project_file = p.kind == SRH_TEXT && p.editable &&
            (p.ui_flags & SRH_PROPERTY_PERSISTENT) && project_file_config_key(p.name);
        ImGui::PushID(static_cast<int>(index));
        const bool running_without_live_edit =
            snapshot->run_state == UiRunState::running &&
            (p.ui_flags & SRH_PROPERTY_LIVE_EDIT) == 0;
        ImGui::BeginDisabled(running_without_live_edit || !p.editable ||
                             ((p.ui_flags & SRH_PROPERTY_PERSISTENT) && project_session.busy()) ||
                             (project_file && project_session.path().empty()));
        if (p.ui_flags & SRH_PROPERTY_UNAVAILABLE) {
            ImGui::Text("%s: N/A", p.name.c_str());
        } else if (p.kind == SRH_BOOLEAN) {
            bool v = value.unsigned_value != 0;
            changed = ImGui::Checkbox(p.name.c_str(), &v);
            value.unsigned_value = v;
        } else if (project_file) {
            ImGui::TextWrapped("%s", value.text[0] ? value.text : "Choose a file");
            if (ImGui::Button("Browse..."))
                select_project_file_dialog(FileDialogAction::select_inspector_project_file,
                                          p.name, selected, static_cast<uint32_t>(index));
        } else if (p.kind == SRH_TEXT) {
            ImGui::Text("%s: %s", p.name.c_str(), value.text);
        } else if (p.kind == SRH_FIXED) {
            double display = static_cast<double>(value.signed_value) / 1000.0;
            changed = ImGui::InputDouble(p.name.c_str(), &display, 0, 0, "%.3f");
            if (changed && display >= -9.0e15 && display <= 9.0e15)
                value.signed_value = static_cast<int64_t>(display * 1000.0);
            else
                changed = false;
        } else if (p.kind == SRH_SIGNED) {
            changed = ImGui::InputScalar(p.name.c_str(), ImGuiDataType_S64, &value.signed_value);
        } else if (p.kind == SRH_ENUM) {
            std::vector<std::string> labels;
            size_t start = 0;
            while (start <= p.enum_labels.size()) {
                auto end = p.enum_labels.find('|', start);
                labels.push_back(p.enum_labels.substr(start, end - start));
                if (end == std::string::npos)
                    break;
                start = end + 1;
            }
            if (ImGui::BeginCombo(p.name.c_str(),
                                  value.unsigned_value < labels.size()
                                      ? labels[static_cast<size_t>(value.unsigned_value)].c_str()
                                      : "Unknown")) {
                for (size_t n = 0; n < labels.size(); ++n)
                    if (ImGui::Selectable(labels[n].c_str(), value.unsigned_value == n)) {
                        value.unsigned_value = n;
                        changed = true;
                    }
                ImGui::EndCombo();
            }
        } else {
            if (p.base == 16) {
                changed = srz80::gui::input_hexadecimal(p.name.c_str(), value.unsigned_value,
                                                         p.bits);
            } else {
                changed = ImGui::InputScalar(p.name.c_str(), ImGuiDataType_U64,
                                             &value.unsigned_value);
            }
        }
        ImGui::EndDisabled();
        const bool has_tooltip =
            std::any_of(p.description.begin(), p.description.end(),
                        [](unsigned char c) { return !std::isspace(c); });
        if (has_tooltip && ImGui::IsItemHovered())
            ImGui::SetTooltip("%s", p.description.c_str());
        if (changed) {
            std::string edit_error;
            if (p.ui_flags & SRH_PROPERTY_PERSISTENT) {
                const auto project_card = std::find_if(project_session.active_cards().begin(), project_session.active_cards().end(),
                                                       [&](const auto &candidate) {
                                                           return candidate.first == selected;
                                                       });
                if (project_card != project_session.active_cards().end()) {
                    auto config = project_card->second.value("config", nlohmann::json::object());
                    if (!config.is_object())
                        config = nlohmann::json::object();
                    if (p.kind == SRH_ENUM) {
                        const auto labels = p.enum_labels;
                        size_t start = 0;
                        for (uint64_t label = 0; start <= labels.size(); ++label) {
                            const auto end = labels.find('|', start);
                            if (label == value.unsigned_value) {
                                config[p.name] = labels.substr(start, end - start);
                                break;
                            }
                            if (end == std::string::npos)
                                break;
                            start = end + 1;
                        }
                    } else if (p.kind == SRH_TEXT) {
                        config[p.name] = value.text;
                    } else if (p.kind == SRH_SIGNED || p.kind == SRH_FIXED) {
                        config[p.name] = value.signed_value;
                    } else {
                        config[p.name] = value.unsigned_value;
                    }
                    ProjectSession::Action action;
                    action.kind = ProjectSession::ActionKind::property;
                    action.card = selected;
                    action.selected = selected;
                    action.index = index;
                    action.value = value;
                    action.config = std::move(config);
                    project_session.request(std::move(action));
                }
            } else if (controller.request_edit(selected, index, value, edit_error, snapshot->generation) != SRH_OK) {
                error = edit_error.empty() ? "Register edit rejected" : edit_error;
            }
        }
        ImGui::PopID();
    };

    int max_rows = 0;
    for (const auto &indices : group_indices)
        max_rows = std::max(max_rows, static_cast<int>(indices.size()));

    if (!groups.empty() && ImGui::BeginTable("cpu registers", static_cast<int>(groups.size()),
                          ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable)) {
        for (const auto &group : groups)
            ImGui::TableSetupColumn(group.c_str());
        ImGui::TableHeadersRow();
        for (int row = 0; row < max_rows; ++row) {
            ImGui::TableNextRow();
            for (size_t g = 0; g < groups.size(); ++g) {
                ImGui::TableNextColumn();
                if (row < static_cast<int>(group_indices[g].size())) {
                    size_t index = group_indices[g][static_cast<size_t>(row)];
                    draw_property(properties[index], index);
                }
            }
        }
        ImGui::EndTable();
    }

    // Box16-style CPU stack: if the selected CPU exposes SP, show nearby stack
    // bytes from the primary memory space.
    auto memory = find_space("cpu0.memory");
    if (memory) {
        uint64_t sp = 0;
        bool have_sp = false;
        for (const auto &p : properties)
            if (p.name == "SP") {
                sp = p.value.unsigned_value;
                have_sp = true;
                break;
            }
        if (have_sp) {
            ImGui::SeparatorText("Stack");
            const auto max = snapshot->inspection->spaces.at(memory).maximum;
            const uint64_t start = sp & 0xFFFF;
            const uint32_t count =
                static_cast<uint32_t>(std::min<uint64_t>(16, max >= start ? max - start + 1 : 0));
            const auto now = std::chrono::steady_clock::now();
            const bool same = stack_space == memory && stack_base == start && stack_generation == snapshot->generation;
            if (!same)
                stack_cache.clear();
            if (stack_request.valid() && stack_request.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                auto reply = stack_request.get();
                if (same && reply.status == SRH_OK && reply.generation == snapshot->generation) {
                    stack_cache = std::move(reply.memory);
                    stack_cache_time = now;
                }
            }
            if (!stack_request.valid() && (!same || now - stack_cache_time >= std::chrono::milliseconds(snapshot_interval_ms()))) {
                stack_space = memory;
                stack_base = start;
                stack_generation = snapshot->generation;
                stack_request = controller.read_memory_async(memory, start, count, snapshot->generation);
            }
            const auto &cells = stack_cache;
            for (uint32_t i = 0; i < 16; ++i) {
                uint64_t address = (sp + i) & 0xFFFF;
                if (i < cells.size() && cells[i].available()) {
                    ImGui::Text("%04llX: %02X", static_cast<unsigned long long>(address), cells[i].value);
                } else {
                    ImGui::TextDisabled("%04llX: --", static_cast<unsigned long long>(address));
                }
            }
        }
    }

    ImGui::End();
}

} // namespace srz80::ui
