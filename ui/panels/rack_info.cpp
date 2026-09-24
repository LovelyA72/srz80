#include "../gui.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>

namespace srz80::ui {
namespace {

template <size_t Size>
void copy_text(std::array<char, Size> &target, const std::string &source) {
    std::snprintf(target.data(), target.size(), "%s", source.c_str());
}

SDL_Texture *make_texture(SDL_Renderer *renderer, const RackThumbnail &image) {
    if (!renderer || image.pixels.empty() || image.width <= 0 || image.height <= 0) return nullptr;
    auto *surface = SDL_CreateSurfaceFrom(image.width, image.height, SDL_PIXELFORMAT_RGBA32,
                                          const_cast<uint8_t *>(image.pixels.data()), image.width * 4);
    auto *texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
    if (surface) SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    return texture;
}

} // namespace

void App::rack_info() {
    if (!project_session.loaded()) {
        show_rack_info = false;
        return;
    }
    if (!rack_info_initialized || rack_info_generation != project_session.generation()) {
        rack_info_initialized = true;
        rack_info_generation = project_session.generation();
        rack_info_draft = project_session.rack_info();
        copy_text(rack_info_name, rack_info_draft.name);
        copy_text(rack_info_author, rack_info_draft.author);
        copy_text(rack_info_version, rack_info_draft.version);
        copy_text(rack_info_notes, rack_info_draft.notes);
        if (rack_thumbnail_texture) {
            SDL_DestroyTexture(rack_thumbnail_texture);
            rack_thumbnail_texture = nullptr;
        }
        rack_thumbnail_width = rack_thumbnail_height = 0;
        if (!rack_info_draft.thumbnail.empty() && !rack_thumbnail_job.valid()) {
            rack_thumbnail_import = false;
            rack_thumbnail_generation = rack_info_generation;
            rack_thumbnail_job = std::async(std::launch::async,
                [data = rack_info_draft.thumbnail] { return decode_rack_thumbnail(data); });
        }
    }

    if (rack_thumbnail_job.valid() &&
        rack_thumbnail_job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        auto image = rack_thumbnail_job.get();
        if (rack_thumbnail_generation != project_session.generation()) {
            rack_info_initialized = false;
        } else if (!image.error.empty()) {
            error = image.error;
        } else {
            if (rack_thumbnail_texture) SDL_DestroyTexture(rack_thumbnail_texture);
            rack_thumbnail_texture = make_texture(renderer, image);
            rack_thumbnail_width = image.width;
            rack_thumbnail_height = image.height;
            if (rack_thumbnail_import) {
                rack_info_draft.thumbnail = std::move(image.encoded);
                project_session.commit_rack_info(rack_info_draft);
            }
        }
        rack_thumbnail_import = false;
    }

    ImGui::SetNextWindowSize(ImVec2(560, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Rack Info", &show_rack_info)) {
        ImGui::End();
        return;
    }

    const float field_x = ImGui::GetCursorPosX() + ImGui::CalcTextSize("Version").x +
                          ImGui::GetStyle().ItemSpacing.x;
    const auto text_field = [&](const char *label, const char *id, auto &buffer) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(label);
        ImGui::SameLine(field_x);
        ImGui::SetNextItemWidth(-FLT_MIN);
        return ImGui::InputText(id, buffer.data(), buffer.size());
    };
    bool changed = text_field("Name", "##rack-info-name", rack_info_name);
    changed |= text_field("Author", "##rack-info-author", rack_info_author);
    changed |= text_field("Version", "##rack-info-version", rack_info_version);

    if (rack_thumbnail_texture) {
        const float longest = static_cast<float>(std::max(rack_thumbnail_width, rack_thumbnail_height));
        const float scale = longest > 240.0f ? 240.0f / longest : 1.0f;
        ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(rack_thumbnail_texture)),
                     ImVec2(rack_thumbnail_width * scale, rack_thumbnail_height * scale));
    } else {
        ImGui::TextDisabled(rack_thumbnail_job.valid() ? "Loading image..." : "No image");
    }
    if (ImGui::Button("Choose image...")) select_rack_thumbnail_file_dialog();
    ImGui::SameLine();
    ImGui::BeginDisabled(rack_info_draft.thumbnail.empty());
    if (ImGui::Button("Remove image")) {
        rack_info_draft.thumbnail.clear();
        if (rack_thumbnail_texture) SDL_DestroyTexture(rack_thumbnail_texture);
        rack_thumbnail_texture = nullptr;
        rack_thumbnail_width = rack_thumbnail_height = 0;
        project_session.commit_rack_info(rack_info_draft);
    }
    ImGui::EndDisabled();

    ImGui::TextUnformatted("Project notes");
    changed |= ImGui::InputTextMultiline("##project-notes", rack_info_notes.data(),
                                          rack_info_notes.size(), ImVec2(-FLT_MIN, 180));
    if (changed) {
        rack_info_draft.name = rack_info_name.data();
        rack_info_draft.author = rack_info_author.data();
        rack_info_draft.version = rack_info_version.data();
        rack_info_draft.notes = rack_info_notes.data();
        project_session.commit_rack_info(rack_info_draft);
    }
    ImGui::End();
}

