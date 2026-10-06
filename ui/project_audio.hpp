#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <nlohmann/json.hpp>

namespace srz80::ui {

struct CompressorSettings {
    bool enabled = false;
    float downward_threshold_db = -12.0f;
    float downward_ratio = 4.0f;
    float upward_threshold_db = -40.0f;
    float upward_ratio = 2.0f;
    float attack_ms = 10.0f;
    float release_ms = 100.0f;
    float knee_db = 6.0f;
    float max_boost_db = 12.0f;
    float makeup_db = 6.0f;
    bool operator==(const CompressorSettings &) const = default;
};

struct CompressorParameter {
    const char *key, *label, *format;
    float CompressorSettings::*member;
    float minimum, maximum;
};

inline constexpr std::array compressor_parameters = {
    CompressorParameter{"downward_threshold_db", "Downward threshold", "%.1f dBFS", &CompressorSettings::downward_threshold_db, -96.0f, 0.0f},
    CompressorParameter{"downward_ratio", "Downward ratio", "%.1f:1", &CompressorSettings::downward_ratio, 1.0f, 20.0f},
    CompressorParameter{"upward_threshold_db", "Upward threshold", "%.1f dBFS", &CompressorSettings::upward_threshold_db, -96.0f, 0.0f},
    CompressorParameter{"upward_ratio", "Upward ratio", "%.1f:1", &CompressorSettings::upward_ratio, 1.0f, 20.0f},
    CompressorParameter{"attack_ms", "Attack", "%.1f ms", &CompressorSettings::attack_ms, 0.1f, 1000.0f},
    CompressorParameter{"release_ms", "Release", "%.1f ms", &CompressorSettings::release_ms, 1.0f, 5000.0f},
    CompressorParameter{"knee_db", "Knee", "%.1f dB", &CompressorSettings::knee_db, 0.0f, 24.0f},
    CompressorParameter{"max_boost_db", "Maximum upward boost", "%.1f dB", &CompressorSettings::max_boost_db, 0.0f, 36.0f},
    CompressorParameter{"makeup_db", "Makeup gain", "%+.1f dB", &CompressorSettings::makeup_db, -24.0f, 24.0f},
};

struct ProjectAudioSettings {
    int gain_tenths = 0;
    CompressorSettings compressor;
    bool software_clipping = true;
    bool operator==(const ProjectAudioSettings &) const = default;
};

inline ProjectAudioSettings project_audio_settings(const nlohmann::json &audio) {
    ProjectAudioSettings settings;
    if (!audio.is_object()) return settings;
    if (const auto gain = audio.find("gain_db"); gain != audio.end() && gain->is_number()) {
        const double db = gain->get<double>();
        if (std::isfinite(db)) settings.gain_tenths = static_cast<int>(std::lround(std::clamp(db, -36.0, 24.0) * 10.0));
    }
    if (const auto clip = audio.find("software_clipping"); clip != audio.end() && clip->is_boolean())
        settings.software_clipping = clip->get<bool>();
    const auto comp = audio.find("compressor");
    if (comp == audio.end() || !comp->is_object()) return settings;
    if (const auto enabled = comp->find("enabled"); enabled != comp->end() && enabled->is_boolean())
        settings.compressor.enabled = enabled->get<bool>();
    for (const auto &parameter : compressor_parameters) {
        const auto value = comp->find(parameter.key);
        if (value == comp->end() || !value->is_number()) continue;
        const float parsed = value->get<float>();
        if (std::isfinite(parsed))
            settings.compressor.*parameter.member = std::clamp(parsed, parameter.minimum, parameter.maximum);
    }
    return settings;
}

inline void write_project_audio(nlohmann::json &audio, const ProjectAudioSettings &settings) {
    if (!audio.is_object()) audio = nlohmann::json::object();
    audio["gain_db"] = std::clamp(settings.gain_tenths, -360, 240) / 10.0;
    audio["software_clipping"] = settings.software_clipping;
    auto &comp = audio["compressor"];
    if (!comp.is_object()) comp = nlohmann::json::object();
    comp["enabled"] = settings.compressor.enabled;
    for (const auto &parameter : compressor_parameters)
        comp[parameter.key] = settings.compressor.*parameter.member;
}

} // namespace srz80::ui
