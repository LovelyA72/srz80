#include "theme.hpp"
#include <array>
#include <imgui.h>

namespace srz80::ui {
namespace {

void dark_red() {
    ImGui::StyleColorsDark();
    auto &style = ImGui::GetStyle();

    style.WindowPadding = ImVec2(10.0f, 9.0f);
    style.FramePadding = ImVec2(7.0f, 4.0f);
    style.CellPadding = ImVec2(6.0f, 4.0f);
    style.ItemSpacing = ImVec2(7.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(5.0f, 4.0f);
    style.ScrollbarSize = 13.0f;
    style.GrabMinSize = 9.0f;
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;
    style.WindowRounding = 2.0f;
    style.ChildRounding = 1.0f;
    style.FrameRounding = 1.0f;
    style.PopupRounding = 2.0f;
    style.ScrollbarRounding = 1.0f;
    style.GrabRounding = 1.0f;
    style.TabRounding = 1.0f;

    auto &c = style.Colors;
    c[ImGuiCol_Text] = ImVec4(0.86f, 0.85f, 0.82f, 1.00f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.48f, 0.47f, 0.45f, 1.00f);
    c[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.058f, 0.064f, 1.00f);
    c[ImGuiCol_ChildBg] = ImVec4(0.055f, 0.058f, 0.064f, 0.00f);
    c[ImGuiCol_PopupBg] = ImVec4(0.075f, 0.078f, 0.084f, 0.98f);
    c[ImGuiCol_Border] = ImVec4(0.20f, 0.20f, 0.21f, 1.00f);
    c[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
    c[ImGuiCol_FrameBg] = ImVec4(0.105f, 0.108f, 0.115f, 1.00f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.17f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.25f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_TitleBg] = ImVec4(0.070f, 0.072f, 0.078f, 1.00f);
    c[ImGuiCol_TitleBgActive] = ImVec4(0.095f, 0.086f, 0.090f, 1.00f);
    c[ImGuiCol_TitleBgCollapsed] = ImVec4(0.055f, 0.058f, 0.064f, 1.00f);
    c[ImGuiCol_MenuBarBg] = ImVec4(0.065f, 0.067f, 0.072f, 1.00f);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0.045f, 0.047f, 0.052f, 1.00f);
    c[ImGuiCol_ScrollbarGrab] = ImVec4(0.22f, 0.22f, 0.23f, 1.00f);
    c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.31f, 0.27f, 0.28f, 1.00f);
    c[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.43f, 0.23f, 0.27f, 1.00f);
    c[ImGuiCol_CheckMark] = ImVec4(0.82f, 0.31f, 0.38f, 1.00f);
    c[ImGuiCol_CheckboxSelectedBg] = ImVec4(0.25f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.64f, 0.25f, 0.31f, 1.00f);
    c[ImGuiCol_SliderGrabActive] = ImVec4(0.82f, 0.31f, 0.38f, 1.00f);
    c[ImGuiCol_Button] = ImVec4(0.14f, 0.14f, 0.15f, 1.00f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.34f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.46f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_Header] = ImVec4(0.25f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.36f, 0.17f, 0.21f, 1.00f);
    c[ImGuiCol_HeaderActive] = ImVec4(0.46f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_Separator] = ImVec4(0.22f, 0.22f, 0.23f, 1.00f);
    c[ImGuiCol_SeparatorHovered] = ImVec4(0.51f, 0.24f, 0.29f, 1.00f);
    c[ImGuiCol_SeparatorActive] = ImVec4(0.72f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_ResizeGrip] = ImVec4(0.42f, 0.20f, 0.24f, 0.35f);
    c[ImGuiCol_ResizeGripHovered] = ImVec4(0.64f, 0.25f, 0.31f, 0.75f);
    c[ImGuiCol_ResizeGripActive] = ImVec4(0.82f, 0.31f, 0.38f, 0.95f);
    c[ImGuiCol_Tab] = ImVec4(0.085f, 0.087f, 0.093f, 1.00f);
    c[ImGuiCol_TabHovered] = ImVec4(0.34f, 0.17f, 0.20f, 1.00f);
    c[ImGuiCol_TabSelected] = ImVec4(0.25f, 0.13f, 0.16f, 1.00f);
    c[ImGuiCol_TabSelectedOverline] = ImVec4(0.72f, 0.28f, 0.34f, 1.00f);
    c[ImGuiCol_TabDimmed] = ImVec4(0.060f, 0.062f, 0.068f, 1.00f);
    c[ImGuiCol_TabDimmedSelected] = ImVec4(0.15f, 0.10f, 0.11f, 1.00f);
    c[ImGuiCol_TabDimmedSelectedOverline] = ImVec4(0.42f, 0.20f, 0.24f, 1.00f);
    c[ImGuiCol_DockingPreview] = ImVec4(0.72f, 0.28f, 0.34f, 0.55f);
    c[ImGuiCol_DockingEmptyBg] = ImVec4(0.035f, 0.037f, 0.041f, 1.00f);
    c[ImGuiCol_TableHeaderBg] = ImVec4(0.095f, 0.097f, 0.103f, 1.00f);
    c[ImGuiCol_TableBorderStrong] = ImVec4(0.24f, 0.24f, 0.25f, 1.00f);
    c[ImGuiCol_TableBorderLight] = ImVec4(0.16f, 0.16f, 0.17f, 1.00f);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.00f, 1.00f, 1.00f, 0.018f);
    c[ImGuiCol_TextSelectedBg] = ImVec4(0.55f, 0.22f, 0.27f, 0.55f);
    c[ImGuiCol_NavCursor] = ImVec4(0.88f, 0.36f, 0.43f, 1.00f);
}

void imgui_dark() { ImGui::StyleColorsDark(); }

struct Theme { const char *name; void (*apply)(); ImVec4 meter_color; };
const std::array themes{
    Theme{"Dark Red", dark_red, ImVec4(0.18f, 0.82f, 0.39f, 1.0f)},
    Theme{"ImGui", imgui_dark, ImVec4(0.90f, 0.70f, 0.00f, 1.0f)},
};
ImVec4 current_meter_color = themes[0].meter_color;

} // namespace

const std::string &ui_theme_names() {
    static const std::string names = [] {
        std::string result;
        for (const auto &theme : themes) {
            if (!result.empty())
                result += '|';
            result += theme.name;
        }
        return result;
    }();
    return names;
}

bool apply_ui_theme(std::string_view name) {
    for (const auto &theme : themes) {
        if (name == theme.name) {
            ImGui::GetStyle() = ImGuiStyle{};
            theme.apply();
            current_meter_color = theme.meter_color;
            return true;
        }
    }
    return false;
}

ImVec4 mixer_meter_color() { return current_meter_color; }

} // namespace srz80::ui
