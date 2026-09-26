#pragma once
#include "ui_snapshot.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace srz80::ui {
// Append-only shader uniform layout. Keep integer scanout counters exact even
// after float loses integer precision. The shader owns its signal phase model.
struct VideoShaderUniforms {
    std::array<float, 4> source{};
    std::array<uint32_t, 4> scanout{}; // frame low/high words, next line, line count
};
static_assert(offsetof(VideoShaderUniforms, scanout) == 16);
static_assert(sizeof(VideoShaderUniforms) == 32);
inline VideoShaderUniforms video_shader_uniforms(uint32_t width, uint32_t height,
                                                uint32_t logical_width,
                                                const UiVideoSnapshot::VideoFrame &frame) {
    VideoShaderUniforms out;
    out.source = {float(width), float(height), 0.0f, float(logical_width)};
    if (frame.has_scanout) {
        // Even/odd scanout frames drive the shader's interlace phase.
        out.source[2] = float(frame.scanout_frame & 1u);
        out.scanout = {uint32_t(frame.scanout_frame), uint32_t(frame.scanout_frame >> 32u),
                       frame.scanout_line, frame.scanout_lines};
    }
    return out;
}
} // namespace srz80::ui
