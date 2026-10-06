#include "audio_backend.hpp"
#include "simulation_controller.hpp"
#include <algorithm>
#include <sstream>
#include <cmath>

namespace srz80::ui {
namespace {
std::string sdl_error(const char *fallback) {
    const char *error = SDL_GetError();
    return error && *error ? error : fallback;
}
}

AudioBackend::~AudioBackend() { close(); }

bool AudioBackend::open(SimulationController &controller, const AudioBackendSettings &settings) {
    close();
    controller_ = &controller;
    settings_ = settings;
    settings_.input_channels = std::clamp(settings.input_channels, 1u, 8u);
    input_volume_.store(std::min(settings.input_volume, 200u));
    input_attempted_ = false;
    input_failed_.store(false);
    input_error_.clear();
    error_.clear();
    if (!settings.enabled) {
        error_ = "Audio disabled in settings";
        return false;
    }

    // SDL selects a driver when the audio subsystem initializes, not when a
    // device opens. Reinitialize after closing our stream so persisted and live
    // selections actually take effect. This application owns the subsystem.
    const char *desired = settings.driver == "Automatic" ? nullptr : settings.driver.c_str();
    const char *current = SDL_GetCurrentAudioDriver();
    const char *hint = SDL_GetHint(SDL_HINT_AUDIO_DRIVER);
    const bool selection_changed = desired ? (!current || settings.driver != current)
                                           : (hint && *hint);
    if (!current || selection_changed) {
        if (current)
            SDL_QuitSubSystem(SDL_INIT_AUDIO);
        SDL_SetHintWithPriority(SDL_HINT_AUDIO_DRIVER, desired, SDL_HINT_OVERRIDE);
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            error_ = sdl_error("Cannot initialize selected audio driver");
            controller.log_message("[audio] Disabled: " + error_);
            controller_ = nullptr;
            return false;
        }
    }

    const uint32_t channels = settings.force_mono ? 1u : std::clamp(settings.outputs, 1u, 2u);
    const uint32_t requested_buffer = settings.low_latency ? std::min(settings.buffer_size, 256u)
                                                            : settings.buffer_size;
    target_buffer_frames_ = std::clamp(requested_buffer, 32u, 4096u);
    const std::string buffer_hint = std::to_string(target_buffer_frames_);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, buffer_hint.c_str());

    requested_ = {SDL_AUDIO_S16LE, static_cast<int>(channels),
                  static_cast<int>(std::clamp(settings.sample_rate, 8000u, 384000u))};
    SDL_AudioDeviceID physical = SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK;
    if (settings.device != "<System default>" && !settings.device.empty()) {
        int count = 0;
        SDL_AudioDeviceID *devices = SDL_GetAudioPlaybackDevices(&count);
        for (int i = 0; devices && i < count; ++i) {
            const char *name = SDL_GetAudioDeviceName(devices[i]);
            if (name && settings.device == name) {
                physical = devices[i];
                break;
            }
        }
        SDL_free(devices);
        if (physical == SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK) {
            error_ = "Playback device not found: " + settings.device;
            controller.log_message("[audio] Disabled: " + error_);
            controller_ = nullptr;
            return false;
        }
    }

    device_ = SDL_OpenAudioDevice(physical, &requested_);
    if (!device_) {
        error_ = sdl_error("SDL could not open an audio device");
        controller.log_message("[audio] Disabled: " + error_);
        controller_ = nullptr;
        return false;
    }

    // The controller mixer and SDL stream use the same requested rate. SDL may
    // still convert if the physical device cannot open at that rate.
    const uint32_t mixer_rate = std::clamp(settings.sample_rate, 8000u, 384000u);
    SDL_AudioSpec source{SDL_AUDIO_S16LE, 2, static_cast<int>(mixer_rate)};
    stream_ = SDL_CreateAudioStream(&source, nullptr);
    if (!stream_ || !SDL_BindAudioStream(device_, stream_)) {
        error_ = sdl_error("SDL could not create the audio conversion stream");
        controller.log_message("[audio] Disabled: " + error_);
        close();
        return false;
    }
    if (!SDL_GetAudioDeviceFormat(device_, &actual_, &actual_buffer_frames_)) {
        actual_ = requested_;
        actual_buffer_frames_ = static_cast<int>(target_buffer_frames_);
    }
    // Accumulate two device periods before starting/recovering playback so
    // normal producer wake-up jitter does not alternate audio and silence.
    prebuffer_frames_ = std::max<uint32_t>(mixer_rate / 100,
        static_cast<uint64_t>(actual_buffer_frames_) * 2 * mixer_rate / std::max(actual_.freq, 1));
    primed_ = false;
    pcm_epoch_ = controller.audio_epoch();
    if (!SDL_SetAudioStreamGetCallback(stream_, supply_audio, this)) {
        error_ = sdl_error("Cannot install audio delivery callback");
        close();
        return false;
    }
    if (!SDL_ResumeAudioDevice(device_)) {
        error_ = sdl_error("SDL could not start the audio device");
        controller.log_message("[audio] Disabled: " + error_);
        close();
        return false;
    }
    controller.log_message("[audio] " + status());
    return true;
}

