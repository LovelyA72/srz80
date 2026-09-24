#include "simulation_controller.hpp"
#include <srz80/providers.h>
#include <algorithm>
#include <cassert>
#include <fstream>
#include <stdexcept>

namespace srz80::ui {
uint64_t SimulationController::request_pause() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Pause;
    Command c{std::move(legacy)};
    return post_command(std::move(c));
}
uint64_t SimulationController::request_resume() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Resume;
    Command c{std::move(legacy)};
    return post_command(std::move(c));
}
uint64_t SimulationController::request_stop() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Stop;
    Command c{std::move(legacy)};
    return post_command(std::move(c));
}
uint64_t SimulationController::request_reset(bool cold) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Reset;
    legacy.flag = cold;
    Command c{std::move(legacy)};
    return post_command(std::move(c));
}
uint64_t SimulationController::request_step(uint32_t clock) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Step;
    legacy.clock = clock;
    Command c{std::move(legacy)};
    return post_command(std::move(c));
}
uint64_t SimulationController::request_frequency(uint32_t clock, uint32_t hz) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Frequency;
    legacy.clock = clock;
    legacy.hz = hz;
    Command c{std::move(legacy)};
    return post_command(std::move(c));
}
uint64_t SimulationController::request_stop_clocks() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::StopClocks;
    Command c{std::move(legacy)};
    return post_command(std::move(c));
}
bool SimulationController::stop_and_wait(std::string &error) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Stop;
    Command c{std::move(legacy)};
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    error = r.error;
    return r.status == SRH_OK;
}
void SimulationController::enqueue_input(const std::string &endpoint, uint64_t timestamp,
                                         uint8_t value, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::EnqueueInput;
    legacy.text1 = endpoint;
    legacy.value1 = timestamp;
    legacy.value2 = value;
    Command c{std::move(legacy)};
    c.generation = generation;
    post_command(std::move(c));
}
void SimulationController::clear_trace() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ClearTrace;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
void SimulationController::set_trace_capture(bool enabled, uint32_t operations) {
    operations &= SRH_READ | SRH_WRITE;
    const bool previous_enabled = trace_capture_requested_.exchange(enabled);
    const uint32_t previous_operations = trace_capture_operations_requested_.exchange(operations);
    if (previous_enabled == enabled && previous_operations == operations)
        return;
    LegacyCommand legacy;
    legacy.kind = CmdKind::SetTraceCapture;
    legacy.flag = enabled;
    legacy.hz = operations;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
void SimulationController::clear_logs() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ClearLogs;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
void SimulationController::add_breakpoint(Handle card, Handle space, uint64_t first, uint64_t last,
                                          uint32_t operations, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::AddBreakpoint;
    legacy.handle1 = card;
    legacy.handle2 = space;
    legacy.value1 = first;
    legacy.value2 = last;
    legacy.hz = operations;
    Command c{std::move(legacy)};
    c.generation = generation;
    post_command(std::move(c));
}
void SimulationController::remove_breakpoint(uint64_t id, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::RemoveBreakpoint;
    legacy.value1 = id;
    Command c{std::move(legacy)};
    c.generation = generation;
    post_command(std::move(c));
}
void SimulationController::set_breakpoint_enabled(uint64_t id, bool enabled, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::SetBreakpointEnabled;
    legacy.value1 = id;
    legacy.flag = enabled;
    Command c{std::move(legacy)};
    c.generation = generation;
    post_command(std::move(c));
}
void SimulationController::set_audio_master_volume(uint32_t percent) {
    audio_master_volume_ = std::min(percent, 100u);
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioMasterVolume;
    legacy.value1 = audio_master_volume_.load();
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
bool SimulationController::set_audio_sample_rate(uint32_t sample_rate, std::string &error) {
    sample_rate = std::clamp(sample_rate, 8'000u, 384'000u);
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioSampleRate;
    legacy.value1 = sample_rate;
    Command command{std::move(legacy)};
    Reply reply = submit_and_wait(std::move(command), std::chrono::seconds(5));
    if (reply.status == SRH_OK)
        audio_sample_rate_ = sample_rate;
    error = reply.error;
    return reply.status == SRH_OK;
}
bool SimulationController::set_audio_resampling(SrzAudioResampling method, std::string &error) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioResampling;
    legacy.value1 = method;
    Command command{std::move(legacy)};
    Reply reply = submit_and_wait(std::move(command), std::chrono::seconds(5));
    if (reply.status == SRH_OK)
        audio_resampling_ = method;
    error = reply.error;
    return reply.status == SRH_OK;
}
void SimulationController::set_audio_source_volume(Handle source, uint32_t percent, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioSourceVolume;
    legacy.handle1 = source;
    legacy.value1 = std::min(percent, 150u);
    Command c{std::move(legacy)};
    c.generation = generation;
    post_command(std::move(c));
}
void SimulationController::set_audio_source_pan(Handle source, int32_t pan, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioSourcePan;
    legacy.handle1 = source;
    legacy.value1 = static_cast<uint64_t>(std::clamp(pan, -64, 63) + 64);
    Command c{std::move(legacy)};
    c.generation = generation;
    post_command(std::move(c));
}
void SimulationController::set_audio_source_muted(Handle source, bool muted, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioSourceMuted;
    legacy.handle1 = source;
    legacy.flag = muted;
    Command c{std::move(legacy)};
    c.generation = generation;
    post_command(std::move(c));
}
void SimulationController::set_audio_software_clipping(bool enabled) {
    audio_software_clipping_ = enabled;
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioSoftwareClipping;
    legacy.flag = enabled;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
void SimulationController::set_audio_dc_offset_correction(bool enabled) {
    audio_dc_offset_correction_ = enabled;
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioDcOffsetCorrection;
    legacy.flag = enabled;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
void SimulationController::set_audio_queue_capacity(uint64_t frames) {
    audio_queue_capacity_frames_ = frames;
    LegacyCommand legacy;
    legacy.kind = CmdKind::AudioQueueCapacity;
    legacy.value1 = frames;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
void SimulationController::log_message(const std::string &message) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Log;
    legacy.text1 = message;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}
void SimulationController::set_video_visible(bool visible) {
    if (video_visible_.exchange(visible) != visible && visible)
        force_video_publish_ = true;
}
void SimulationController::set_inspection_visible(bool visible) {
    if (inspection_visible_.exchange(visible) != visible && visible)
        force_inspection_publish_ = true;
}
void SimulationController::set_inspection_interest(uint32_t interests) {
    if (inspection_interest_.exchange(interests) != interests)
        force_inspection_publish_ = true;
}
void SimulationController::set_snapshot_cadence(uint64_t normal_interval_ms,
                                                uint64_t high_clock_interval_ms,
                                                uint32_t threshold_hz) {
    normal_interval_ms_ = std::max<uint64_t>(16, normal_interval_ms);
    high_clock_interval_ms_ = std::max<uint64_t>(normal_interval_ms_.load(), high_clock_interval_ms);
    high_clock_threshold_hz_ = threshold_hz;
}
void SimulationController::request_manual_refresh() {
    manual_refresh_ = true;
    force_control_publish_ = true;
    force_inspection_publish_ = true;
    force_video_publish_ = true;
}

bool SimulationController::load_project_json(const std::string &json,
                                             const std::filesystem::path &project_dir,
                                             std::string &error) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::LoadProjectJson;
    legacy.text1 = json;
    legacy.path1 = project_dir;
    Command c{std::move(legacy)};
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(10));
    error = r.error;
    return r.status == SRH_OK;
}

bool SimulationController::new_project(std::string &error) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::NewProject;
    Command c{std::move(legacy)};
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(10));
    error = r.error;
    return r.status == SRH_OK;
}

