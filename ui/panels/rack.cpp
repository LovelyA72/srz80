#include "../gui.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace srz80::ui {
namespace {

uint64_t config_number(const nlohmann::json &card, const char *name, uint64_t fallback = 0) {
    if (!card.contains(name))
        return fallback;
    const auto &value = card.at(name);
    if (value.is_number_unsigned())
        return value.get<uint64_t>();
    if (value.is_number_integer() && value.get<int64_t>() >= 0)
        return static_cast<uint64_t>(value.get<int64_t>());
    if (value.is_string()) {
        const auto text = value.get<std::string>();
        size_t end = 0;
        auto result = std::stoull(text, &end, 0);
        if (end == text.size())
            return result;
    }
    throw std::invalid_argument(std::string("Invalid ") + name + " value");
}

// A card whose plugin is still discoverable keeps its descriptor, so its saved
// configuration stays editable even while the card itself could not be built.
bool plugin_discovered(const std::vector<UiCardType> &types, const std::string &id) {
    return std::any_of(types.begin(), types.end(),
                       [&](const auto &type) { return type.id == id; });
}

std::string card_type_display_name(const std::vector<UiCardType> &types, const std::string &id) {
    const auto type = std::find_if(types.begin(), types.end(),
                                   [&](const auto &candidate) { return candidate.id == id; });
    return type != types.end() && !type->name.empty() ? type->name : id;
}

std::string card_display_name(const UiCardInfo &card, const std::vector<UiCardType> &types) {
    return card.name.empty() ? card_type_display_name(types, card.type) : card.name;
}

std::string card_display_name(const nlohmann::json &card, const std::vector<UiCardType> &types) {
    const auto name = card.value("name", std::string());
    if (!name.empty())
        return name;
    return card_type_display_name(types, card.value("plugin", std::string("card")));
}

// Cards the engine could not build stay in the rack so their saved
// configuration survives, and every one of their lines is drawn in this color.
const ImVec4 invalid_text = ImVec4(0.95f, 0.35f, 0.25f, 1.0f);

// An invalid card is drawn with a neutral swatch instead of its plugin color,
// so no working bus mapping is suggested for it.
const ImU32 invalid_swatch = IM_COL32(0x66, 0x66, 0x66, 255);

} // namespace

bool App::file_input(const char *id, const char *label, char *path, size_t capacity) {
    const auto &style = ImGui::GetStyle();
    const float button_width = ImGui::CalcTextSize("...").x + style.FramePadding.x * 2;
    const float input_width = std::max(1.0f, ImGui::CalcItemWidth() - button_width - style.ItemSpacing.x);
    ImGui::SetNextItemWidth(input_width);
    const std::string input_id = std::string("##") + id;
    ImGui::InputText(input_id.c_str(), path, capacity);
    note_text_input(path, capacity);
    ImGui::SameLine();
    const bool browse = ImGui::Button((std::string("...##") + id).c_str());
    ImGui::SameLine();
    ImGui::TextUnformatted(label);
    return browse;
}

