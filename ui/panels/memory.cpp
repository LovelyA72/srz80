#include "../gui.hpp"
#include "../../third_party/imgui_memory_editor.h"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <srz80/imgui_input.hpp>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace srz80::ui {
namespace {

// The editor is presentation-only: its callbacks keep all reads and writes on
// the controller path, so inspecting memory cannot bypass the rack bus.
MemoryEditor memory_editor = [] {
    MemoryEditor editor;
    editor.OptShowDataPreview = true;
    return editor;
}();
size_t memory_editor_selection = static_cast<size_t>(-1);

} // namespace

ImU8 App::memory_editor_read(const ImU8 *, size_t offset, void *user_data) {
    const auto *app = static_cast<const App *>(user_data);
    if (offset >= app->memory_cache.size())
        return 0;
    return app->memory_cache[offset].value;
}

void App::memory_editor_write(ImU8 *, size_t offset, ImU8 value, void *user_data) {
    auto *app = static_cast<App *>(user_data);
    if (!app->snapshot || !app->space || offset >= app->memory_cache.size() ||
        app->memory_base > UINT64_MAX - offset) {
        app->error = "Memory write is outside the displayed range";
        return;
    }
    const uint64_t address = app->memory_base + offset;
    if (!app->snapshot->inspection->spaces.contains(app->space) ||
        address > app->snapshot->inspection->spaces.at(app->space).maximum) {
        app->error = "Memory write exceeds the address space";
        return;
    }

    std::string write_error;
    const auto status = app->controller.request_write_memory(app->space, address, {value},
                                                             write_error, app->snapshot->generation);
    if (status != SRH_OK) {
        app->error = write_error.empty() ? "Memory write rejected" : write_error;
        return;
    }

    app->memory_cache[offset] = {value, SRH_OK};
    app->memory_cache_space = 0; // Refresh through the bus after this frame.
    app->memory_selection_space = app->space;
    app->memory_selection_anchor = app->memory_selection_cursor = address;
    app->memory_has_selection = true;
    memory_editor_selection = offset;
    app->error.clear();
}

ImU32 App::memory_editor_background(const ImU8 *, size_t offset, void *user_data) {
    const auto *app = static_cast<const App *>(user_data);
    if (offset < app->memory_cache.size() && !app->memory_cache[offset].available())
        return IM_COL32(140, 80, 45, 90);
    return 0;
}

namespace {

std::vector<uint8_t> parse_hex_bytes(std::string text) {
    for (char &character : text)
        if (character == ',' || character == ';')
            character = ' ';
    std::istringstream input(text);
    std::vector<uint8_t> result;
    std::string token;
    while (input >> token) {
        if (token.starts_with("0x") || token.starts_with("0X"))
            token.erase(0, 2);
        if (token.empty() || token.size() % 2)
            throw std::invalid_argument("Paste data must contain complete hexadecimal bytes");
        for (size_t offset = 0; offset < token.size(); offset += 2) {
            unsigned value = 0;
            const auto *first = token.data() + offset;
            auto converted = std::from_chars(first, first + 2, value, 16);
            if (converted.ec != std::errc{} || converted.ptr != first + 2)
                throw std::invalid_argument("Paste data contains a non-hexadecimal byte");
            result.push_back(static_cast<uint8_t>(value));
        }
    }
    if (result.empty())
        throw std::invalid_argument("Clipboard contains no hexadecimal bytes");
    return result;
}

} // namespace