bool SimulationController::restore_state(const std::string &project_json,
                                         const std::string &project_dir,
                                         const std::string &execution_json, bool pause_after,
                                         std::string &error) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::RestoreState;
    legacy.text1 = project_json;
    legacy.path1 = project_dir;
    legacy.text2 = execution_json;
    legacy.flag = pause_after;
    Command c{std::move(legacy)};
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(10));
    error = r.error;
    return r.status == SRH_OK;
}

bool SimulationController::save_state(bool include_trace, std::string &json, std::string &error) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::SaveState;
    legacy.flag = include_trace;
    Command c{std::move(legacy)};
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(10));
    error = r.error;
    json = r.text1;
    return r.status == SRH_OK;
}

bool SimulationController::park(Handle card, std::string &error, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Park;
    legacy.handle1 = card;
    Command c{std::move(legacy)};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    error = r.error;
    return r.status == SRH_OK;
}
bool SimulationController::plug(Handle card, std::string &error, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Plug;
    legacy.handle1 = card;
    Command c{std::move(legacy)};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    error = r.error;
    return r.status == SRH_OK;
}
bool SimulationController::put_away(Handle card, std::string &error, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::PutAway;
    legacy.handle1 = card;
    Command c{std::move(legacy)};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    error = r.error;
    return r.status == SRH_OK;
}
bool SimulationController::remove_card(Handle card, std::string &error, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::Remove;
    legacy.handle1 = card;
    Command c{std::move(legacy)};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    error = r.error;
    return r.status == SRH_OK;
}

bool SimulationController::add_card(const CardRequest &request, Handle &new_card,
                                    std::string &error, uint64_t generation) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::AddCard;
    legacy.card = request;
    Command c{std::move(legacy)};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(10));
    error = r.error;
    new_card = r.handle1;
    return r.status == SRH_OK;
}

std::vector<UiProperty> SimulationController::request_properties(Handle card, uint64_t generation) {
    Command c{Properties{.card = card}};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    return std::move(r.properties);
}

