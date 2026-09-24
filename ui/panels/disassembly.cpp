#include "../gui.hpp"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <limits>
#include <srz80/imgui_input.hpp>
#include <string>
#include <vector>

namespace srz80::ui {
namespace {

size_t disasm_row_for_address(const std::vector<DisasmLine> &lines, uint64_t address) {
    if (lines.empty())
        return 0;
    const auto next = std::upper_bound(lines.begin(), lines.end(), address,
                                       [](uint64_t value, const DisasmLine &line) {
                                           return value < line.address;
                                       });
    return next == lines.begin() ? 0 : static_cast<size_t>(next - lines.begin() - 1);
}

bool disasm_address_in_line(uint64_t address, const DisasmLine &line) {
    if (!line.instruction_bytes || address < line.address)
        return false;
    return address - line.address < line.instruction_bytes;
}

bool disasm_snap_to_cached_instruction(const std::vector<DisasmLine> &lines,
                                       uint64_t &address) {
    if (lines.empty())
        return address == 0;
    const size_t row = disasm_row_for_address(lines, address);
    if (row < lines.size() && disasm_address_in_line(address, lines[row])) {
        address = lines[row].address;
        return true;
    }
    return address == 0;
}

bool is_return_instruction(const std::string &text) {
    if (text.empty())
        return false;
    size_t start = text.find_first_not_of(" \t");
    if (start == std::string::npos)
        return false;
    size_t end = text.find_first_of(" \t,", start);
    std::string mnem = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
    for (char &c : mnem)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return mnem == "ret" || mnem == "reti" || mnem == "retn" ||
           mnem == "rts" || mnem == "rti" || mnem == "rtl" ||
           mnem == "retf" || mnem == "iret" || mnem == "iretd" || mnem == "iretq";
}

} // namespace

void App::disassembly() {
    ImGui::Begin("Disassembly");
    if (!snapshot) {
        ImGui::End();
        return;
    }

    auto cards = snapshot->inspection->cards;
    const char *label = "Select CPU card";
    for (const auto &card : cards) {
        if (card.id == selected)
            label = card.display_name().c_str();
    }
    if (ImGui::BeginCombo("Card", label)) {
        for (const auto &card : cards) {
            auto name = card.display_name() + " #" + std::to_string(card.id);
            if (ImGui::Selectable(name.c_str(), selected == card.id))
                selected = card.id;
        }
        ImGui::EndCombo();
    }

    const auto availability = snapshot->disassembly.find(selected);
    const uint64_t revision = availability == snapshot->disassembly.end() ? 0 : availability->second.revision;
    if (disasm_card != selected || revision != disasm_availability_revision) {
        disasm_cache.clear();
        disasm_history.clear();
        disasm_availability_revision = revision;
    }
    if (availability == snapshot->disassembly.end() || availability->second.state != SRZ_DISASSEMBLY_ENABLED) {
        disasm_cache.clear();
        poll_disassembly(disasm_request_space);
        const auto message = availability == snapshot->disassembly.end() ? std::string{} : availability->second.message;
        const bool disabled = availability != snapshot->disassembly.end() && availability->second.state == SRZ_DISASSEMBLY_DISABLED;
        ImGui::TextWrapped("%s", message.empty() ? (disabled ? "Disassembly disabled by this card." : "This card has no disassembler.") : message.c_str());
        ImGui::End();
        return;
    }

    srz80::Handle memory = find_space("cpu0.memory");
    if (!memory)
        memory = snapshot->inspection->spaces.empty() ? 0 : snapshot->inspection->spaces.begin()->first;
    if (!memory) {
        ImGui::TextUnformatted("No address space is available.");
        ImGui::End();
        return;
    }

    sync_disassembly_memory_revision();
    poll_disassembly(memory);

    uint64_t pc = 0;
    bool have_pc = false;
    if (auto it = snapshot->inspection->properties.find(selected); it != snapshot->inspection->properties.end()) {
        for (auto &p : it->second) {
            if (p.name == "PC" && !(p.ui_flags & SRH_PROPERTY_UNAVAILABLE)) {
                pc = p.value.unsigned_value;
                have_pc = true;
                break;
            }
        }
    }
    if (!have_pc) {
        ImGui::TextUnformatted("Selected card has no PC property.");
        ImGui::End();
        return;
    }

    sync_disassembly_memory_revision();

    const uint64_t maximum = snapshot->inspection->spaces.at(memory).maximum;
    const float row_height = ImGui::GetTextLineHeightWithSpacing();
    // Row highlight bands are kept inside their own text line so they never cover
    // the inter-line gap and bleed into the neighbouring row.
    const float row_band_height = ImGui::GetTextLineHeight() + 2.0f;
    const float row_band_offset = std::max(0.0f, (row_height - row_band_height) * 0.5f);
    const size_t visible_rows = std::max<size_t>(
        1, static_cast<size_t>(std::floor(ImGui::GetContentRegionAvail().y / row_height)));
    const size_t window_rows = std::min<size_t>(1024, std::max<size_t>(128, visible_rows + 64));

    // Toolbar Controls
    const bool previous_follow = disasm_follow_pc;
    ImGui::Checkbox("Follow PC", &disasm_follow_pc);
    if (disasm_follow_pc && !previous_follow) {
        disasm_anchor_known = true;
        disasm_start = pc;
        disasm_cache.clear();
        queue_disassembly(selected, memory, pc, window_rows, DisasmRequestKind::Reset, true, visible_rows / 2);
    }

    ImGui::SameLine();
    if (ImGui::Button("Go to PC")) {
        if (disasm_start != pc) {
            disasm_history.push_back(disasm_start);
            if (disasm_history.size() > 64)
                disasm_history.erase(disasm_history.begin());
        }
        disasm_follow_pc = true;
        disasm_start = pc;
        disasm_cache.clear();
        queue_disassembly(selected, memory, pc, window_rows, DisasmRequestKind::Reset, true, visible_rows / 2);
    }

    ImGui::SameLine();
    const bool can_go_back = !disasm_history.empty();
    if (!can_go_back)
        ImGui::BeginDisabled();
    if (ImGui::Button("Back")) {
        if (can_go_back) {
            uint64_t prev_addr = disasm_history.back();
            disasm_history.pop_back();
            disasm_follow_pc = false;
            disasm_start = prev_addr;
            disasm_cache.clear();
            queue_disassembly(selected, memory, prev_addr, window_rows, DisasmRequestKind::Reset, true, visible_rows / 2);
        }
    }
    if (!can_go_back)
        ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::Checkbox("Hex", &disasm_show_hex);

    ImGui::SameLine();
    if (ImGui::Button("Refresh"))
        disasm_cache.clear();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    if (srz80::gui::input_hexadecimal("Address", disasm_start, 16)) {
        disasm_follow_pc = false;
        if (disasm_start > maximum)
            disasm_start = 0;
        uint64_t snapped = disasm_start;
        if (disasm_anchor_known && disasm_snap_to_cached_instruction(disasm_cache, snapped)) {
            disasm_start = snapped;
            disasm_anchor_known = true;
        } else {
            disasm_anchor_known = (disasm_start == 0);
        }
        disasm_cache.clear();
        queue_disassembly(selected, memory, disasm_start, window_rows, DisasmRequestKind::Reset, disasm_anchor_known, disasm_start > 0 ? 64 : 0);
    }

    // Status line: PC and Run State indicator
    char pc_str[32];
    if (maximum <= 0xFFFF) {
        std::snprintf(pc_str, sizeof(pc_str), "%04llX", static_cast<unsigned long long>(pc));
    } else if (maximum <= 0xFFFFFFFF) {
        std::snprintf(pc_str, sizeof(pc_str), "%08llX", static_cast<unsigned long long>(pc));
    } else {
        std::snprintf(pc_str, sizeof(pc_str), "%016llX", static_cast<unsigned long long>(pc));
    }

    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "PC: ");
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(1.0f, 0.90f, 0.20f, 1.0f), "%s", pc_str);
    ImGui::SameLine();
    if (snapshot->paused()) {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.40f, 1.0f), "[PAUSED]");
    } else {
        ImGui::TextColored(ImVec4(0.35f, 0.90f, 0.45f, 1.0f), "[RUNNING]");
    }

    // Check environment changes (card, space, maximum)
    if (disasm_card != selected || disasm_space != memory || disasm_maximum != maximum) {
        disasm_card = selected;
        disasm_space = memory;
        disasm_maximum = maximum;
        disasm_start = disasm_follow_pc ? pc : 0;
        disasm_anchor_known = true;
        disasm_cache.clear();
        disasm_history.clear();
        queue_disassembly(selected, memory, disasm_start, window_rows, DisasmRequestKind::Reset, true, disasm_start > 0 ? 64 : 0);
    } else if (disasm_cache.empty() && !disasm_request.valid()) {
        const uint64_t target = disasm_follow_pc ? pc : disasm_start;
        queue_disassembly(selected, memory, target, window_rows, DisasmRequestKind::Reset, true, target > 0 ? 64 : 0);
    } else if (disasm_follow_pc && pc != disasm_last_pc) {
        disasm_last_pc = pc;
        // Check if PC is already visible in viewport
        const size_t pc_row = disasm_row_for_address(disasm_cache, pc);
        const bool pc_visible = pc_row >= disasm_view_row && pc_row < disasm_view_row + visible_rows &&
                                disasm_address_in_line(pc, disasm_cache[pc_row]);
        if (!pc_visible) {
            // Auto-center on PC
            if (pc_row < disasm_cache.size() && disasm_address_in_line(pc, disasm_cache[pc_row])) {
                disasm_view_row = pc_row > visible_rows / 2 ? pc_row - visible_rows / 2 : 0;
                disasm_start = disasm_cache[disasm_view_row].address;
            } else if (!disasm_request.valid()) {
                disasm_start = pc;
                queue_disassembly(selected, memory, pc, window_rows, DisasmRequestKind::Reset, true, visible_rows / 2);
            }
        }
    }

    // Instructions view child window
    ImGui::BeginChild("instructions", ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const auto &io = ImGui::GetIO();
    const bool navigation_active =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) && !io.WantTextInput;

    int scroll_rows = 0;
    if (navigation_active) {
        scroll_rows -= static_cast<int>(std::round(io.MouseWheel * 3.0f));
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow, true)) --scroll_rows;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true)) ++scroll_rows;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp, true))
            scroll_rows -= static_cast<int>(visible_rows);
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown, true))
            scroll_rows += static_cast<int>(visible_rows);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
            disasm_follow_pc = false;
            disasm_start = 0;
            disasm_cache.clear();
            queue_disassembly(selected, memory, 0, window_rows, DisasmRequestKind::Reset, true, 0);
        }
    }

    // Handle smooth line-by-line scrolling without jumping to 0x0000
    if (scroll_rows != 0 && !disasm_cache.empty()) {
        disasm_follow_pc = false;
        if (scroll_rows > 0) {
            // Scrolling down
            const size_t max_row = disasm_cache.size() > visible_rows ? disasm_cache.size() - visible_rows : 0;
            disasm_view_row = std::min<size_t>(disasm_view_row + static_cast<size_t>(scroll_rows), max_row);
            disasm_start = disasm_cache[disasm_view_row].address;
        } else {
            // Scrolling up
            const size_t move_up = static_cast<size_t>(-scroll_rows);
            if (disasm_view_row >= move_up) {
                disasm_view_row -= move_up;
            } else {
                disasm_view_row = 0;
            }
            disasm_start = disasm_cache[disasm_view_row].address;
        }
    }

    // Pre-fetch earlier instructions (scrolling up)
    if (!disasm_cache.empty() && disasm_view_row < 24 && disasm_cache.front().address > 0 && !disasm_request.valid()) {
        queue_disassembly(selected, memory, disasm_cache.front().address, 0, DisasmRequestKind::Prepend, true, 64);
    }

    // Pre-fetch later instructions (scrolling down)
    if (!disasm_cache.empty() && disasm_view_row + visible_rows + 24 > disasm_cache.size() && !disasm_request.valid()) {
        const auto &last = disasm_cache.back();
        if (last.address <= maximum - last.instruction_bytes) {
            const uint64_t next_addr = last.address + last.instruction_bytes;
            if (next_addr <= maximum)
                queue_disassembly(selected, memory, next_addr, 64, DisasmRequestKind::Append, true, 0);
        }
    }

    // Bound cache memory
    constexpr size_t max_cached_lines = 2048;
    if (disasm_cache.size() > max_cached_lines) {
        if (disasm_view_row > max_cached_lines / 2) {
            const size_t trim_count = disasm_view_row - max_cached_lines / 4;
            disasm_cache.erase(disasm_cache.begin(), disasm_cache.begin() + trim_count);
            disasm_view_row -= trim_count;
        } else if (disasm_cache.size() > disasm_view_row + max_cached_lines / 2) {
            disasm_cache.resize(disasm_view_row + max_cached_lines / 2);
        }
    }

    const auto find_breakpoint = [&](uint64_t addr) -> const UiBreakpoint * {
        if (!snapshot || !snapshot->inspection) return nullptr;
        for (const auto &bp : snapshot->inspection->breakpoints) {
            if ((bp.space == 0 || bp.space == memory) &&
                (bp.card == 0 || bp.card == selected) &&
                bp.first <= addr && addr <= bp.last) {
                return &bp;
            }
        }
        return nullptr;
    };

    const size_t display_end = std::min(disasm_view_row + visible_rows, disasm_cache.size());

    for (size_t row = disasm_view_row; row < display_end; ++row) {
        const auto &line = disasm_cache[row];
        if (!line.instruction_bytes)
            continue;

        const bool is_pc = disasm_address_in_line(pc, line);
        const auto *bp = find_breakpoint(line.address);
        const bool has_bp = (bp != nullptr);
        const bool bp_enabled = has_bp && bp->enabled;

        ImGui::PushID(static_cast<int>(row));

        // Format address string according to address space width
        char addr_text[32];
        if (maximum <= 0xFFFF) {
            std::snprintf(addr_text, sizeof(addr_text), "%04llX", static_cast<unsigned long long>(line.address));
        } else if (maximum <= 0xFFFFFFFF) {
            std::snprintf(addr_text, sizeof(addr_text), "%08llX", static_cast<unsigned long long>(line.address));
        } else {
            std::snprintf(addr_text, sizeof(addr_text), "%016llX", static_cast<unsigned long long>(line.address));
        }

        const ImVec2 row_min = ImGui::GetCursorScreenPos();
        const float row_avail_width = ImGui::GetContentRegionAvail().x;
        const ImVec2 row_max = ImVec2(row_min.x + row_avail_width, row_min.y + row_height);
        const ImVec2 row_band_min(row_min.x, row_min.y + row_band_offset);
        const ImVec2 row_band_max(row_max.x, row_band_min.y + row_band_height);

        // 1. Draw row Selectable FIRST so it owns the full hit area. Its own frame
        //    background is disabled so only the narrower row band paints below.
        const bool is_selected = (disasm_selected_address == line.address);
        ImGui::PushStyleColor(ImGuiCol_Header, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, IM_COL32(0, 0, 0, 0));
        ImGui::Selectable("##row", is_selected, ImGuiSelectableFlags_None, ImVec2(row_avail_width, row_height));
        ImGui::PopStyleColor(3);
        // Hit-test the exact, non-overlapping row rect. Selectable() pads its box by
        // half the item spacing, which makes neighbouring rows overlap so two rows can
        // report hover at a shared boundary; testing row_min..row_max keeps it single.
        const bool row_hovered = ImGui::IsMouseHoveringRect(row_min, row_max) && !io.WantTextInput;

        if (row_hovered) {
            if (ImGui::IsMouseClicked(0)) {
                if (io.MousePos.x - row_min.x < 28.0f) {
                    if (has_bp) {
                        controller.remove_breakpoint(bp->id, snapshot->generation);
                    } else {
                        controller.add_breakpoint(selected, memory, line.address, line.address, 0, snapshot->generation);
                    }
                    error.clear();
                } else {
                    disasm_selected_address = line.address;
                }
            }
        }

        if (ImGui::BeginPopupContextItem()) {
            if (ImGui::MenuItem(has_bp ? "Remove Breakpoint" : "Add Breakpoint")) {
                if (has_bp) {
                    controller.remove_breakpoint(bp->id, snapshot->generation);
                } else {
                    controller.add_breakpoint(selected, memory, line.address, line.address, 0, snapshot->generation);
                }
            }
            if (ImGui::MenuItem("Go to Address")) {
                disasm_history.push_back(disasm_start);
                disasm_start = line.address;
                disasm_follow_pc = false;
                disasm_cache.clear();
                queue_disassembly(selected, memory, line.address, window_rows, DisasmRequestKind::Reset, true, visible_rows / 2);
            }
            if (ImGui::MenuItem("Copy Address")) {
                ImGui::SetClipboardText(addr_text);
            }
            if (ImGui::MenuItem("Copy Instruction")) {
                ImGui::SetClipboardText(line.text.c_str());
            }
            ImGui::EndPopup();
        }

        // 2. Single row band for hover, selection, PC and breakpoint states
        ImU32 row_band_color = 0;
        if (is_pc && has_bp) {
            row_band_color = ImColor(125, 55, 20, row_hovered ? 80 : 130);
        } else if (is_pc) {
            row_band_color = ImColor(100, 85, 20, row_hovered ? 70 : 110);
        } else if (has_bp) {
            row_band_color = ImColor(115, 25, 30, row_hovered ? 70 : 110);
        } else if (row_hovered) {
            row_band_color = ImGui::GetColorU32(ImGuiCol_HeaderHovered);
        } else if (is_selected) {
            row_band_color = ImGui::GetColorU32(ImGuiCol_Header);
        }
        if (row_band_color != 0)
            ImGui::GetWindowDrawList()->AddRectFilled(row_band_min, row_band_max, row_band_color);

        // 3. Move cursor back to row_min to draw text columns ON TOP of backgrounds
        ImGui::SetCursorScreenPos(ImVec2(row_min.x + 4.0f, row_min.y + 1.0f));

        // Column 1: Breakpoint indicator
        if (has_bp) {
            ImGui::TextColored(bp_enabled ? ImVec4(0.95f, 0.25f, 0.25f, 1.0f) : ImVec4(0.55f, 0.55f, 0.55f, 1.0f), "%s", bp_enabled ? "●" : "○");
        } else if (row_hovered && (io.MousePos.x - row_min.x < 28.0f)) {
            ImGui::TextColored(ImVec4(0.60f, 0.30f, 0.32f, 1.0f), "·");
        } else {
            ImGui::TextUnformatted(" ");
        }

        // Column 2: Address (in cyan)
        ImGui::SameLine(28.0f);
        ImGui::TextColored(ImVec4(0.35f, 0.82f, 0.92f, 1.0f), "%s", addr_text);

        // Column 3: PC Cursor indicator
        ImGui::SameLine();
        if (is_pc) {
            ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.20f, 1.0f), " ->");
        } else {
            ImGui::TextUnformatted("   ");
        }

        // Column 4: Instruction text
        ImGui::SameLine();
        if (is_pc) {
            ImGui::TextColored(ImVec4(1.0f, 0.90f, 0.20f, 1.0f), "%s", line.ok ? line.text.c_str() : "-- (peek unavailable)");
        } else if (has_bp) {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f), "%s", line.ok ? line.text.c_str() : "-- (peek unavailable)");
        } else {
            if (line.ok) {
                ImGui::TextUnformatted(line.text.c_str());
            } else {
                ImGui::TextDisabled("-- (peek unavailable)");
            }
        }

        // Column 5: Hex bytes in comment style (if enabled)
        if (disasm_show_hex && line.ok && !line.bytes.empty()) {
            std::string bytes_text = "; ";
            for (size_t b = 0; b < line.bytes.size() && b < 6; ++b) {
                char h[8];
                std::snprintf(h, sizeof(h), "%02X ", line.bytes[b]);
                bytes_text += h;
            }
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.50f, 0.50f, 0.52f, 1.0f), "%-20s", bytes_text.c_str());
        }

        // Column 6: Cycle count
        if (line.cycles > 0) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.48f, 0.48f, 0.50f, 1.0f), "[%u T]", line.cycles);
        }

        // Subroutine separator line (Gearboy-style)
        if (line.ok && is_return_instruction(line.text)) {
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(row_min.x, row_min.y + row_height - 1.0f),
                ImVec2(row_max.x, row_min.y + row_height - 1.0f),
                ImColor(50, 100, 55, 160), 1.0f);
        }

        // 4. Advance cursor to next row
        ImGui::SetCursorScreenPos(ImVec2(row_min.x, row_min.y + row_height));

        ImGui::PopID();
    }

    ImGui::EndChild();
    ImGui::End();
}

} // namespace srz80::ui
