#include "../gui.hpp"
#include <algorithm>

namespace srz80::ui {

void App::breakpoints() {
    ImGui::Begin("Breakpoints");
    if (!snapshot) {
        ImGui::End();
        return;
    }
    select_space("Space");
    const char *kinds[]{"Execution (selected card)", "Read", "Write", "Read or write"};
    ImGui::Combo("Kind", &bp_type, kinds, 4);
    hex_input("First", bp_first);
    hex_input("Last", bp_last);
    if (ImGui::Button("Add breakpoint")) {
        controller.add_breakpoint(bp_type == 0 ? selected : 0, space, bp_first, bp_last,
                                  static_cast<uint32_t>(bp_type), snapshot->generation);
        error.clear();
    }
    ImGui::TextWrapped("%s", snapshot->stop_reason.c_str());
    uint64_t remove = 0;
    uint64_t toggle = 0;
    bool toggle_value = false;
    for (const auto &b : snapshot->inspection->breakpoints) {
        ImGui::PushID(static_cast<int>(b.id));
        bool enabled = b.enabled;
        if (ImGui::Checkbox("Enabled", &enabled)) {
            toggle = b.id;
            toggle_value = enabled;
        }
        ImGui::SameLine();
        ImGui::Text("%s | card %llu space %llu | %llX..%llX", kinds[b.operations],
                    static_cast<unsigned long long>(b.card), static_cast<unsigned long long>(b.space),
                    static_cast<unsigned long long>(b.first), static_cast<unsigned long long>(b.last));
        ImGui::SameLine();
        if (ImGui::SmallButton("Delete"))
            remove = b.id;
        ImGui::PopID();
    }
    if (remove)
        controller.remove_breakpoint(remove, snapshot->generation);
    if (toggle)
        controller.set_breakpoint_enabled(toggle, toggle_value, snapshot->generation);
    ImGui::End();
}

} // namespace srz80::ui
