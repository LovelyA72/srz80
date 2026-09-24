#include "gui.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <string>

#include <stb_image.h>

namespace srz80::ui {
namespace {

SDL_Texture *load_texture(SDL_Renderer *renderer, const std::filesystem::path &path,
                          int &width, int &height) {
    int channels = 0;
    size_t file_size = 0;
    void *file_data = SDL_LoadFile(path.string().c_str(), &file_size);
    auto *pixels = file_data
                       ? stbi_load_from_memory(static_cast<const stbi_uc *>(file_data),
                                               static_cast<int>(file_size), &width, &height,
                                               &channels, 4)
                       : nullptr;
    SDL_free(file_data);
    if (!pixels)
        return nullptr;
    SDL_Surface *surface = SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_RGBA32,
                                                 pixels, width * 4);
    SDL_Texture *texture = surface ? SDL_CreateTextureFromSurface(renderer, surface) : nullptr;
    if (surface)
        SDL_DestroySurface(surface);
    stbi_image_free(pixels);
    if (texture) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    return texture;
}

std::filesystem::path welcome_asset(const std::filesystem::path &base, const char *name) {
    auto path = base / "resources" / name;
    if (std::filesystem::is_regular_file(path))
        return path;
#ifdef SRZ80_SOURCE_DIR
    path = std::filesystem::path(SRZ80_SOURCE_DIR) / "resources" / name;
#endif
    return path;
}

} // namespace

void App::welcome() {
    if (renderer && !welcome_banner) {
        welcome_banner = load_texture(renderer, welcome_asset(base, "banner.png"),
                                      welcome_banner_width, welcome_banner_height);
        welcome_logo = load_texture(renderer, welcome_asset(base, "banner-logo.png"),
                                    welcome_logo_width, welcome_logo_height);
    }

    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    const float width = std::min(920.0f, viewport->WorkSize.x - 32.0f);
    const float height = std::min(670.0f, viewport->WorkSize.y - 32.0f);
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::max(width, 420.0f), std::max(height, 420.0f)),
                             ImGuiCond_Always);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking |
                                       ImGuiWindowFlags_NoSavedSettings;
    if (!ImGui::Begin("Welcome to SRZ80", nullptr, flags)) {
        ImGui::End();
        return;
    }

    if (welcome_banner && welcome_banner_width > 0) {
        const ImVec2 image_pos = ImGui::GetCursorScreenPos();
        const float image_width = ImGui::GetContentRegionAvail().x;
        const float image_height = image_width * welcome_banner_height / welcome_banner_width;
        ImGui::Image(ImTextureID(reinterpret_cast<uintptr_t>(welcome_banner)),
                     ImVec2(image_width, image_height));
        if (welcome_logo && welcome_logo_width > 0) {
            const float logo_width = image_width * 0.42f;
            const float logo_height = logo_width * welcome_logo_height / welcome_logo_width;
            const ImVec2 first(image_pos.x + 18.0f, image_pos.y + 14.0f);
            ImGui::GetWindowDrawList()->AddImage(
                ImTextureID(reinterpret_cast<uintptr_t>(welcome_logo)), first,
                ImVec2(first.x + logo_width, first.y + logo_height));
        }
    }

    ImGui::Spacing();
    const float action_width = std::max(160.0f,
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f);
    if (ImGui::Button("New project...", ImVec2(action_width, 0))) {
        if (project_session.request(ProjectSession::ActionKind::new_project)) {
            welcome_project_request = true;
            show_welcome = false;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Open project...", ImVec2(-FLT_MIN, 0))) {
        if (project_session.request(ProjectSession::ActionKind::open))
            welcome_project_request = true;
    }

    if (!error.empty()) {
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.45f, 1.0f));
        ImGui::TextWrapped("Could not load project: %s", error.c_str());
        ImGui::PopStyleColor();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Recent projects");
    if (recent_projects.empty()) {
        ImGui::TextDisabled("No recent projects yet.");
    } else {
        ImGui::BeginChild("welcome_recent_projects", ImVec2(0, 0), ImGuiChildFlags_None);
        for (size_t index = 0; index < recent_projects.size(); ++index) {
            const auto &path = recent_projects[index];
            std::error_code ec;
            const bool available = std::filesystem::is_regular_file(path, ec);
            const std::string name = recent_project_names.at(path);
            ImGui::PushID(static_cast<int>(index));
            ImGui::BeginDisabled(!available);
            if (ImGui::Selectable(name.c_str())) {
                if (project_session.request(ProjectSession::ActionKind::open, path)) {
                    welcome_project_request = true;
                    show_welcome = false;
                }
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("%s%s", path.string().c_str(), available ? "" : "\nFile is missing");
            ImGui::PopID();
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

} // namespace srz80::ui
