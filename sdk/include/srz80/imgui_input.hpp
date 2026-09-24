#pragma once

// Shared hexadecimal editor for the host GUI and GUI tools.  It accepts the
// conventional optional 0x prefix, then normalizes committed values to 0x.
#include <algorithm>
#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <imgui.h>
#include <cstdio>
#include <limits>
#include <string_view>

namespace srz80::gui {

// Call immediately after an ImGui value control. This is deliberately opt-in:
// middle-clicking controls without an explicit reset value remains unchanged.
template <typename T>
requires std::equality_comparable<T>
bool reset_on_middle_click(T &value, const T &reset_value) {
    if (!ImGui::IsItemClicked(ImGuiMouseButton_Middle) || value == reset_value)
        return false;
    value = reset_value;
    return true;
}

template <std::unsigned_integral T>
bool input_hexadecimal(const char *label, T &value, uint32_t bits = sizeof(T) * 8u) {
    const auto digits = std::clamp(bits / 4u + (bits % 4u != 0), 1u, 16u);
    std::array<char, 19> text{}; // 0x + 16 hexadecimal digits + NUL
    std::snprintf(text.data(), text.size(), "0x%0*llX", static_cast<int>(digits),
                  static_cast<unsigned long long>(value));

    const auto flags = ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_CharsUppercase |
                       ImGuiInputTextFlags_EnterReturnsTrue;
    bool commit = ImGui::InputText(label, text.data(), text.size(), flags);
    commit = commit || ImGui::IsItemDeactivatedAfterEdit();
    if (!commit)
        return false;

    std::string_view source{text.data()};
    if (source.starts_with("0x"))
        source.remove_prefix(2);
    uint64_t parsed = 0;
    const auto result = std::from_chars(source.data(), source.data() + source.size(), parsed, 16);
    if (source.empty() || result.ec != std::errc{} || result.ptr != source.data() + source.size() ||
        parsed > std::numeric_limits<T>::max())
        return false;
    const auto updated = static_cast<T>(parsed);
    if (updated == value)
        return false;
    value = updated;
    return true;
}

} // namespace srz80::gui