void App::poll_card_types() {
    if (!card_types_request.valid())
        return;
    if (card_types_request.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        return;
    auto reply = card_types_request.get();
    if (reply.status != SRH_OK) {
        controller.log_message("[system] Card plugin discovery failed: " +
                               (reply.error.empty() ? "unknown error" : reply.error));
    }
    // The engine read every descriptor on the simulation thread. Only owned
    // metadata and an owned failure status cross back here.
    card_types = std::move(reply.card_types);
    if (add_type >= static_cast<int>(card_types.size()))
        add_type = 0;
    add_type_seen = -1;
}

void App::remove_card(srz80::Handle id) {
    ProjectSession::Action action;
    action.kind = ProjectSession::ActionKind::park;
    action.card = id;
    action.selected = selected;
    project_session.request(std::move(action));
}
void App::plug_card(srz80::Handle id) {
    ProjectSession::Action action;
    action.kind = ProjectSession::ActionKind::plug;
    action.card = id;
    action.selected = selected;
    project_session.request(std::move(action));
}
void App::put_away_card(srz80::Handle id) {
    ProjectSession::Action action;
    action.kind = ProjectSession::ActionKind::put_away;
    action.card = id;
    action.selected = selected;
    project_session.request(std::move(action));
}

void App::rack() {
    ImGui::Begin("Rack");
    if (!error.empty())
        ImGui::TextWrapped("%s", error.c_str());

    if (ImGui::Button("Add card..."))
        show_add_card = true;
    ImGui::Separator();

    if (!snapshot) {
        ImGui::End();
        return;
    }
    auto cards = snapshot->inspection->cards;
    std::stable_sort(cards.begin(), cards.end(), [&](const auto &left, const auto &right) {
        auto position = [&](Handle id) {
            return std::find_if(project_session.active_cards().begin(), project_session.active_cards().end(),
                                [&](const auto &entry) { return entry.first == id; });
        };
        return position(left.id) < position(right.id);
    });
    if (!snapshot->inspection->all_cards.empty() &&
        std::none_of(snapshot->inspection->all_cards.begin(), snapshot->inspection->all_cards.end(),
                     [&](const auto &c) { return c.id == selected; }))
        selected = cards.empty() ? 0 : cards.back().id;
    if (cards.empty() && project_session.removed_cards().empty())
        ImGui::TextDisabled("Rack empty");

    static const ImU32 colors[] = {IM_COL32(86, 156, 214, 255), IM_COL32(111, 192, 120, 255),
                                   IM_COL32(208, 154, 72, 255), IM_COL32(172, 112, 196, 255)};
    auto color_swatch = [](ImU32 color, bool muted = false) {
        auto position = ImGui::GetCursorScreenPos();
        const ImVec2 size(18.0f, 18.0f);
        ImGui::Dummy(size);
        if (muted) {
            auto rgba = ImGui::ColorConvertU32ToFloat4(color);
            rgba.w = 0.5f;
            color = ImGui::ColorConvertFloat4ToU32(rgba);
        }
        ImGui::GetWindowDrawList()->AddRectFilled(position,
                                                   ImVec2(position.x + size.x, position.y + size.y),
                                                   color);
    };

    ImGui::BeginChild("rack_cards", ImVec2(0, ImGui::GetContentRegionAvail().y), false);
    struct PendingMove {
        Handle source;
        Handle target;
        bool after;
    };
    std::optional<PendingMove> pending_move;
    const bool can_reorder = !project_session.busy();
    for (const auto &card : cards) {
        const auto display_name = card_display_name(card, card_types);
        const bool invalid = !card.load_error.empty();
        // The plugin may be loaded even when the card failed to build (an
        // unreadable ROM image, for example), so keep Card info reachable for
        // as long as its descriptor exists.
        const bool info_editable = !invalid || plugin_discovered(card_types, card.type);
        ImGui::PushID(static_cast<int>(card.id));
        ImU32 card_color = colors[std::hash<std::string>{}(card.type) % 4];
        color_swatch(invalid ? invalid_swatch : card_color);
        ImGui::SameLine();
        char label[512];
        std::snprintf(label, sizeof(label), "%s   ID %llu   Priority %d",
                      display_name.c_str(), static_cast<unsigned long long>(card.id), card.priority);
        const float selectable_width = std::max(80.0f, ImGui::GetContentRegionAvail().x - 112.0f);
        if (invalid)
            ImGui::PushStyleColor(ImGuiCol_Text, invalid_text);
        const bool select_card = ImGui::Selectable(label, selected == card.id, 0, ImVec2(selectable_width, 0));
        if (invalid)
            ImGui::PopStyleColor();
        if (select_card)
            selected = card.id;
        if (!can_reorder && ImGui::IsItemHovered())
            ImGui::SetTooltip("Wait for the project change to finish");
        if (can_reorder && ImGui::BeginDragDropSource()) {
            const Handle payload = card.id;
            ImGui::SetDragDropPayload("SRZ80_RACK_CARD", &payload, sizeof(payload));
            ImGui::TextUnformatted(display_name.c_str());
            ImGui::EndDragDropSource();
        }
        if (can_reorder && ImGui::BeginDragDropTarget()) {
            if (const auto *payload = ImGui::AcceptDragDropPayload("SRZ80_RACK_CARD")) {
                Handle source = 0;
                if (payload->DataSize == sizeof(source)) {
                    std::memcpy(&source, payload->Data, sizeof(source));
                    const auto middle = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
                    pending_move = PendingMove{source, card.id, ImGui::GetMousePos().y >= middle};
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!info_editable);
        if (ImGui::SmallButton("INFO")) {
            open_card_info(card.id);
        }
        if (!info_editable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Plugin \"%s\" is missing, so there is nothing to configure",
                              card.type.c_str());
        else if (info_editable && invalid && ImGui::IsItemHovered())
            ImGui::SetTooltip("Open Card info and fix the saved settings");
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove"))
            remove_card(card.id);
        if (invalid) {
            ImGui::PushStyleColor(ImGuiCol_Text, invalid_text);
            ImGui::TextWrapped("Unavailable: %s", card.load_error.c_str());
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(info_editable
                    ? "Card is inactive, but its settings are still saved. Fix them in Card info or reopen the project to retry"
                    : "Card is inactive, but its settings are still saved. Restore the plugin, then reopen the project");
        }
        ImGui::PopID();
    }
    if (can_reorder && pending_move && pending_move->source != pending_move->target) {
        ProjectSession::Action action;
        action.kind = ProjectSession::ActionKind::reorder;
        action.card = pending_move->source;
        action.target = pending_move->target;
        action.after = pending_move->after;
        action.selected = selected;
        project_session.request(std::move(action));
    }

    if (!project_session.removed_cards().empty()) {
        ImGui::Separator();
        ImGui::TextDisabled("Removed (parked) cards");
        enum class RemovedAction { plug, put_away };
        std::optional<std::pair<RemovedAction, Handle>> removed_action;
        for (const auto &entry : project_session.removed_cards()) {
            auto id = entry.first;
            const auto display_name = card_display_name(entry.second, card_types);
            std::string type = entry.second.value("plugin", std::string());
            int32_t priority = entry.second.value("priority", 0);
            ImGui::PushID(static_cast<int>(id));
            ImU32 card_color = colors[std::hash<std::string>{}(type.empty() ? "?" : type) % 4];
            char label[256];
            std::snprintf(label, sizeof(label), "%s   ID %llu   Priority %d",
                          display_name.c_str(),
                          static_cast<unsigned long long>(id), priority);
            const auto runtime_card = std::find_if(snapshot->inspection->all_cards.begin(),
                snapshot->inspection->all_cards.end(), [id](const auto &card) { return card.id == id; });
            const bool invalid = runtime_card != snapshot->inspection->all_cards.end() &&
                                 !runtime_card->load_error.empty();
            // Parked cards stay muted. An invalid one shows the neutral swatch.
            color_swatch(invalid ? invalid_swatch : card_color, !invalid);
            ImGui::SameLine();
            if (invalid)
                ImGui::TextColored(invalid_text, "%s", label);
            else
                ImGui::TextDisabled("%s", label);
            ImGui::SameLine();
            if (ImGui::SmallButton("Plug back"))
                removed_action = std::make_pair(RemovedAction::plug, id);
            ImGui::SameLine();
            if (ImGui::SmallButton("Put away"))
                removed_action = std::make_pair(RemovedAction::put_away, id);
            if (invalid) {
                ImGui::PushStyleColor(ImGuiCol_Text, invalid_text);
                ImGui::TextWrapped("Unavailable: %s", runtime_card->load_error.c_str());
                ImGui::PopStyleColor();
            }
            ImGui::PopID();
        }
        // Submit at most one removed-card action per frame.
        if (removed_action) {
            if (removed_action->first == RemovedAction::plug)
                plug_card(removed_action->second);
            else
                put_away_card(removed_action->second);
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void App::log() {
    ImGui::Begin("Log");
    if (!snapshot) {
        ImGui::End();
        return;
    }
    const auto &audio = snapshot->audio;
    ImGui::Text("Audio: %s | queued %llu/%llu frames | pcm %llu | dropped %llu | underflow %llu",
                audio_backend.opened() ? "enabled" : "disabled",
                static_cast<unsigned long long>(audio.queued_frames),
                static_cast<unsigned long long>(audio.queue_capacity_frames),
                static_cast<unsigned long long>(audio.pcm_queued_frames),
                static_cast<unsigned long long>(audio.dropped_frames),
                static_cast<unsigned long long>(audio.underflow_frames));
    if (!audio_backend.opened() && !audio_backend.error().empty())
        ImGui::TextDisabled("%s", audio_backend.error().c_str());
    ImGui::Separator();
    if (ImGui::SmallButton("Clear"))
        controller.clear_logs();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu entries", snapshot->inspection->logs.size());
    std::string log_text;
    for (size_t index = 0; index < snapshot->inspection->logs.size(); ++index) {
        if (index) log_text += '\n';
        log_text += snapshot->inspection->logs[index];
    }
    std::vector<char> buffer(log_text.begin(), log_text.end());
    buffer.push_back('\0');
    ImGui::InputTextMultiline("##log_text", buffer.data(), buffer.size(), ImVec2(-FLT_MIN, -FLT_MIN),
                              ImGuiInputTextFlags_ReadOnly);
    ImGui::End();
}

void App::add_card_modal() {
    if (show_add_card && !ImGui::IsPopupOpen("Add card"))
        ImGui::OpenPopup("Add card");
    if (ImGui::BeginPopupModal("Add card", &show_add_card, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!error.empty())
            ImGui::TextWrapped("%s", error.c_str());
        if (!snapshot) {
            ImGui::EndPopup();
            return;
        }
        if (card_types.empty()) {
            if (discovering_card_types())
                ImGui::TextDisabled("Reading card plugins from %s...",
                                    (base / "plugins").string().c_str());
            else
                ImGui::TextDisabled("No card plugins found in %s", (base / "plugins").string().c_str());
            ImGui::EndPopup();
            return;
        }
        if (add_type < 0 || add_type >= static_cast<int>(card_types.size()))
            add_type = 0;
        if (add_type_seen != add_type) {
            const auto &type = card_types[static_cast<size_t>(add_type)];
            add_base = type.default_base;
            add_size = type.default_size;
            add_priority = type.default_priority;
            add_clock = static_cast<int>(type.default_clock);
            replace_path_buffers(rom_paths, type.image_slots.size());
            add_project_file_paths.clear();
            const auto defaults = nlohmann::json::parse(type.config_json, nullptr, false);
            if (defaults.is_object())
                for (auto it = defaults.begin(); it != defaults.end(); ++it)
                    if (it.value().is_string() && project_file_config_key(it.key())) {
                        auto &buffer = add_project_file_paths[it.key()];
                        const auto value = it.value().get<std::string>();
                        if (value.size() < buffer.size())
                            std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
                    }
            add_type_seen = add_type;
        }
        const auto &type = card_types[static_cast<size_t>(add_type)];
        if (ImGui::BeginCombo("Type", type.name.c_str())) {
            std::string category;
            for (size_t i = 0; i < card_types.size(); ++i) {
                const auto &candidate = card_types[i];
                if (candidate.category != category) {
                    if (!category.empty())
                        ImGui::Separator();
                    ImGui::TextDisabled("%s", candidate.category.c_str());
                    category = candidate.category;
                }
                const std::string label = candidate.name + "##card_type_" + candidate.id;
                if (ImGui::Selectable(label.c_str(), add_type == static_cast<int>(i))) {
                    add_type = static_cast<int>(i);
                    add_type_seen = -1;
                }
                if (ImGui::IsItemHovered() && !candidate.description.empty())
                    ImGui::SetTooltip("%s", candidate.description.c_str());
            }
            ImGui::EndCombo();
        }
        if (!type.description.empty())
            ImGui::TextDisabled("%s", type.description.c_str());
        select_space("Space");
        if (type.flags & SRH_CARD_REQUIRES_IO_SPACE) {
            if (!add_io_space)
                add_io_space = find_space("cpu0.io") ? find_space("cpu0.io") : space;
            combo_space("I/O space", add_io_space);
        }
        hex_input("Base / reset PC", add_base);
        ImGui::InputScalar("Size", ImGuiDataType_U64, &add_size);
        ImGui::InputInt("Priority", &add_priority);
        if (!type.image_slots.empty()) {
            if (focus_rom_path) {
                ImGui::SetKeyboardFocusHere();
                focus_rom_path = false;
            }
            for (uint32_t image = 0; image < type.image_slots.size(); ++image) {
                char id[32];
                std::snprintf(id, sizeof(id), "add_rom_file_%u", image);
                auto &path = rom_paths[image];
                if (file_input(id, type.image_slots[image].c_str(), path.data(), path.size()))
                    select_rom_file_dialog(false, image);
            }
        }
        for (auto &[key, path] : add_project_file_paths) {
            std::string label = key;
            std::replace(label.begin(), label.end(), '_', ' ');
            if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
            ImGui::BeginDisabled(project_session.path().empty());
            if (file_input(key.c_str(), label.c_str(), path.data(), path.size()))
                select_project_file_dialog(FileDialogAction::select_add_project_file, key);
            ImGui::EndDisabled();
        }
        if (type.flags & SRH_CARD_SHOW_CLOCK)
            ImGui::SliderInt("Clock", &add_clock, 0, 2);
        ImGui::BeginDisabled(!space || project_session.busy());
        if (ImGui::Button("Insert")) {
            try {
                std::string config_json;
                nlohmann::json card_config = nlohmann::json::parse(type.config_json);
                if (!card_config.is_object())
                    throw std::runtime_error("Card default configuration must be an object");
                if (type.flags & SRH_CARD_REQUIRES_IO_SPACE) {
                    if (!add_io_space)
                        throw std::runtime_error("Card requires an I/O address space");
                    if (type.io_space_config_key.empty())
                        throw std::runtime_error("Card does not define its I/O space config key");
                    card_config[type.io_space_config_key] = snapshot->inspection->spaces.at(add_io_space).name;
                }
                if (!type.base_config_key.empty())
                    card_config[type.base_config_key] = add_base;
                for (const auto &[key, path] : add_project_file_paths)
                    card_config[key] = validated_project_file_path(path.data());
                config_json = card_config.dump();
                SimulationController::CardRequest request;
                request.type = type.id;
                request.space = space;
                request.base = add_base;
                request.size = add_size;
                request.reset_vector = add_base;
                request.priority = add_priority;
                request.clock = static_cast<uint32_t>(add_clock);
                request.config_json = config_json;
                request.plugin_directory = base / "plugins";
                if (!type.image_slots.empty()) {
                    for (uint32_t image = 0; image < type.image_slots.size(); ++image) {
                        const auto &path = rom_paths[image];
                        if (!path[0]) {
                            request.image_paths.emplace_back();
                            continue;
                        }
                        request.image_paths.push_back(
                            std::filesystem::absolute(path.data()).lexically_normal());
                    }
                }

                ProjectSession::Action action;
                action.kind = ProjectSession::ActionKind::insert;
                action.insertion = std::move(request);
                action.settings.space_name = snapshot->inspection->spaces.at(space).name;
                action.selected = selected;
                if (project_session.request(std::move(action))) {
                    show_add_card = false;
                    ImGui::CloseCurrentPopup();
                }
            } catch (const std::exception &e) {
                error = e.what();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close"))
            show_add_card = false;
        ImGui::EndPopup();
    }
}

void App::open_card_info(Handle id) {
    auto entry = std::find_if(project_session.active_cards().begin(), project_session.active_cards().end(),
                              [&](const auto &candidate) { return candidate.first == id; });
    if (entry == project_session.active_cards().end()) {
        error = "Card configuration is unavailable";
        return;
    }
    try {
        const auto &card = entry->second;
        info_card = id;
        selected = id;
        const auto name = card.value("name", std::string());
        if (name.size() >= sizeof(info_name))
            throw std::invalid_argument("Card name is too long to edit");
        std::snprintf(info_name, sizeof(info_name), "%s", name.c_str());
        info_space = find_space(card.value("space", std::string()));
        info_base = config_number(card, "base");
        info_size = config_number(card, "size");
        info_reset_vector = config_number(card, "reset_vector");
        auto priority = card.value("priority", int64_t(0));
        if (priority < INT32_MIN || priority > INT32_MAX)
            throw std::invalid_argument("Priority out of range");
        info_priority = static_cast<int>(priority);
        info_clock = static_cast<int>(config_number(card, "clock"));
        std::vector<std::string> images;
        if (card.contains("image")) images.push_back(card.at("image").get<std::string>());
        if (card.contains("images")) images = card.at("images").get<std::vector<std::string>>();
        const auto type = std::find_if(card_types.begin(), card_types.end(), [&](const auto &candidate) {
            return candidate.id == card.value("plugin", std::string());
        });
        const size_t declared_slots = type == card_types.end() ? 0 : type->image_slots.size();
        replace_path_buffers(info_rom_paths, std::max(declared_slots, images.size()));
        for (size_t index = 0; index < images.size(); ++index) {
            if (images[index].size() >= info_rom_paths[index].size())
                throw std::invalid_argument("ROM path is too long to edit");
            std::snprintf(info_rom_paths[index].data(), info_rom_paths[index].size(), "%s",
                          images[index].c_str());
        }
        const auto config = card.value("config", nlohmann::json::object()).dump(2);
        info_project_file_paths.clear();
        const auto settings = card.value("config", nlohmann::json::object());
        if (type != card_types.end()) {
            const auto defaults = nlohmann::json::parse(type->config_json, nullptr, false);
            if (defaults.is_object())
                for (auto it = defaults.begin(); it != defaults.end(); ++it)
                    if (it.value().is_string() && project_file_config_key(it.key()))
                        info_project_file_paths.try_emplace(it.key());
        }
        if (settings.is_object())
            for (auto it = settings.begin(); it != settings.end(); ++it)
                if (it.value().is_string() && project_file_config_key(it.key())) {
                    auto &buffer = info_project_file_paths[it.key()];
                    const auto value = it.value().get<std::string>();
                    if (value.size() >= buffer.size())
                        throw std::invalid_argument("Project file path is too long to edit");
                    std::snprintf(buffer.data(), buffer.size(), "%s", value.c_str());
                }
        if (config.size() >= sizeof(info_config))
            throw std::invalid_argument("Plugin configuration is too large to edit");
        std::snprintf(info_config, sizeof(info_config), "%s", config.c_str());
        error.clear();
        show_card_info = true;
    } catch (const std::exception &e) {
        error = e.what();
    }
}

void App::card_info_modal() {
    if (!snapshot)
        return;
    bool alive = std::any_of(snapshot->inspection->all_cards.begin(), snapshot->inspection->all_cards.end(),
                             [&](const auto &c) { return c.id == info_card; });
    if (!alive) {
        show_card_info = false;
        info_card = 0;
        return;
    }
    if (!ImGui::IsPopupOpen("Card info"))
        ImGui::OpenPopup("Card info");
    if (!ImGui::BeginPopupModal("Card info", &show_card_info,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    auto cards = snapshot->inspection->cards;
    auto card = std::find_if(cards.begin(), cards.end(),
                             [&](const auto &candidate) { return candidate.id == info_card; });
    const auto card_type = card == cards.end()
                               ? card_types.end()
                               : std::find_if(card_types.begin(), card_types.end(),
                                              [&](const auto &type) {
                                                  return type.id == card->type;
                                              });
    const bool has_images = !info_rom_paths.empty();
    if (card != cards.end()) {
        const auto &original_name = card_type != card_types.end() ? card_type->name : card->type;
        ImGui::Text("%s card", original_name.c_str());
        ImGui::TextDisabled("ID %llu   Priority %d",
                            static_cast<unsigned long long>(card->id), card->priority);
        if (!card->load_error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, invalid_text);
            ImGui::TextWrapped("Unavailable: %s", card->load_error.c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Correct the configuration below and apply it, or reopen the project to retry.");
        }
    }
    ImGui::Separator();

    if (!error.empty())
        ImGui::TextWrapped("%s", error.c_str());
    ImGui::InputText("Friendly name", info_name, sizeof(info_name));
    combo_space("Space", info_space);
    hex_input("Base", info_base);
    ImGui::InputScalar("Size", ImGuiDataType_U64, &info_size);
    hex_input("Reset vector", info_reset_vector);
    ImGui::InputInt("Priority", &info_priority);
    ImGui::SliderInt("Clock", &info_clock, 0, 2);
    if (has_images) {
        for (uint32_t image = 0; image < info_rom_paths.size(); ++image) {
            char id[32];
            std::snprintf(id, sizeof(id), "info_rom_file_%u", image);
            auto &path = info_rom_paths[image];
            const std::string fallback = "Image " + std::to_string(image + 1);
            const char *label = card_type != card_types.end() && image < card_type->image_slots.size()
                                    ? card_type->image_slots[image].c_str()
                                    : fallback.c_str();
            if (file_input(id, label, path.data(), path.size()))
                select_rom_file_dialog(true, image);
        }
    }
    for (auto &[key, path] : info_project_file_paths) {
        std::string label = key;
        std::replace(label.begin(), label.end(), '_', ' ');
        if (!label.empty()) label[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(label[0])));
        ImGui::BeginDisabled(project_session.path().empty());
        if (file_input(("info_" + key).c_str(), label.c_str(), path.data(), path.size()))
            select_project_file_dialog(FileDialogAction::select_info_project_file, key);
        ImGui::EndDisabled();
    }
    ImGui::TextUnformatted("Plugin configuration (JSON)");
    ImGui::InputTextMultiline("##card_config", info_config, sizeof(info_config), ImVec2(420, 100));
    note_text_input(info_config, sizeof(info_config));
    ImGui::TextDisabled("Applying configuration rebuilds and resets the rack.");

    ImGui::Separator();
    ImGui::BeginDisabled(!info_space || project_session.busy());
    if (ImGui::Button("Apply")) {
        try {
            auto config = nlohmann::json::parse(info_config[0] ? info_config : "{}");
            if (!config.is_object()) throw std::invalid_argument("Plugin configuration must be a JSON object");
            for (const auto &[key, path] : info_project_file_paths)
                config[key] = validated_project_file_path(path.data());
            if (card_type != card_types.end() && !card_type->base_config_key.empty())
                config[card_type->base_config_key] = info_base;
            ProjectSession::Action action;
            action.kind = ProjectSession::ActionKind::configure;
            action.card = info_card;
            action.selected = selected;
            action.settings.name = info_name;
            action.settings.space_name = snapshot->inspection->spaces.at(info_space).name;
            if (has_images) {
                for (const auto &path : info_rom_paths)
                    action.settings.images.emplace_back(path.data());
            }
            action.settings.base = info_base;
            action.settings.size = info_size;
            action.settings.reset_vector = info_reset_vector;
            action.settings.priority = info_priority;
            action.settings.clock = static_cast<uint32_t>(info_clock);
            action.settings.config = std::move(config);
            if (!project_session.request(std::move(action)))
                throw std::runtime_error("Project is busy");
            show_card_info = false;
            info_card = 0;
            ImGui::CloseCurrentPopup();
        } catch (const std::exception &e) {
            error = e.what();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Close")) {
        show_card_info = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace srz80::ui
