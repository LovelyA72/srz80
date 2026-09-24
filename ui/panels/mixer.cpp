#include "../gui.hpp"
#include "../theme.hpp"
#include <srz80/imgui_input.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

namespace srz80::ui {

namespace {
struct MeterState {
    uint64_t sequence = 0;
    double last_time = 0.0;
    float level[2]{};
    float target[2]{};
};
std::map<Handle, MeterState> meter_states;
uint64_t meter_generation = 0;

void level_meter(Handle id, uint64_t sequence, const uint32_t *peaks, int channels,
                 float height, bool silent) {
    auto &state = meter_states[id];
    const double now = ImGui::GetTime();
    const float elapsed = static_cast<float>(std::clamp(now - state.last_time, 0.0, 1.0));
    state.last_time = now;
    for (int channel = 0; channel < channels; ++channel) {
        if (silent) {
            state.level[channel] = 0.0f;
            state.target[channel] = 0.0f;
        } else {
            if (state.sequence != sequence)
                state.target[channel] = std::clamp(static_cast<float>(peaks[channel]) / 32768.0f,
                                                   0.0f, 1.0f);
            const float response = state.target[channel] > state.level[channel] ? 0.045f : 0.25f;
            const float blend = 1.0f - std::exp(-elapsed / response);
            state.level[channel] += (state.target[channel] - state.level[channel]) * blend;
        }
    }
    state.sequence = sequence;
    constexpr float bar_width = 5.0f;
    constexpr float gap = 2.0f;
    const float width = channels * bar_width + (channels - 1) * gap;
    const ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, height));
    auto *draw = ImGui::GetWindowDrawList();
    for (int channel = 0; channel < channels; ++channel) {
        const float x = top.x + channel * (bar_width + gap);
        const float fraction = state.level[channel];
        draw->AddRectFilled(ImVec2(x, top.y), ImVec2(x + bar_width, top.y + height),
                            ImGui::GetColorU32(ImGuiCol_FrameBg));
        if (fraction > 0.0f)
            draw->AddRectFilled(ImVec2(x, top.y + height * (1.0f - fraction)),
                                ImVec2(x + bar_width, top.y + height),
                                ImGui::GetColorU32(mixer_meter_color()));
    }
}
} // namespace

void App::mixer() {
    ImGui::SetNextWindowSize(ImVec2(640.0f, 360.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Mixer", &show_mixer)) {
        ImGui::End();
        return;
    }

    if (audio_backend.opened())
        ImGui::TextDisabled("%s", audio_backend.status().c_str());
    else
        ImGui::TextDisabled("Audio unavailable");
    ImGui::Separator();

    if (!snapshot) {
        ImGui::End();
        return;
    }
    if (meter_generation != snapshot->generation) {
        meter_states.clear();
        meter_generation = snapshot->generation;
    }
    const float strip_height = std::max(150.0f, ImGui::GetContentRegionAvail().y - 88.0f);
    auto master = static_cast<int>(snapshot->audio_master_volume);
    ImGui::BeginGroup();
    ImGui::PushID("master");
    bool master_changed =
        ImGui::VSliderInt("##volume", ImVec2(52.0f, strip_height), &master, 0, 100, "%d%%");
    master_changed |= srz80::gui::reset_on_middle_click(master, 100);
    if (master_changed) {
        controller.set_audio_master_volume(static_cast<uint32_t>(std::clamp(master, 0, 100)));
        project_session.commit_mixer_master(static_cast<uint32_t>(std::clamp(master, 0, 100)));
    }
    ImGui::SameLine(0.0f, 3.0f);
    level_meter(0, snapshot->sequence, snapshot->audio_master_levels.data(), 2,
                strip_height, snapshot->paused());
    ImGui::TextUnformatted("Master");
    ImGui::PopID();
    ImGui::EndGroup();

    ImGui::SameLine();
    ImGui::BeginChild("mixer_sources", ImVec2(0.0f, 0.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    const auto sources = snapshot->audio_sources;
    if (sources.empty()) {
        ImGui::TextDisabled("No sources");
    } else {
        std::map<std::pair<Handle, std::string>, uint32_t> source_ordinals;
        for (const auto &source : sources) {
            if (!source.active)
                continue;
            const auto ordinal = source_ordinals[{source.owner, source.name}]++;
            ImGui::PushID(static_cast<int>(source.id));
            ImGui::BeginGroup();
            int volume = static_cast<int>(source.volume_percent);
            bool muted = source.muted;
            int pan = source.pan;
            bool volume_changed =
                ImGui::VSliderInt("##volume", ImVec2(52.0f, strip_height), &volume, 0, 150, "%d%%");
            volume_changed |= srz80::gui::reset_on_middle_click(volume, 100);
            if (volume_changed) {
                controller.set_audio_source_volume(source.id,
                                                   static_cast<uint32_t>(std::clamp(volume, 0, 150)), snapshot->generation);
                project_session.commit_mixer_source(source.owner, source.name, ordinal,
                                                     static_cast<uint32_t>(std::clamp(volume, 0, 150)), muted, pan);
            }
            ImGui::SameLine(0.0f, 3.0f);
            level_meter(source.id, snapshot->sequence, &source.level_peak, 1,
                        strip_height, snapshot->paused() || source.muted);
            ImGui::SetNextItemWidth(72.0f);
            bool pan_changed =
                ImGui::SliderInt("##pan", &pan, -64, 63, "Pan %d", ImGuiSliderFlags_AlwaysClamp);
            pan_changed |= srz80::gui::reset_on_middle_click(pan, 0);
            if (pan_changed) {
                controller.set_audio_source_pan(source.id, pan, snapshot->generation);
                project_session.commit_mixer_source(source.owner, source.name, ordinal,
                                                     static_cast<uint32_t>(volume), muted, pan);
            }
            if (ImGui::Checkbox("Mute", &muted)) {
                controller.set_audio_source_muted(source.id, muted, snapshot->generation);
                project_session.commit_mixer_source(source.owner, source.name, ordinal,
                                                     static_cast<uint32_t>(volume), muted, pan);
            }
            ImGui::TextWrapped("%s", source.name.empty() ? "Audio source" : source.name.c_str());
            ImGui::EndGroup();
            ImGui::SameLine();
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

} // namespace srz80::ui
