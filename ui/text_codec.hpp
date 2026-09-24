#pragma once
#include <string>

namespace srz80::ui {
// Small text codecs used by host persistence.  The GUI stores its serialized
// ImGui layout as a base64 value under the "imgui" configuration key, and
// layout export/import writes the same encoding.  These helpers are free of
// SDL, ImGui and engine dependencies.
std::string base64_encode(const std::string &input);
std::string base64_decode(const std::string &input);
} // namespace srz80::ui