SrhStatus SimulationController::request_edit(Handle card, uint32_t index, const SrhValue &value,
                                             std::string &error, uint64_t generation) {
    Command c{EditProperty{.card = card, .index = index, .value = value}};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    error = r.error;
    return r.status;
}

std::vector<MemoryCell> SimulationController::request_memory(Handle space, uint64_t base,
                                                             uint32_t length, uint64_t generation) {
    Command c{ReadMemory{.space = space, .base = base, .length = length}};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    return std::move(r.memory);
}

SimulationController::MemoryLoadResult SimulationController::load_memory(
    Handle space, std::vector<MemorySegment> segments, bool reset, uint64_t generation) {
    Command c{LoadMemory{.space = space, .segments = std::move(segments), .reset = reset}};
    c.generation = generation;
    auto reply = submit_and_wait(std::move(c), std::chrono::seconds(10));
    return {reply.status, std::move(reply.error), reply.memory_load.failed_segment, reply.memory_load.written};
}

SrhStatus SimulationController::request_write_memory(Handle space, uint64_t base,
                                                     const std::vector<uint8_t> &bytes,
                                                     std::string &error, uint64_t generation) {
    Command c{WriteMemory{.space = space, .base = base, .bytes = bytes}};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    error = r.error;
    return r.status;
}

std::vector<DisasmLine> SimulationController::request_disassembly(Handle card, Handle space,
                                                                  uint64_t start, uint32_t count, uint64_t generation,
                                                                  uint32_t backward_count) {
    Command c{DisassembleRange{.card = card, .space = space, .start = start, .count = count, .backward_count = backward_count}};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    return std::move(r.disasm);
}

std::vector<uint8_t> SimulationController::request_text(Handle card, SrhStatus &status, uint64_t generation) {
    Command c{TextQuery{.card = card}};
    c.generation = generation;
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    status = r.status;
    return std::move(r.bytes);
}

std::string SimulationController::config_get(const std::string &key, const std::string &fallback) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigGet;
    legacy.text1 = key;
    legacy.text2 = fallback;
    Command c{std::move(legacy)};
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    return r.status == SRH_OK ? r.text1 : fallback;
}

std::string SimulationController::config_entry_get(const std::string &key,
                                                   const std::string &fallback) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigEntryGet;
    legacy.text1 = key;
    Command c{std::move(legacy)};
    Reply r = submit_and_wait(std::move(c), std::chrono::seconds(5));
    return r.status == SRH_OK ? r.text1 : fallback;
}

SrhStatus SimulationController::config_entry_set(const std::string &key,
                                                  const std::string &value) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigEntrySet;
    legacy.text1 = key;
    legacy.text2 = value;
    Command c{std::move(legacy)};
    return submit_and_wait(std::move(c), std::chrono::seconds(5)).status;
}

void SimulationController::config_set(const std::string &key, const std::string &value) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigSet;
    legacy.text1 = key;
    legacy.text2 = value;
    Command c{std::move(legacy)};
    (void)submit_and_wait(std::move(c), std::chrono::seconds(5));
}

void SimulationController::post_config_set(const std::string &key, const std::string &value) {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigSet;
    legacy.text1 = key;
    legacy.text2 = value;
    Command c{std::move(legacy)};
    post_command(std::move(c));
}

void SimulationController::config_load() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigLoad;
    Command c{std::move(legacy)};
    (void)submit_and_wait(std::move(c), std::chrono::seconds(5));
}

void SimulationController::post_config_save() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigSave;
    post_command(Command{std::move(legacy)});
}

void SimulationController::config_save() {
    LegacyCommand legacy;
    legacy.kind = CmdKind::ConfigSave;
    Command c{std::move(legacy)};
    (void)submit_and_wait(std::move(c), std::chrono::seconds(5));
}

SrhStatus SimulationController::provider_command(Handle owner, uint64_t generation, uint32_t kind,
                                              uint64_t revision, const std::string &payload) {
    if (kind > 1 || payload.size() >= SRH_PROVIDER_MAX_BYTES)
        return SRH_INVALID;
    LegacyCommand legacy;
    legacy.kind = CmdKind::ProviderCommand;
    legacy.handle1 = owner;
    legacy.clock = kind;
    legacy.value1 = revision;
    legacy.text1 = payload;
    // Interactive providers need an authoritative result in the click frame.
    // The 30 ms ceiling keeps a stalled simulation from turning a control
    // click into an unbounded UI wait.
    Command c{std::move(legacy)};
    c.generation = generation;
    return submit_and_wait(std::move(c), kind == 0 ? std::chrono::milliseconds(30) : std::chrono::seconds(5)).status;
}

nlohmann::json SimulationController::plugin_project_data() {
    LegacyCommand legacy; legacy.kind = CmdKind::PluginProjectData;
    Command c{std::move(legacy)};
    const auto reply = submit_and_wait(std::move(c), std::chrono::seconds(5));
    if (reply.status != SRH_OK) throw std::runtime_error(reply.error);
    return reply.json;
}

}