void AudioBackend::close() {
    close_input();
    if (device_)
        SDL_PauseAudioDevice(device_);
    if (stream_) {
        // SDL serializes callback replacement with in-flight callbacks. Do not
        // release the borrowed controller until no callback can access it.
        SDL_SetAudioStreamGetCallback(stream_, nullptr, nullptr);
        SDL_DestroyAudioStream(stream_);
    }
    if (device_)
        SDL_CloseAudioDevice(device_);
    stream_ = nullptr;
    device_ = 0;
    controller_ = nullptr;
}

void SDLCALL AudioBackend::supply_audio(void *context, SDL_AudioStream *stream,
                                      int additional_bytes, int total_bytes) {
    auto &self = *static_cast<AudioBackend *>(context);
    // SDL holds the stream lock. This callback touches only the thread-safe
    // PCM queue. Engine, card, tool and ImGui code remain on their own threads.
    const auto epoch = self.controller_->audio_epoch();
    if (self.pcm_epoch_ != epoch) {
        SDL_ClearAudioStream(stream);
        self.pcm_epoch_ = epoch;
        self.primed_ = false;
        additional_bytes = total_bytes;
    }
    if (!self.primed_) {
        if (self.controller_->audio_available() <
            std::min(self.prebuffer_frames_, self.controller_->audio_capacity()))
            return;
        self.primed_ = true;
    }
    constexpr int bytes_per_frame = sizeof(int16_t) * 2;
    uint32_t remaining = additional_bytes > 0
        ? (static_cast<uint32_t>(additional_bytes) + bytes_per_frame - 1) / bytes_per_frame : 0;
    while (remaining) {
        const auto want = std::min<uint32_t>(remaining, self.scratch_.size() / 2);
        uint64_t block_epoch;
        const auto got = self.controller_->audio_read(self.scratch_.data(), want, block_epoch);
        if (block_epoch != self.pcm_epoch_) {
            SDL_ClearAudioStream(stream);
            self.pcm_epoch_ = block_epoch;
        }
        // Discard an old block if reset raced the pop. Retry on the next device
        // request, keeping this callback bounded even during repeated resets.
        if (!got || self.controller_->audio_epoch() != block_epoch) {
            self.primed_ = false;
            break;
        }
        if (!SDL_PutAudioStreamData(stream, self.scratch_.data(), got * bytes_per_frame))
            break;
        remaining -= got;
    }
}

bool AudioBackend::recording() const {
    return input_stream_ && !input_failed_.load() && controller_ && controller_->audio_input_requested();
}

void AudioBackend::close_input() {
    if (input_device_) SDL_PauseAudioDevice(input_device_);
    if (input_stream_) {
        SDL_SetAudioStreamPutCallback(input_stream_, nullptr, nullptr);
        SDL_DestroyAudioStream(input_stream_);
    }
    if (input_device_) SDL_CloseAudioDevice(input_device_);
    input_stream_ = nullptr;
    input_device_ = 0;
    input_peak_.store(0);
    input_clip_until_.store(0);
    if (controller_) controller_->audio_input_push(nullptr, 0, 0, 0);
}

void AudioBackend::input_device_removed(SDL_AudioDeviceID device) {
    // SDL does not report the physical ID behind a default logical device.
    // Fail closed on recording-device removal rather than following a new mic.
    if (input_stream_ && (device == input_device_ || device == input_physical_ ||
                          input_physical_ == SDL_AUDIO_DEVICE_DEFAULT_RECORDING))
        input_failed_.store(true);
}

void AudioBackend::retry_input() {
    close_input();
    input_attempted_ = false;
    input_failed_.store(false);
    input_error_.clear();
}

