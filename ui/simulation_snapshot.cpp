#include "simulation_controller.hpp"
#include "simulation_engine.hpp"
#include <algorithm>
#include <cassert>
#include <fstream>
#include <stdexcept>

namespace srz80::ui {
namespace {
UiRunState to_ui_run_state(SrzRunState state) {
    switch (state) {
    case SRZ_STOPPED:
        return UiRunState::stopped;
    case SRZ_RUNNING:
        return UiRunState::running;
    default:
        return UiRunState::paused;
    }
}

UiTimeMode to_ui_time_mode(SrzTimeMode mode) {
    switch (mode) {
    case SRZ_TIME_FIXED:
        return UiTimeMode::fixed;
    case SRZ_TIME_SYSTEM:
        return UiTimeMode::system;
    default:
        return UiTimeMode::project;
    }
}

std::string engine_string(const SrzEngine *engine, uint64_t (*query)(const SrzEngine *, char *, uint64_t)) {
    if (!engine)
        return {};
    std::vector<char> buffer(128);
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        const uint64_t length = query(engine, buffer.data(), buffer.size());
        if (length < buffer.size())
            return std::string(buffer.data(), static_cast<size_t>(length));
        buffer.assign(static_cast<size_t>(length) + 1, '\0');
    }
    return {};
}

UiCardInfo card_of(const SrzCardInfo &card) {
    UiCardInfo out;
    out.id = card.id;
    out.type = engine_text(card.type);
    out.name = engine_text(card.name);
    out.priority = card.priority;
    out.active = card.active != 0;
    out.parked = card.parked != 0;
    out.load_error = engine_text(card.load_error);
    return out;
}

UiClock clock_of(const SrzClock &clock) {
    UiClock out;
    out.hz = clock.hz;
    out.ticks = clock.ticks;
    out.phase = clock.phase;
    out.order = clock.order;
    return out;
}
} // namespace

