#include "../gui.hpp"
#include <algorithm>
#include <string>

namespace srz80::ui {

namespace {
std::vector<std::filesystem::path> project_rom_paths(const ProjectSession &session) {
    std::vector<std::filesystem::path> result;
    if (!session.loaded() || !session.document().contains("cards") || !session.document()["cards"].is_array())
        return result;
    const auto root = session.path().empty() ? std::filesystem::path{} : session.path().parent_path();
    for (const auto &card : session.document()["cards"]) {
        auto add = [&](const nlohmann::json &value) {
            if (!value.is_string() || value.get_ref<const std::string &>().empty()) return;
            auto path = std::filesystem::path(value.get<std::string>());
            result.push_back(path.is_relative() ? (root / path).lexically_normal() : path.lexically_normal());
        };
        if (card.is_object() && card.contains("image")) add(card["image"]);
        if (card.is_object() && card.contains("images") && card["images"].is_array())
            for (const auto &value : card["images"]) add(value);
    }
    return result;
}
}

void App::remember_rom_file_states() {
    rom_file_states.clear();
    for (const auto &path : project_rom_paths(project_session)) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) continue;
        const auto time = std::filesystem::last_write_time(path, ec);
        if (!ec) rom_file_states[path] = {size, time};
    }
}

bool App::rom_files_changed() const {
    for (const auto &path : project_rom_paths(project_session)) {
        std::error_code ec;
        const auto size = std::filesystem::file_size(path, ec);
        if (ec) return rom_file_states.contains(path);
        const auto time = std::filesystem::last_write_time(path, ec);
        const auto previous = rom_file_states.find(path);
        if (!ec && (previous == rom_file_states.end() || previous->second != std::pair{size, time})) return true;
    }
    return false;
}

void App::request_resume() {
    if (snapshot && snapshot->stopped() && reload_changed_roms && rom_files_changed()) {
        ProjectSession::Action action;
        action.kind = ProjectSession::ActionKind::reload_roms;
        action.selected = selected;
        if (project_session.request(std::move(action))) {
            reloading_roms = true;
            resume_after_rom_reload = true;
        }
        return;
    }
    pending_runtime_command = controller.request_resume();
}

