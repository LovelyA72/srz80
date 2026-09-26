#pragma once
#include "ui_types.hpp"
#include <chrono>
#include <map>
#include <memory>

namespace srz80::ui {
// Owned byte/status pair returned by read requests.  No pointer or reference
// into the engine survives the request.
struct MemoryCell {
    uint8_t value = 0;
    SrhStatus status = SRH_UNAVAILABLE;
    bool available() const { return status == SRH_OK; }
};

struct UiDisassemblyAvailability {
    uint32_t state = 0;
    uint64_t revision = 0;
    std::string message;
    bool accepts(uint64_t requested_revision, uint64_t reply_revision) const {
        // State value 1 is the engine ABI's enabled state. This UI-owned value
        // carries no engine pointers and is also used by native controller tests.
        return state == 1 && revision == requested_revision && revision == reply_revision;
    }
};

struct DisasmLine {
    uint64_t address = 0;
    std::vector<uint8_t> bytes;
    uint32_t instruction_bytes = 1;
    uint32_t cycles = 0;
    std::string text;
    bool ok = false;
};

struct UiAudioDiagnostics {
    uint64_t queued_frames = 0;
    uint64_t queue_capacity_frames = 0;
    uint64_t dropped_frames = 0;
    uint64_t underflow_frames = 0;
    uint64_t source_errors = 0;
    uint64_t pcm_queued_frames = 0;
    uint64_t pcm_dropped_frames = 0;
    uint64_t pcm_underflow_frames = 0;
};

// Expensive publication classes are shared unchanged between control updates.
struct UiInspectionSnapshot {
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point published_at{};
    std::vector<UiCardInfo> cards;         // active, pluggable cards
    std::vector<UiCardInfo> all_cards;     // includes parked/removed
    std::vector<UiCardInfo> removed_cards; // parked cards only
    std::map<Handle, UiSpace> spaces;
    std::vector<UiBreakpoint> breakpoints;
    std::vector<UiTrace> trace;
    uint64_t dropped = 0;
    std::vector<std::string> logs;
    std::map<Handle, std::vector<UiProperty>> properties;
    // Plugin setting metadata only. Callback pointers never cross from the
    // simulation thread into the UI thread.
    std::vector<UiConfigEntry> config_entries;
    std::vector<UiTextEndpoint> text_endpoints;

    std::vector<UiVideoSurface> video_surfaces;
};
struct UiVideoSnapshot {
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point published_at{};
    struct VideoFrame {
        Handle surface = 0;
        std::shared_ptr<const std::vector<uint8_t>> rgba;
        uint32_t total = 0;
        SrhStatus status = SRH_UNAVAILABLE;
        // Card scanout position captured alongside these pixels, never derived
        // from snapshot publication or GUI refresh counts.
        uint64_t scanout_frame = 0;
        uint32_t scanout_line = 0, scanout_lines = 0;
        bool has_scanout = false;
    };
    std::map<Handle, VideoFrame> video_frames;

};

// Immutable UI snapshot.  Everything here is owned by this structure. Panels
// may retain a snapshot after the simulation advances.
struct UiSnapshot {
    uint64_t sequence = 0;
    uint64_t generation = 0;
    std::chrono::steady_clock::time_point published_at{};
    // Copied by the simulation thread from the engine ABI. The GUI never
    // enters the engine directly, even for a constant query.
    std::string engine_version;

    std::map<Handle, UiDisassemblyAvailability> disassembly;

    UiRunState run_state = UiRunState::stopped;
    std::string stop_reason = "Stopped";
    uint64_t now = 0;
    UiTimeMode time_mode = UiTimeMode::project;
    std::array<UiClock, 3> clocks{};
    // Smoothed execution cost: wall time inside the engine run slice divided by
    // simulated time actually advanced (not the wall-clock slice target).
    // 100% = exactly real time. Above 100% = falling behind. Below = headroom.
    uint32_t simulation_load_percent = 0;

    std::shared_ptr<const UiInspectionSnapshot> inspection = std::make_shared<UiInspectionSnapshot>();

    uint64_t provider_sequence = 0;
    std::shared_ptr<const std::map<Handle, UiProviderData>> providers =
        std::make_shared<const std::map<Handle, UiProviderData>>();
    UiAudioDiagnostics audio;
    std::vector<UiAudioSourceInfo> audio_sources;
    uint32_t audio_master_volume = 100;
    std::array<uint32_t, 2> audio_master_levels{};
    bool audio_dc_offset_correction = true;
    bool audio_software_clipping = false;

    std::shared_ptr<const UiVideoSnapshot> video = std::make_shared<UiVideoSnapshot>();

    uint64_t command_ack_seq = 0;
    uint64_t command_queue_depth = 0;
    uint64_t command_latency_us = 0;
    uint64_t slice_wall_us = 0;
    uint64_t discarded_wall_ns = 0;
    uint64_t snapshot_copy_us = 0;
    std::string last_command_error;

    bool paused() const { return run_state != UiRunState::running; }
    bool stopped() const { return run_state == UiRunState::stopped; }
    bool high_clock(uint32_t threshold_hz) const {
        for (const auto &clock : clocks)
            if (clock.hz > threshold_hz)
                return true;
        return false;
    }
};

}