void SimulationController::publish_due() {
    if (!engine_)
        return;
    // Publication copies everything it keeps out of the engine, so the shared
    // arena is reused instead of growing for every frame.
    srz80_engine_result_clear(result_);
    const auto now = std::chrono::steady_clock::now();

    std::array<SrzClock, 3> clocks{};
    uint32_t clock_count = 0;
    if (srz80_engine_clocks(engine_, clocks.data(), static_cast<uint32_t>(clocks.size()),
                            &clock_count) != SRH_OK)
        return;
    const bool high_clock = clocks[0].hz > high_clock_threshold_hz_.load() ||
                            clocks[1].hz > high_clock_threshold_hz_.load() ||
                            clocks[2].hz > high_clock_threshold_hz_.load();
    const auto interval = std::chrono::milliseconds(
        high_clock ? high_clock_interval_ms_.load() : normal_interval_ms_.load());
    const auto control_interval = std::chrono::milliseconds(
        std::min<uint64_t>(normal_interval_ms_.load(), 50));

    bool publish_control = force_control_publish_.exchange(false) ||
                           (now - last_control_ >= control_interval && control_dirty_.exchange(false));
    bool publish_inspection =
        force_inspection_publish_.exchange(false) ||
        (inspection_visible_ && now - last_inspection_ >= interval && inspection_dirty_.exchange(false));
    bool publish_video = video_visible_ &&
        (force_video_publish_.exchange(false) ||
         (now - last_video_ >= interval && video_dirty_.exchange(false)));
    const bool manual = manual_refresh_.exchange(false);
    if (manual) {
        publish_control = true;
        publish_inspection = true;
        publish_video = video_visible_;
    }
    // Interactive peripherals must not inherit the debugger's high-clock throttle.
    const bool publish_providers = force_provider_publish_.exchange(false) || publish_inspection ||
        ((ui_visible_ || std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() < provider_interest_until_) && engine_running() && latest_ && !latest_->providers->empty() &&
         now - last_provider_publish_ >= std::chrono::milliseconds(16));
    if (!publish_control && !publish_inspection && !publish_video && !publish_providers)
        return;

    auto snapshot = std::make_shared<UiSnapshot>();
    snapshot->sequence = ++snapshot_sequence_;
    snapshot->generation = generation_;
    if (const char *version = srz80_engine_version())
        snapshot->engine_version = version;
    if (srz80_engine_cards(engine_, result_) == SRH_OK) {
        uint32_t count = 0;
        const auto *cards = srz80_engine_result_cards(result_, &count);
        for (uint32_t i = 0; i < count; ++i) {
            SrzDisassemblyAvailability availability{};
            if (srz80_engine_disassembly_availability(engine_, cards[i].id, &availability) == SRH_OK)
                snapshot->disassembly[cards[i].id] = {availability.state, availability.revision, availability.message};
        }
    }
    snapshot->published_at = now;
    snapshot->run_state = to_ui_run_state(srz80_engine_run_state(engine_));
    snapshot->stop_reason = engine_string(engine_, srz80_engine_stop_reason);
    snapshot->now = srz80_engine_now(engine_);
    snapshot->time_mode = to_ui_time_mode(srz80_engine_time_mode(engine_));
    for (uint32_t index = 0; index < clock_count; ++index)
        snapshot->clocks[index] = clock_of(clocks[index]);
    snapshot->simulation_load_percent = simulation_load_percent_;
    snapshot->command_ack_seq = command_ack_seq_.load();
    { std::lock_guard lock(command_mutex_); snapshot->command_queue_depth = commands_.size(); }
    snapshot->command_latency_us = command_latency_us_;
    snapshot->slice_wall_us = slice_wall_us_;
    snapshot->discarded_wall_ns = discarded_wall_ns_;
    {
        std::lock_guard lock(error_mutex_);
        snapshot->last_command_error = last_command_error_;
    }
    {
        SrzAudioDiagnostics diagnostics{};
        if (srz80_engine_audio_diagnostics(engine_, &diagnostics) == SRH_OK) {
            snapshot->audio = {diagnostics.queued_frames,
                               diagnostics.queue_capacity_frames,
                               diagnostics.dropped_frames,
                               diagnostics.underflow_frames,
                               diagnostics.source_errors,
                               pcm_.available(),
                               pcm_.dropped_frames(),
                               pcm_.underflow_frames()};
        }
    }
    snapshot->audio_master_volume = srz80_engine_audio_master_volume(engine_);
    srz80_engine_audio_master_levels(engine_, &snapshot->audio_master_levels[0],
                                    &snapshot->audio_master_levels[1]);
    snapshot->audio_dc_offset_correction = srz80_engine_audio_dc_offset_correction(engine_) != 0;
    snapshot->audio_software_clipping = srz80_engine_audio_software_clipping(engine_) != 0;
    if (srz80_engine_audio_sources(engine_, result_) == SRH_OK) {
        uint32_t count = 0;
        const SrzAudioSource *sources = srz80_engine_result_audio_sources(result_, &count);
        snapshot->audio_sources.reserve(count);
        for (uint32_t index = 0; index < count; ++index) {
            const auto &source = sources[index];
            UiAudioSourceInfo info;
            info.id = source.id;
            info.owner = source.owner;
            info.name = engine_text(source.name);
            info.volume_percent = source.volume_percent;
            info.pan = srz80_engine_audio_source_pan(engine_, source.id);
            info.muted = source.muted != 0;
            info.active = source.active != 0;
            info.level_peak = source.level_peak;
            snapshot->audio_sources.push_back(std::move(info));
        }
    }

    if (publish_providers) {
        auto providers = std::make_shared<std::map<Handle, UiProviderData>>();
        if (srz80_engine_provider_data(engine_, result_) == SRH_OK) {
            uint32_t count = 0;
            const SrzProviderData *list = srz80_engine_result_providers(result_, &count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto &provider = list[index];
                UiProviderData data;
                data.name = engine_text(provider.name);
                data.protocol = engine_text(provider.protocol);
                data.data = provider.data
                                ? std::string(provider.data, static_cast<size_t>(provider.data_size))
                                : std::string();
                data.flags = provider.flags;
                providers->emplace(provider.owner, std::move(data));
            }
        }
        snapshot->providers = std::move(providers);
        snapshot->provider_sequence = snapshot->sequence;
        last_provider_publish_ = now;
    } else if (latest_) {
        snapshot->providers = latest_->providers;
        snapshot->provider_sequence = latest_->provider_sequence;
    }
    if (publish_inspection) {
        auto inspection = std::make_shared<UiInspectionSnapshot>();
        inspection->sequence = snapshot->sequence;
        inspection->published_at = now;
        inspection->cards = copy_cards(false);
        inspection->all_cards = copy_cards(true);
        if (srz80_engine_removed_cards(engine_, result_) == SRH_OK) {
            uint32_t count = 0;
            const SrzCardInfo *list = srz80_engine_result_removed_cards(result_, &count);
            inspection->removed_cards.reserve(count);
            for (uint32_t index = 0; index < count; ++index)
                inspection->removed_cards.push_back(card_of(list[index]));
        }
        inspection->spaces = copy_spaces();
        if (srz80_engine_breakpoints(engine_, result_) == SRH_OK) {
            uint32_t count = 0;
            const SrzBreakpoint *list = srz80_engine_result_breakpoints(result_, &count);
            inspection->breakpoints.reserve(count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto &point = list[index];
                UiBreakpoint out;
                out.id = point.id;
                out.card = point.card;
                out.space = point.space;
                out.first = point.first;
                out.last = point.last;
                out.operations = point.operations;
                out.enabled = point.enabled != 0;
                inspection->breakpoints.push_back(out);
            }
        }
        // Hidden panels retain their history in the engine, without copying it.
        // Reopening a panel or manual refresh requests a new publication.
        const auto interests = manual ? InspectAll : inspection_interest_.load();
        if (interests & InspectTrace) {
            if (srz80_engine_trace(engine_, result_) == SRH_OK) {
                uint32_t count = 0;
                const SrzTrace *list = srz80_engine_result_trace(result_, &count);
                uint32_t handle_count = 0;
                const SrhHandle *handles = srz80_engine_result_handles(result_, &handle_count);
                inspection->trace.reserve(count);
                for (uint32_t index = 0; index < count; ++index) {
                    const auto &trace = list[index];
                    UiTrace out;
                    out.sequence = trace.sequence;
                    out.parent = trace.parent;
                    out.time = trace.time;
                    out.ticks = {trace.ticks[0], trace.ticks[1], trace.ticks[2]};
                    out.master = trace.master;
                    out.space = trace.space;
                    out.address = trace.address;
                    out.operation = trace.operation;
                    out.depth = trace.depth;
                    out.kind = trace.kind;
                    out.instruction = trace.instruction;
                    out.value = trace.value;
                    out.result = trace.result;
                    if (handles && trace.responder_offset <= handle_count &&
                        trace.responder_count <= handle_count - trace.responder_offset)
                        out.responders.assign(
                            handles + trace.responder_offset,
                            handles + trace.responder_offset + trace.responder_count);
                    inspection->trace.push_back(std::move(out));
                }
            }
        }
        inspection->dropped = srz80_engine_dropped(engine_);
        if (interests & InspectLogs) {
            if (srz80_engine_logs(engine_, result_) == SRH_OK) {
                uint32_t count = 0;
                const char *const *list = srz80_engine_result_logs(result_, &count);
                inspection->logs.reserve(count);
                for (uint32_t index = 0; index < count; ++index)
                    inspection->logs.push_back(engine_text(list[index]));
            }
        }
        if (srz80_engine_text_endpoints(engine_, result_) == SRH_OK) {
            uint32_t count = 0;
            const SrzTextEndpoint *list = srz80_engine_result_text_endpoints(result_, &count);
            uint32_t name_count = 0;
            const char *const *names = srz80_engine_result_input_names(result_, &name_count);
            inspection->text_endpoints.reserve(count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto &endpoint = list[index];
                UiTextEndpoint out;
                out.card = endpoint.card;
                if (names && endpoint.input_offset <= name_count &&
                    endpoint.input_count <= name_count - endpoint.input_offset)
                    for (uint32_t item = 0; item < endpoint.input_count; ++item)
                        out.inputs.push_back(
                            engine_text(names[endpoint.input_offset + item]));
                inspection->text_endpoints.push_back(std::move(out));
            }
        }
        if (srz80_engine_config_entries(engine_, result_) == SRH_OK) {
            uint32_t count = 0;
            const SrzConfigEntry *list = srz80_engine_result_config_entries(result_, &count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto &entry = list[index];
                const std::string name = engine_text(entry.name);
                if (std::any_of(inspection->config_entries.begin(), inspection->config_entries.end(),
                                [&](const auto &copy) { return copy.name == name; }))
                    continue;
                UiConfigEntry out;
                out.category = engine_text(entry.category);
                out.name = name;
                out.label = engine_text(entry.label);
                out.description = engine_text(entry.description);
                out.enum_labels = engine_text(entry.enum_labels);
                out.default_value = engine_text(entry.default_value);
                out.provider = engine_text(entry.provider);
                out.type = entry.type;
                out.owner = entry.owner;
                inspection->config_entries.push_back(std::move(out));
            }
        }
        for (const auto &card : inspection->all_cards) {
            if (!card.active || !(interests & InspectProperties))
                continue;
            // A card that rejects the property query keeps no entry at all, so
            // panels cannot mistake an empty vector for "no properties".
            if (srz80_engine_properties(engine_, card.id, result_) != SRH_OK)
                continue;
            auto &properties = inspection->properties[card.id];
            uint32_t count = 0;
            const SrzProperty *list = srz80_engine_result_properties(result_, &count);
            properties.reserve(count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto &property = list[index];
                UiProperty out;
                out.name = engine_text(property.name);
                out.group = engine_text(property.group);
                out.description = engine_text(property.description);
                out.enum_labels = engine_text(property.enum_labels);
                out.kind = property.kind;
                out.bits = property.bits;
                out.base = property.base;
                out.ui_flags = property.ui_flags;
                out.editable = property.editable != 0;
                out.value = property.value;
                properties.push_back(std::move(out));
            }
        }
        if (srz80_engine_video_surfaces(engine_, result_) == SRH_OK) {
            uint32_t count = 0;
            const SrzVideoSurface *list = srz80_engine_result_video_surfaces(result_, &count);
            inspection->video_surfaces.reserve(count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto &surface = list[index];
                UiVideoSurface out;
                out.id = surface.id;
                out.owner = surface.owner;
                out.width = surface.width;
                out.height = surface.height;
                out.format = surface.format;
                out.flags = surface.flags;
                inspection->video_surfaces.push_back(out);
            }
        }
        snapshot->inspection = std::move(inspection);
    } else if (latest_) {
        snapshot->inspection = latest_->inspection;
    }

    if (publish_video) {
        auto video = std::make_shared<UiVideoSnapshot>();
        video->sequence = snapshot->sequence;
        video->published_at = now;
        if (srz80_engine_video_surfaces(engine_, result_) == SRH_OK) {
            uint32_t count = 0;
            const SrzVideoSurface *list = srz80_engine_result_video_surfaces(result_, &count);
            for (uint32_t index = 0; index < count; ++index) {
                const auto &surface = list[index];
                UiVideoSnapshot::VideoFrame frame;
                frame.surface = surface.id;
                const uint64_t pixels = uint64_t{surface.width} * surface.height;
                if (pixels > 16 * 1024 * 1024)
                    continue;
                const uint64_t bytes = pixels * 4;
                auto rgba = std::make_shared<std::vector<uint8_t>>(static_cast<size_t>(bytes));
                uint32_t size = static_cast<uint32_t>(rgba->size());
                uint32_t total = 0;
                frame.status =
                    srz80_engine_video_read(engine_, surface.id, 0, rgba->data(), &size, &total);
                frame.total = total;
                if (frame.status == SRH_OK && size == bytes && total == bytes) {
                    SrhVideoTiming timing{SRH_INIT(SrhVideoTiming), 0, 0, 0};
                    if (srz80_engine_video_timing(engine_, surface.id, &timing) == SRH_OK) {
                        frame.scanout_frame = timing.frame_number;
                        frame.scanout_line = timing.scanline;
                        frame.scanout_lines = timing.line_count;
                        frame.has_scanout = true;
                    }
                }
                frame.rgba = std::move(rgba);
                video->video_frames[surface.id] = std::move(frame);
            }
        }
        snapshot->video = std::move(video);
    } else if (latest_ && latest_->generation == generation_) {
        snapshot->video = latest_->video;
    }

    if (publish_control)
        last_control_ = now;
    if (publish_inspection)
        last_inspection_ = now;
    if (publish_video)
        last_video_ = now;

    snapshot->snapshot_copy_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - now).count());
    {
        std::lock_guard lock(snapshot_mutex_);
        latest_ = std::move(snapshot);
    }
}

} // namespace srz80::ui