void App::clocks() {
    ImGui::Begin("Clock Control");

    if (!snapshot) {
        ImGui::TextUnformatted("Waiting for simulation snapshot...");
        ImGui::End();
        return;
    }

    // Keep state controls distinct without turning the toolbar into a bank of
    // saturated status lights. Labels carry the meaning; color is secondary.
    const bool paused = snapshot->paused();
    const bool stopped = snapshot->stopped();
    ImVec4 run_color = (paused || stopped) ? ImVec4(0.24f, 0.65f, 0.28f, 1.0f)
                                           : ImVec4(0.88f, 0.70f, 0.18f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, run_color);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(run_color.x * 1.15f, run_color.y * 1.15f, run_color.z * 1.15f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                          ImVec4(run_color.x * 0.85f, run_color.y * 0.85f, run_color.z * 0.85f, 1.0f));
    if (ImGui::Button(stopped ? "Start" : paused ? "Resume" : "Pause")) {
        if (paused || stopped)
            request_resume();
        else {
            host_input.release(window, controller);
            pending_runtime_command = controller.request_pause();
        }
    }
    ImGui::PopStyleColor(3);

    // A dedicated Stop sits directly beside Run/Pause.  It is not the same as
    // Cold Reset: it stops the run loop, performs a cold reset, and rewinds
    // the simulated clock to zero.
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.58f, 0.15f, 0.17f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.65f, 0.20f, 0.23f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.87f, 0.24f, 0.28f, 1.0f));
    if (ImGui::Button("Stop")) {
        host_input.release(window, controller);
        pending_runtime_command = controller.request_stop();
    }
    ImGui::PopStyleColor(3);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Stops the rack, cold resets it, and sets time back to zero");

    ImGui::SameLine();
    if (ImGui::Button("Cold Reset")) {
        host_input.release(window, controller);
        pending_runtime_command = controller.request_reset(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Hot Reset")) {
        host_input.release(window, controller);
        pending_runtime_command = controller.request_reset(false);
    }

    const char *mode = snapshot->time_mode == UiTimeMode::project
                           ? "Project"
                       : snapshot->time_mode == UiTimeMode::fixed
                           ? "Fixed"
                           : "System (nondeterministic)";
    ImGui::TextDisabled("%s  ·  %.2f sec  ·  %s", snapshot->stop_reason.c_str(),
                       static_cast<double>(snapshot->now) / 1000000000.0, mode);
    if (ImGui::IsItemHovered() && snapshot->stop_reason.size() > 24)
        ImGui::SetTooltip("%s", snapshot->stop_reason.c_str());

    if (pending_runtime_command > snapshot->command_ack_seq)
        ImGui::TextDisabled("Applying control change...");

    ImGui::SeparatorText("Simulation load");
    const float load = static_cast<float>(snapshot->simulation_load_percent) / 100.0f;
    ImGui::ProgressBar(load, ImVec2(ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("100%").x, 0), "");
    ImGui::SameLine();
    if (snapshot->simulation_load_percent > 100)
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.25f, 1.0f), "%u%%",
                           snapshot->simulation_load_percent);
    else
        ImGui::Text("%u%%", snapshot->simulation_load_percent);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Host time used per simulated second\n"
            "100%% is real time. Over that means it is falling behind");
    if (snapshot->simulation_load_percent > 100)
        ImGui::TextDisabled("Behind real time");

    ImGui::TextDisabled("Queue: %llu  Last command: %.2f ms  Slice: %.2f ms",
        static_cast<unsigned long long>(snapshot->command_queue_depth),
        snapshot->command_latency_us / 1000.0, snapshot->slice_wall_us / 1000.0);
    ImGui::TextDisabled("Snapshot copy: %.2f ms  Discarded catch-up: %.3f s",
        snapshot->snapshot_copy_us / 1000.0, snapshot->discarded_wall_ns / 1e9);

    // Keep external-clock stopping away from the machine Run/Stop controls.
    ImGui::SeparatorText("External clocks");
    ImGui::BeginDisabled(project_session.busy());
    if (ImGui::Button("Stop all")) {
        ProjectSession::Action action;
        action.kind = ProjectSession::ActionKind::stop_clocks;
        action.selected = selected;
        project_session.request(std::move(action));
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Set all 3 master clocks to DC");

    for (uint32_t i = 0; i < 3; ++i) {
        ImGui::PushID(static_cast<int>(i));
        auto c = snapshot->clocks[i];
        ImGui::SeparatorText(("Master clock " + std::to_string(i)).c_str());
        int hz = static_cast<int>(project_session.pending_clock(i).value_or(c.hz));
        ImGui::SetNextItemWidth(150);
        ImGui::BeginDisabled(project_session.busy());
        if (ImGui::InputInt("Hz", &hz)) {
            const auto requested = static_cast<uint32_t>(std::clamp(hz, 0, 50000000));
            ProjectSession::Action action;
            action.kind = ProjectSession::ActionKind::clock;
            action.index = i;
            action.hz = requested;
            action.selected = selected;
            project_session.request(std::move(action));
        }
        ImGui::EndDisabled();
        if (project_session.pending_clock(i)) ImGui::TextDisabled("Applying clock change…");
        ImGui::Text("%s | ticks: %llu", c.hz ? "Running" : "DC",
                    static_cast<unsigned long long>(c.ticks));
        ImGui::BeginDisabled(snapshot->paused() || c.hz != 0);
        if (ImGui::Button("Step one tick"))
            pending_runtime_command = controller.request_step(i);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (c.hz != 0)
                ImGui::SetTooltip("You can only hand step at 0hz");
            else if (snapshot->paused())
                ImGui::SetTooltip("Simulation needs to be running");
        }
        ImGui::PopID();
    }
    ImGui::End();
}

} // namespace srz80::ui