void AudioBackend::update_input() {
    if (!controller_ || !opened()) return;
    if (input_failed_.load()) {
        if (input_stream_) {
            input_error_ = "Audio input device disconnected or capture failed. Apply audio settings to retry.";
            controller_->log_message("[audio input] " + input_error_);
            close_input();
        }
        return;
    }
    if (!settings_.input_enabled || (!controller_->audio_input_present() && !input_monitoring_.load())) {
        if (input_stream_) { close_input(); input_attempted_ = false; }
        return;
    }
    if (input_stream_) {
        SDL_AudioSpec native{};
        if (!SDL_GetAudioDeviceFormat(input_device_, &native, nullptr) ||
            native.freq != static_cast<int>(input_rate_)) {
            input_failed_.store(true);
            update_input();
        }
        return;
    }
    // Failed opens are latched until settings are reapplied, not retried every frame.
    if (input_attempted_) return;
    input_attempted_ = true;
    SDL_AudioDeviceID physical = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (settings_.input_device != "<System default>" && !settings_.input_device.empty()) {
        int count = 0;
        auto *devices = SDL_GetAudioRecordingDevices(&count);
        for (int i = 0; devices && i < count; ++i) {
            const char *name = SDL_GetAudioDeviceName(devices[i]);
            if (name && settings_.input_device == name) { physical = devices[i]; break; }
        }
        SDL_free(devices);
        if (physical == SDL_AUDIO_DEVICE_DEFAULT_RECORDING)
            input_error_ = "Recording device not found: " + settings_.input_device;
    }
    if (input_error_.empty()) {
        input_physical_ = physical;
        // Open native format first. WASAPI shared-mode devices need not support
        // our requested channel count/rate. SDL's stream performs conversion.
        input_device_ = SDL_OpenAudioDevice(physical, nullptr);
        if (input_device_) {
            SDL_PauseAudioDevice(input_device_);
            SDL_AudioSpec native{};
            if (!SDL_GetAudioDeviceFormat(input_device_, &native, nullptr) || native.freq < 8000 || native.freq > 384000) {
                input_error_ = sdl_error("Unsupported recording device format");
                close_input();
                controller_->log_message("[audio input] " + input_error_);
                return;
            }
            input_rate_ = static_cast<uint32_t>(native.freq);
            // Preserve native rate: only Rust's nearest-exact converter changes
            // capture sample rate. SDL handles float/channel conversion only.
            SDL_AudioSpec destination{SDL_AUDIO_F32, static_cast<int>(settings_.input_channels), native.freq};
            input_stream_ = SDL_CreateAudioStream(nullptr, &destination);
            if (input_stream_ && SDL_BindAudioStream(input_device_, input_stream_) &&
                SDL_SetAudioStreamPutCallback(input_stream_, capture_audio, this) &&
                SDL_ResumeAudioDevice(input_device_)) return;
        }
        input_error_ = sdl_error("Cannot start audio input");
    }
    close_input();
    controller_->log_message("[audio input] " + input_error_);
}

void SDLCALL AudioBackend::capture_audio(void *context, SDL_AudioStream *stream, int, int) {
    auto &self = *static_cast<AudioBackend *>(context);
    if ((!self.controller_->audio_input_present() && !self.input_monitoring_.load()) || self.input_failed_.load()) {
        SDL_ClearAudioStream(stream);
        self.input_peak_.store(0);
        return;
    }
    SDL_AudioSpec source{};
    if (!SDL_GetAudioStreamFormat(stream, &source, nullptr) ||
        source.freq != static_cast<int>(self.input_rate_)) {
        self.input_failed_.store(true);
        SDL_ClearAudioStream(stream);
        return;
    }
    const uint32_t channels = self.settings_.input_channels;
    const int bytes = static_cast<int>(1024 * channels * sizeof(float));
    float peak = 0;
    // Bound work on the SDL thread, and drop backlog instead of accumulating latency.
    for (int block = 0; block < 8; ++block) {
        const int got = SDL_GetAudioStreamData(stream, self.input_scratch_.data(), bytes);
        if (got < 0) { self.input_failed_.store(true); break; }
        if (!got) break;
        const size_t samples = static_cast<size_t>(got) / sizeof(float);
        const float gain = self.input_volume_.load() / 100.0f;
        for (size_t i = 0; i < samples; ++i) {
            const float raw = self.input_scratch_[i] * gain;
            const float value = std::isfinite(raw) ? raw : 0.0f;
            peak = std::max(peak, std::abs(value));
            self.input_scratch_[i] = std::clamp(value, -1.0f, 1.0f);
        }
        // Settings monitoring never queues samples for a stopped simulation.
        if (self.controller_->audio_input_requested())
            self.controller_->audio_input_push(self.input_scratch_.data(),
                static_cast<uint32_t>(samples / channels), self.input_rate_, channels);
    }
    self.input_peak_.store(peak);
    if (peak >= 1.0f) self.input_clip_until_.store(SDL_GetTicks() + 250);
    if (SDL_GetAudioStreamAvailable(stream) > bytes * 8) SDL_ClearAudioStream(stream);
}

std::string AudioBackend::status() const {
    std::ostringstream out;
    out << "Playback device opened (" << (SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none")
        << "): want " << requested_.freq << " Hz @ " << requested_.channels
        << " channel" << (requested_.channels == 1 ? "" : "s") << ", " << target_buffer_frames_
        << " frames; got " << actual_.freq << " Hz @ " << actual_.channels << " channel"
        << (actual_.channels == 1 ? "" : "s") << ", " << actual_buffer_frames_ << " frames";
    return out.str();
}
} // namespace srz80::ui
