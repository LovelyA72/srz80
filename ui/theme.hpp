#pragma once

#include <string>
#include <string_view>
#include <imgui.h>

namespace srz80::ui {

const std::string &ui_theme_names();
bool apply_ui_theme(std::string_view name);
ImVec4 mixer_meter_color();

} // namespace srz80::ui
