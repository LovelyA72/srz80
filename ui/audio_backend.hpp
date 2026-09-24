#pragma once

#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <algorithm>
#include <cstdint>
#include <string>

namespace srz80::ui {

class SimulationController;

struct AudioBackendSettings {
    bool enabled = true;
    std::string driver = "Automatic";
    std::string device = "<System default>";
    uint32_t sample_rate = 44100;
    uint32_t outputs = 2;
    uint32_t buffer_size = 1024;
    bool low_latency = false;
    bool force_mono = false;
    bool input_enabled = true;
    std::string input_device = "<System default>";
    uint32_t input_channels = 1;
    uint32_t input_volume = 90;
};

class AudioBackend {
  public:
    AudioBackend() = default;
    ~AudioBackend();
    AudioBackend(const AudioBackend &) = delete;
    AudioBackend &operator=(const AudioBackend &) = delete;

    bool open(SimulationController &controller, const AudioBackendSettings &settings);
    void close();
    bool opened() const { return stream_ != nullptr && device_ != 0; }
    const std::string &error() const { return error_; }
    std::string status() const;
    void update_input();
    void set_input_monitoring(bool enabled) { input_monitoring_.store(enabled); }
    void retry_input();
    void input_device_removed(SDL_AudioDeviceID device);
    bool recording() const;
    float input_level() const {
        return std::max(input_peak_.load(), SDL_GetTicks() < input_clip_until_.load() ? 1.0f : 0.0f);
    }
    const std::string &input_error() const { return input_error_; }
    void set_input_volume(uint32_t percent) { input_volume_.store(std::min(percent, 200u)); }

  private:
    static void SDLCALL supply_audio(void *context, SDL_AudioStream *stream,
                                     int additional_bytes, int total_bytes);
    void close_input();
    static void SDLCALL capture_audio(void *, SDL_AudioStream *, int, int);
    AudioBackendSettings settings_;
    SDL_AudioDeviceID input_device_ = 0;
    uint32_t input_rate_ = 0;
    SDL_AudioDeviceID input_physical_ = 0;
    SDL_AudioStream *input_stream_ = nullptr;
    std::array<float, 1024 * 8> input_scratch_{};
    std::atomic<uint32_t> input_volume_{90};
    std::atomic<float> input_peak_{0};
    std::atomic<uint64_t> input_clip_until_{0};
    std::atomic<bool> input_failed_{false};
    std::atomic<bool> input_monitoring_{false};
    bool input_attempted_ = false;
    std::string input_error_;
    SimulationController *controller_ = nullptr;
    SDL_AudioDeviceID device_ = 0;
    SDL_AudioStream *stream_ = nullptr;
    SDL_AudioSpec requested_{};
    SDL_AudioSpec actual_{};
    int actual_buffer_frames_ = 0;
    uint32_t target_buffer_frames_ = 1024;
    uint64_t pcm_epoch_ = 0;
    uint32_t prebuffer_frames_ = 0;
    bool primed_ = false;
    std::array<int16_t, 8192 * 2> scratch_{};
    std::string error_;
};
} // namespace srz80::ui