void App::memory_buses() {
    if (!project_session.loaded()) { show_memory_buses = false; return; }
    static uint64_t draft_generation = 0, draft_revision = 0;
    static nlohmann::json draft;
    static int selected_bus = -1;
    static std::array<char, 128> name{}, maximum{};
    static int fallback = 0, resolver = 0;
    static bool random = false;
    auto load = [&](int index) {
        selected_bus = index;
        if (index < 0 || index >= static_cast<int>(draft.size())) return;
        const auto &bus = draft[index];
        copy_text(name, bus.value("name", std::string{}));
        auto value = bus.value("maximum", nlohmann::json{});
        copy_text(maximum, value.is_string() ? value.get<std::string>() : value.dump());
        auto byte = bus.value("unclaimed", nlohmann::json(0));
        fallback = static_cast<int>(std::stoul(byte.is_string() ? byte.get<std::string>() : byte.dump(), nullptr, 0));
        resolver = bus.value("resolver", std::string("priority")) == "or" ? 1 : 0;
        random = bus.value("random", false);
    };
    if (draft_generation != project_session.generation() || draft_revision != project_session.revision()) {
        draft_generation = project_session.generation();
        draft_revision = project_session.revision();
        draft = project_session.document().value("spaces", nlohmann::json::array());
        load(draft.empty() ? -1 : 0);
    }
    ImGui::SetNextWindowSize(ImVec2(620, 430), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Memory buses", &show_memory_buses)) { ImGui::End(); return; }
    if (ImGui::BeginTable("buses", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Name"); ImGui::TableSetupColumn("Maximum"); ImGui::TableSetupColumn("Resolver");
        ImGui::TableHeadersRow();
        for (int i = 0; i < static_cast<int>(draft.size()); ++i) {
            const auto &bus = draft[i];
            ImGui::PushID(i); ImGui::TableNextRow(); ImGui::TableNextColumn();
            if (ImGui::Selectable(bus.value("name", std::string{}).c_str(), selected_bus == i,
                                  ImGuiSelectableFlags_SpanAllColumns)) load(i);
            ImGui::TableNextColumn();
            auto limit = bus.value("maximum", nlohmann::json{});
            ImGui::TextUnformatted((limit.is_string() ? limit.get<std::string>() : limit.dump()).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(bus.value("resolver", std::string("priority")).c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(project_session.busy());
    if (ImGui::Button("Add bus")) {
        std::string new_name = "new.memory";
        for (int suffix = 2; std::any_of(draft.begin(), draft.end(), [&](const auto &bus) {
                 return bus.value("name", std::string{}) == new_name;
             }); ++suffix) new_name = "new.memory" + std::to_string(suffix);
        draft.push_back({{"name", new_name}, {"maximum", "0xFFFF"},
                         {"unclaimed", 0}, {"resolver", "priority"}});
        load(static_cast<int>(draft.size()) - 1);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(selected_bus < 0);
    if (ImGui::Button("Remove bus")) {
        draft.erase(draft.begin() + selected_bus);
        load(draft.empty() ? -1 : std::min(selected_bus, static_cast<int>(draft.size()) - 1));
    }
    ImGui::EndDisabled();
    if (selected_bus >= 0 && selected_bus < static_cast<int>(draft.size())) {
        ImGui::Separator();
        bool edited = ImGui::InputText("Name", name.data(), name.size());
        edited |= ImGui::InputText("Maximum", maximum.data(), maximum.size());
        edited |= ImGui::SliderInt("Unclaimed byte", &fallback, 0, 255);
        const char *resolvers[] = {"Priority", "Bitwise OR"};
        edited |= ImGui::Combo("Resolver", &resolver, resolvers, 2);
        edited |= ImGui::Checkbox("Random unclaimed reads", &random);
        if (edited) {
            auto &bus = draft[selected_bus];
            bus["name"] = name.data();
            bus["maximum"] = maximum.data();
            bus["unclaimed"] = fallback;
            bus["resolver"] = resolver ? "or" : "priority";
            bus["random"] = random;
        }
    }
    ImGui::EndDisabled();
    ImGui::Separator();
    ImGui::BeginDisabled(project_session.busy() || draft == project_session.document().value("spaces", nlohmann::json::array()));
    if (ImGui::Button("Apply changes")) {
        ProjectSession::Action action;
        action.kind = ProjectSession::ActionKind::edit_spaces;
        action.config = draft;
        action.selected = selected;
        project_session.request(std::move(action));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Discard changes")) {
        draft = project_session.document().value("spaces", nlohmann::json::array());
        load(draft.empty() ? -1 : 0);
    }
    ImGui::End();
}

} // namespace srz80::ui
