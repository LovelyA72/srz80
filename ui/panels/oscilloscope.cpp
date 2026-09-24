#include "../gui.hpp"
#include <algorithm>
#include <cmath>

namespace srz80::ui {
namespace {
constexpr uint32_t sample_rate = 44100;

int16_t sample_at(const std::vector<int16_t> &samples, size_t frame, int channel) {
    if (channel == 2)
        return static_cast<int16_t>((static_cast<int32_t>(samples[frame * 2]) +
                                     static_cast<int32_t>(samples[frame * 2 + 1])) / 2);
    return samples[frame * 2 + static_cast<size_t>(channel)];
}

bool crosses(int16_t before, int16_t after, int16_t level, bool rising) {
    return rising ? before < level && after >= level : before > level && after <= level;
}

void draw_channel(ImDrawList *draw_list, ImVec2 origin, ImVec2 size,
                  const std::vector<int16_t> &samples, size_t start, size_t frames,
                  int channel, float gain, ImU32 color) {
    if (frames < 2 || size.x < 2.0f)
        return;
    const size_t points = std::min<size_t>(frames, std::max(2, static_cast<int>(size.x)));
    const float x_step = size.x / static_cast<float>(points - 1);
    const float amplitude = size.y * 0.46f * gain / 32768.0f;
    ImVec2 previous{};
    for (size_t point = 0; point < points; ++point) {
        const size_t frame = start + (point * (frames - 1)) / (points - 1);
        const float value = static_cast<float>(sample_at(samples, frame, channel));
        const ImVec2 current{origin.x + point * x_step, origin.y + size.y * 0.5f - value * amplitude};
        if (point)
            draw_list->AddLine(previous, current, color, 1.5f);
        previous = current;
    }
}

void draw_xy(ImDrawList *draw_list, ImVec2 origin, ImVec2 size,
             const std::vector<int16_t> &samples, size_t start, size_t frames, float gain) {
    if (frames < 2)
        return;
    const size_t points = std::min<size_t>(frames, 4096);
    const float x_amplitude = size.x * 0.46f * gain / 32768.0f;
    const float y_amplitude = size.y * 0.46f * gain / 32768.0f;
    ImVec2 previous{};
    for (size_t point = 0; point < points; ++point) {
        const size_t frame = start + (point * (frames - 1)) / (points - 1);
        const ImVec2 current{origin.x + size.x * 0.5f + sample_at(samples, frame, 0) * x_amplitude,
                             origin.y + size.y * 0.5f - sample_at(samples, frame, 1) * y_amplitude};
        if (point)
            draw_list->AddLine(previous, current, IM_COL32(170, 235, 130, 255), 1.5f);
        previous = current;
    }
}
} // namespace

void App::oscilloscope() {
    ImGui::SetNextWindowSize(ImVec2(760.0f, 400.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Audio Scope", &show_oscilloscope)) {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(100.0f);
    ImGui::SliderInt("Timebase", &scope_timebase_ms, 2, 250, "%d ms");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    ImGui::SliderFloat("Gain", &scope_gain, 0.1f, 8.0f, "%.1fx", ImGuiSliderFlags_Logarithmic);
    ImGui::SameLine();
    ImGui::Checkbox("Freeze", &scope_frozen);
    ImGui::SameLine();
    const char *display_modes[] = {"Mono", "Stereo"};
    int display_mode = scope_stereo ? 1 : 0;
    ImGui::SetNextItemWidth(90.0f);
    if (ImGui::Combo("Mode", &display_mode, display_modes, 2)) {
        scope_stereo = display_mode == 1;
        if (!scope_stereo)
            scope_xy_mode = false;
    }
    if (scope_stereo) {
        ImGui::SameLine();
        ImGui::Checkbox("X-Y", &scope_xy_mode);
    }

    const char *channels[] = {"Left", "Right"};
    if (scope_stereo) {
        ImGui::SetNextItemWidth(95.0f);
        ImGui::Combo("Trigger", &scope_trigger_channel, channels, 2);
        ImGui::SameLine();
    } else {
        ImGui::TextUnformatted("Trigger: Mono");
        ImGui::SameLine();
    }
    ImGui::SetNextItemWidth(130.0f);
    ImGui::SliderInt("Level", &scope_trigger_level, -100, 100, "%d%%");
    ImGui::SameLine();
    ImGui::Checkbox("Rising edge", &scope_trigger_rising);
    if (scope_stereo && !scope_xy_mode) {
        ImGui::SameLine();
        ImGui::Checkbox("L", &scope_show_left);
        ImGui::SameLine();
        ImGui::Checkbox("R", &scope_show_right);
    }

    const uint32_t display_frames = std::max(2u, (sample_rate * static_cast<uint32_t>(scope_timebase_ms)) / 1000);
    if (!scope_frozen)
        controller.audio_recent(scope_samples, display_frames * 2);

    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    canvas_size.x = std::max(canvas_size.x, 64.0f);
    canvas_size.y = std::max(canvas_size.y, 80.0f);
    const ImVec2 canvas_origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##scope_canvas", canvas_size);
    ImDrawList *draw_list = ImGui::GetWindowDrawList();
    const ImVec2 canvas_end{canvas_origin.x + canvas_size.x, canvas_origin.y + canvas_size.y};
    draw_list->AddRectFilled(canvas_origin, canvas_end, IM_COL32(12, 15, 18, 255));
    draw_list->AddRect(canvas_origin, canvas_end, IM_COL32(100, 110, 120, 255));
    for (int division = 1; division < 10; ++division) {
        const float x = canvas_origin.x + canvas_size.x * division / 10.0f;
        draw_list->AddLine(ImVec2(x, canvas_origin.y), ImVec2(x, canvas_end.y), IM_COL32(42, 49, 55, 255));
    }
    for (int division = 1; division < 8; ++division) {
        const float y = canvas_origin.y + canvas_size.y * division / 8.0f;
        draw_list->AddLine(ImVec2(canvas_origin.x, y), ImVec2(canvas_end.x, y), IM_COL32(42, 49, 55, 255));
    }

    const size_t available = scope_samples.size() / 2;
    if (available < display_frames) {
        ImGui::SetCursorScreenPos(ImVec2(canvas_origin.x + 8.0f, canvas_origin.y + 8.0f));
        ImGui::TextDisabled("Waiting for mixed PCM (%zu / %u frames)", available, display_frames);
        ImGui::End();
        return;
    }

    const int16_t level = static_cast<int16_t>((scope_trigger_level * 32767) / 100);
    size_t start = available - display_frames;
    const int trigger_channel = scope_stereo ? scope_trigger_channel : 2;
    const size_t pre_trigger = display_frames / 2;
    const size_t post_trigger = display_frames - pre_trigger;
    if (available > display_frames + 1) {
        // Require context on both sides of the edge, so the trigger lands at
        // the horizontal center rather than the start of the trace.
        for (size_t frame = available - post_trigger; frame > pre_trigger; --frame) {
            if (crosses(sample_at(scope_samples, frame - 1, trigger_channel),
                        sample_at(scope_samples, frame, trigger_channel), level,
                        scope_trigger_rising)) {
                start = frame - pre_trigger;
                break;
            }
        }
    }

    draw_list->PushClipRect(canvas_origin, canvas_end, true);
    if (scope_stereo && scope_xy_mode)
        draw_xy(draw_list, canvas_origin, canvas_size, scope_samples, start, display_frames, scope_gain);
    else if (scope_stereo) {
        if (scope_show_left)
            draw_channel(draw_list, canvas_origin, canvas_size, scope_samples, start, display_frames, 0,
                         scope_gain, IM_COL32(85, 210, 255, 255));
        if (scope_show_right)
            draw_channel(draw_list, canvas_origin, canvas_size, scope_samples, start, display_frames, 1,
                         scope_gain, IM_COL32(255, 170, 75, 255));
    }
    else
        draw_channel(draw_list, canvas_origin, canvas_size, scope_samples, start, display_frames, 2,
                     scope_gain, IM_COL32(170, 235, 130, 255));
    draw_list->PopClipRect();
    ImGui::End();
}

} // namespace srz80::ui