void App::memory() {
    if (!ImGui::Begin("Memory inspector")) {
        ImGui::End();
        return;
    }
    if (!snapshot) {
        ImGui::End();
        return;
    }
    const auto previous_space = space;
    // Keep the inspector's navigation controls on one compact row; the byte
    // table should remain the dominant part of this panel.
    ImGui::SetNextItemWidth(170.0f);
    select_space("##memory_space");
    ImGui::SameLine();
    ImGui::TextDisabled("space");
    if (space != previous_space) {
        memory_has_selection = false;
        memory_drag_selecting = false;
        memory_cache_space = 0;
        memory_editor.DataEditingAddr = memory_editor.DataPreviewAddr = static_cast<size_t>(-1);
        memory_editor.HighlightMin = memory_editor.HighlightMax = static_cast<size_t>(-1);
        memory_editor_selection = static_cast<size_t>(-1);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("base");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150.0f);
    hex_input("##memory_base", memory_base);
    ImGui::SameLine();
    ImGui::TextDisabled("length");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(170.0f);
    if (ImGui::SliderInt("##memory_length", &memory_length, 16, 4096))
        memory_cache_length = 0;

    auto selection_first = std::min(memory_selection_anchor, memory_selection_cursor);
    auto selection_last = std::max(memory_selection_anchor, memory_selection_cursor);
    auto selection_size = memory_has_selection && memory_selection_space == space
                              ? selection_last - selection_first + 1
                              : uint64_t{0};
    const bool editable = snapshot->run_state != UiRunState::running;

    auto copy_selection = [&] {
        if (!selection_size)
            return;
        if (selection_size > 1024 * 1024) {
            error = "Selection exceeds the 1 MiB copy limit";
            return;
        }
        clipboard_is_memory = true;
        clipboard_size = selection_size;
        clipboard_request = controller.read_memory_async(memory_selection_space, selection_first,
            static_cast<uint32_t>(selection_size), snapshot->generation);
    };
    auto write_bytes = [&](uint64_t first, const std::vector<uint8_t> &bytes) {
        if (!editable)
            return;
        if (!space || !snapshot->inspection->spaces.contains(space))
            throw std::runtime_error("Select an address space before writing memory");
        const auto maximum = snapshot->inspection->spaces.at(space).maximum;
        if (bytes.empty() || first > maximum || bytes.size() - 1u > maximum - first)
            throw std::invalid_argument("Memory write exceeds the address space");
        std::string write_error;
        auto status = controller.request_write_memory(space, first, bytes, write_error, snapshot->generation);
        if (status != SRH_OK)
            throw std::runtime_error(write_error.empty() ? "Memory write rejected"
                                                         : write_error);
        memory_selection_space = space;
        memory_selection_anchor = first;
        memory_selection_cursor = first + bytes.size() - 1u;
        memory_has_selection = true;
        memory_cache_space = 0; // force refresh after mutation
        error.clear();
    };
    auto paste_selection = [&] {
        const char *clipboard = ImGui::GetClipboardText();
        if (!clipboard)
            throw std::invalid_argument("Clipboard is empty");
        write_bytes(selection_size ? selection_first : memory_base, parse_hex_bytes(clipboard));
    };
    auto fill_selection = [&] {
        if (!selection_size)
            return;
        write_bytes(selection_first,
                    std::vector<uint8_t>(static_cast<size_t>(selection_size), memory_fill_value));
    };

    ImGui::TextDisabled("Selection: %s", selection_size
                                     ? (std::to_string(selection_first) + " - " +
                                        std::to_string(selection_last) + " (" +
                                        std::to_string(selection_size) + " bytes)")
                                           .c_str()
                                     : "none");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drag across bytes, or Shift-click, to select a range");
    ImGui::BeginDisabled(!selection_size);
    if (ImGui::Button("Copy"))
        copy_selection();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!editable);
    if (ImGui::Button("Paste")) {
        try {
            paste_selection();
        } catch (const std::exception &exception) {
            error = exception.what();
        }
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(45.0f);
    srz80::gui::input_hexadecimal("##fill_value", memory_fill_value);
    ImGui::SameLine();
    ImGui::BeginDisabled(!selection_size);
    if (ImGui::Button("Fill selection")) {
        try {
            fill_selection();
        } catch (const std::exception &exception) {
            error = exception.what();
        }
    }
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Clear selection"))
        memory_has_selection = false;

    if (!editable) {
        ImGui::SameLine();
        ImGui::TextDisabled("Pause to edit");
    }

    const bool memory_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    auto &io = ImGui::GetIO();
    if (memory_focused && io.KeyCtrl && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_A, false) && space && memory_length > 0 &&
            memory_base <= snapshot->inspection->spaces.at(space).maximum) {
            memory_selection_space = space;
            memory_selection_anchor = memory_base;
            memory_selection_cursor =
                memory_base + std::min<uint64_t>(static_cast<uint64_t>(memory_length - 1),
                                                  snapshot->inspection->spaces.at(space).maximum - memory_base);
            memory_has_selection = true;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_C, false))
            copy_selection();
        if (editable && ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            try {
                paste_selection();
            } catch (const std::exception &exception) {
                error = exception.what();
            }
        }
    }

    if (space) {
        refresh_memory_cache();
        if (!memory_cache.empty()) {
            memory_editor.ReadOnly = !editable;
            memory_editor.Cols = memory_editor_columns;
            memory_editor.OptShowDataPreview = memory_editor_show_preview;
            memory_editor.OptShowHexII = memory_editor_show_hexii;
            memory_editor.OptShowAscii = memory_editor_show_ascii;
            memory_editor.OptGreyOutZeroes = memory_editor_grey_zeroes;
            memory_editor.OptUpperCaseHex = memory_editor_uppercase_hex;
            memory_editor.ReadFn = App::memory_editor_read;
            memory_editor.WriteFn = App::memory_editor_write;
            memory_editor.BgColorFn = App::memory_editor_background;
            memory_editor.UserData = this;

            if (memory_has_selection && memory_selection_space == space && selection_last >= memory_base) {
                const auto first = selection_first > memory_base ? selection_first - memory_base : 0;
                const auto last = selection_last - memory_base;
                memory_editor.HighlightMin = static_cast<size_t>(std::min<uint64_t>(first, memory_cache.size()));
                memory_editor.HighlightMax = static_cast<size_t>(
                    last < memory_cache.size() ? last + 1 : memory_cache.size());
            } else {
                memory_editor.HighlightMin = memory_editor.HighlightMax = static_cast<size_t>(-1);
            }

            const bool show_options = memory_editor.OptShowOptions;
            const bool show_preview = memory_editor.OptShowDataPreview;
            memory_editor.OptShowOptions = false;
            memory_editor.OptShowDataPreview = false;

            if (ImGui::BeginTable("memory-inspector-layout", 2,
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV,
                                  ImVec2(0.0f, -FLT_MIN))) {
                ImGui::TableSetupColumn("Memory", ImGuiTableColumnFlags_WidthStretch, 4.0f);
                ImGui::TableSetupColumn("Details", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                memory_editor.DrawContents(memory_cache.data(), memory_cache.size(), memory_base);

                // MemoryEditor exposes the byte beneath the mouse, but leaves
                // range selection to its host. Keep its normal click-to-edit
                // behavior while using a drag (or Shift-click) to extend the
                // inspector selection used by Copy, Fill, and Clear.
                if (memory_editor.MouseHovered &&
                    memory_editor.MouseHoveredAddr < memory_cache.size() &&
                    memory_base <= UINT64_MAX - memory_editor.MouseHoveredAddr) {
                    const auto address = memory_base + memory_editor.MouseHoveredAddr;
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (!ImGui::GetIO().KeyShift || !memory_has_selection ||
                            memory_selection_space != space)
                            memory_selection_anchor = address;
                        memory_selection_cursor = address;
                        memory_selection_space = space;
                        memory_has_selection = true;
                        memory_drag_selecting = true;
                        memory_editor_selection = memory_editor.MouseHoveredAddr;
                    } else if (memory_drag_selecting &&
                               (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                                ImGui::IsMouseReleased(ImGuiMouseButton_Left))) {
                        memory_selection_cursor = address;
                    }
                }
                if (!ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    memory_drag_selecting = false;

                memory_editor.OptShowOptions = show_options;
                memory_editor.OptShowDataPreview = show_preview;

                ImGui::TableNextColumn();
                ImGui::BeginChild("memory-details", ImVec2(0.0f, -FLT_MIN), false);
                if (memory_editor.OptShowDataPreview) {
                    ImGui::TextDisabled("PREVIEW");
                    MemoryEditor::Sizes sizes;
                    memory_editor.CalcSizes(sizes, memory_cache.size(), memory_base);
                    memory_editor.DrawPreviewLine(sizes, memory_cache.data(), memory_cache.size(),
                                                  memory_base);
                    ImGui::Separator();
                }

                ImGui::TextDisabled("DISPLAY");
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::SliderInt("Columns", &memory_editor.Cols, 4, 32))
                    memory_editor.ContentsWidthChanged = true;
                ImGui::Checkbox("Data preview", &memory_editor.OptShowDataPreview);
                ImGui::Checkbox("HexII", &memory_editor.OptShowHexII);
                if (ImGui::Checkbox("ASCII", &memory_editor.OptShowAscii))
                    memory_editor.ContentsWidthChanged = true;
                ImGui::Checkbox("Dim zeroes", &memory_editor.OptGreyOutZeroes);
                ImGui::Checkbox("Uppercase hex", &memory_editor.OptUpperCaseHex);

                ImGui::Separator();
                ImGui::TextDisabled("NAVIGATION");
                ImGui::Text("Range %llX..%llX",
                            static_cast<unsigned long long>(memory_base),
                            static_cast<unsigned long long>(memory_base + memory_cache.size() - 1));
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputTextWithHint("##memory-goto", "Go to address",
                                             memory_editor.AddrInputBuf,
                                             IM_ARRAYSIZE(memory_editor.AddrInputBuf),
                                             ImGuiInputTextFlags_CharsHexadecimal |
                                                 ImGuiInputTextFlags_EnterReturnsTrue)) {
                    uint64_t address = 0;
                    const auto *begin = memory_editor.AddrInputBuf;
                    const auto *end = begin + std::strlen(begin);
                    const auto converted = std::from_chars(begin, end, address, 16);
                    if (converted.ec == std::errc{} && converted.ptr == end && address >= memory_base)
                        memory_editor.GotoAddr = static_cast<size_t>(address - memory_base);
                }

                ImGui::EndChild();
                ImGui::EndTable();
            } else {
                memory_editor.OptShowOptions = show_options;
                memory_editor.OptShowDataPreview = show_preview;
            }

            memory_editor_columns = memory_editor.Cols;
            memory_editor_show_preview = memory_editor.OptShowDataPreview;
            memory_editor_show_hexii = memory_editor.OptShowHexII;
            memory_editor_show_ascii = memory_editor.OptShowAscii;
            memory_editor_grey_zeroes = memory_editor.OptGreyOutZeroes;
            memory_editor_uppercase_hex = memory_editor.OptUpperCaseHex;

            if (memory_editor.DataEditingAddr < memory_cache.size() &&
                memory_editor.DataEditingAddr != memory_editor_selection &&
                memory_base <= UINT64_MAX - memory_editor.DataEditingAddr) {
                const auto address = memory_base + memory_editor.DataEditingAddr;
                memory_selection_space = space;
                memory_selection_anchor = memory_selection_cursor = address;
                memory_has_selection = true;
                memory_editor_selection = memory_editor.DataEditingAddr;
            }
        }
    }
    ImGui::End();
}

} // namespace srz80::ui
