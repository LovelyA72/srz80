#include "../gui.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <imgui_internal.h>
#include <string>
#include <vector>

namespace srz80::ui {

int App::console_input_callback(ImGuiInputTextCallbackData *data) {
    auto *app = static_cast<App *>(data->UserData);
    if (data->EventFlag != ImGuiInputTextFlags_CallbackEdit || !app->console_live_mode ||
        data->BufTextLen == 0)
        return 0;

    std::string input(data->Buf, static_cast<size_t>(data->BufTextLen));
    if (app->send_console_input(input.c_str()))
        data->DeleteChars(0, data->BufTextLen);
    return 0;
}

srz80::Handle App::console_card() const {
    if (!snapshot)
        return 0;
    if (console_text_card &&
        std::any_of(snapshot->inspection->text_endpoints.begin(), snapshot->inspection->text_endpoints.end(),
                    [&](const auto &endpoint) { return endpoint.card == console_text_card; }))
        return console_text_card;
    return snapshot->inspection->text_endpoints.empty() ? 0 : snapshot->inspection->text_endpoints.front().card;
}

std::string App::escape_transcript_byte(uint8_t value) {
    if (value >= 32 && value < 127)
        return std::string(1, static_cast<char>(value));
    switch (value) {
    case '\n': return "\n";
    case '\r': return "\\r";
    case '\b': return "\\b";
    default: {
        char buffer[8];
        std::snprintf(buffer, sizeof(buffer), "\\x%02X", value);
        return buffer;
    }
    }
}

bool App::send_console_input(const char *text) {
    bool bad = false;
    for (const char *p = text; *p; ++p)
        if (static_cast<unsigned char>(*p) >= 0x80)
            bad = true;
    if (bad) {
        error = "Console input supports ASCII only; non-ASCII byte rejected.";
        return false;
    }
    if (!snapshot)
        return false;
    const auto card = console_card();
    const auto endpoint = std::find_if(snapshot->inspection->text_endpoints.begin(),
                                       snapshot->inspection->text_endpoints.end(),
                                       [&](const auto &candidate) {
                                           return candidate.card == card;
                                       });
    if (endpoint == snapshot->inspection->text_endpoints.end() || endpoint->inputs.empty()) {
        error = "The selected text provider has no input endpoint.";
        return false;
    }
    for (const char *p = text; *p; ++p)
        controller.enqueue_input(endpoint->inputs.front(), snapshot->now,
                                 static_cast<uint8_t>(*p), snapshot->generation);
    invalidate_console_text();
    error.clear();
    return true;
}

void App::copy_console_text() {
    srz80::Handle card = console_card();
    if (!card)
        return;
    clipboard_is_memory = false;
    clipboard_request = controller.read_text_async(card, snapshot->generation);
}

void App::console() {
    ImGui::Begin("Console");
    if (!snapshot) {
        ImGui::End();
        return;
    }
    srz80::Handle provider = console_card();
    if (!provider) {
        ImGui::TextUnformatted("No text-provider card is loaded.");
        ImGui::End();
        return;
    }
    auto cards = snapshot->inspection->cards;
    std::string label = "Text provider";
    for (const auto &card : cards)
        if (card.id == provider)
            label = card.display_name() + " #" + std::to_string(card.id);
    if (ImGui::BeginCombo("Provider", label.c_str())) {
        for (const auto &endpoint : snapshot->inspection->text_endpoints)
            for (const auto &card : cards)
                if (card.id == endpoint.card) {
                auto name = card.display_name() + " #" + std::to_string(card.id);
                if (ImGui::Selectable(name.c_str(), provider == card.id)) {
                    provider = card.id;
                    console_text_card = provider;
                    invalidate_console_text();
                }
            }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::Checkbox("Autoscroll", &console_autoscroll);
    if (focus_console_input) {
        ImGui::SetKeyboardFocusHere();
        focus_console_input = false;
    }
    if (ImGui::InputText("ASCII input", console_input, sizeof(console_input),
                         ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_CallbackEdit,
                         App::console_input_callback, this)) {
        send_console_input(console_input);
        // Treat Enter as the terminal key it represents, rather than only as
        // a GUI submit action. This lets line-oriented UART firmware commit a
        // command without requiring a literal control character in the field.
        send_console_input("\r");
        console_input[0] = 0;
        focus_console_input = true;
    }
    note_text_input(console_input, sizeof(console_input));
    ImGui::SameLine();
    ImGui::Checkbox("Live mode", &console_live_mode);
    ImGui::SameLine();
    if (ImGui::Button("Send") && console_input[0]) {
        send_console_input(console_input);
        console_input[0] = 0;
        focus_console_input = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy visible text"))
        copy_console_text();

    console_text_card = provider;
    refresh_console_text();
    const auto &all = console_text_cache;
    ImGui::BeginChild("transcript", ImVec2(0, 0), true);
    if (console_autoscroll) {
        // A deliberate scroll gesture means the user wants to read away from
        // the tail; stop following so autoscroll does not fight the input.
        ImGuiWindow *transcript = ImGui::GetCurrentWindow();
        const bool scrollbar_held =
            ImGui::GetActiveID() == ImGui::GetWindowScrollbarID(transcript, ImGuiAxis_Y);
        const bool wheeled = ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f;
        const bool keyed = ImGui::IsWindowFocused() &&
                           (ImGui::IsKeyPressed(ImGuiKey_PageUp) ||
                            ImGui::IsKeyPressed(ImGuiKey_PageDown) ||
                            ImGui::IsKeyPressed(ImGuiKey_Home) ||
                            ImGui::IsKeyPressed(ImGuiKey_End));
        if (scrollbar_held || wheeled || keyed)
            console_autoscroll = false;
    }
    std::vector<std::pair<size_t, size_t>> lines;
    size_t start = 0;
    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] != '\r' && all[i] != '\n')
            continue;
        lines.emplace_back(start, i);
        if (all[i] == '\r' && i + 1 < all.size() && all[i + 1] == '\n')
            ++i;
        start = i + 1;
    }
    lines.emplace_back(start, all.size());
    ImGuiListClipper clip;
    clip.Begin(static_cast<int>(lines.size()));
    while (clip.Step())
        for (int row = clip.DisplayStart; row < clip.DisplayEnd; ++row) {
            const auto [line_start, line_end] = lines[static_cast<size_t>(row)];
            std::string line;
            for (size_t i = line_start; i < line_end; ++i)
                line += escape_transcript_byte(all[i]);
            ImGui::TextUnformatted(line.empty() ? " " : line.c_str());
        }
    if (console_autoscroll)
        ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
    ImGui::End();
}

} // namespace srz80::ui
