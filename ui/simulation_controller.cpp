#include "simulation_controller.hpp"
#include "simulation_pacing.hpp"
#include "platform_paths.hpp"
#include "simulation_engine.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace srz80::ui {

SrzSlice engine_slice(const std::string &text) { return SrzSlice{text.data(), text.size()}; }

std::string engine_path_text(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

std::string engine_text(const char *value) { return value ? std::string(value) : std::string(); }

// Reads an engine-produced string into owned storage, growing once when the
// first probe was too small.
namespace {
template <class F> std::string query_text(F &&query) {
    std::vector<char> buffer(256);
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const uint64_t length = query(buffer.data(), buffer.size());
        if (length < buffer.size())
            return std::string(buffer.data(), static_cast<size_t>(length));
        buffer.assign(static_cast<size_t>(length) + 1, '\0');
    }
    return {};
}
} // namespace

std::string engine_last_message(const SrzEngine *engine) {
    if (!engine)
        return "Simulation engine is unavailable";
    return query_text([&](char *buffer, uint64_t capacity) {
        return srz80_engine_last_error(engine, buffer, capacity);
    });
}

std::string engine_config_value(const SrzEngine *engine, const std::string &key,
                                const std::string &fallback) {
    if (!engine)
        return fallback;
    return query_text([&](char *buffer, uint64_t capacity) {
        return srz80_engine_config_value(engine, engine_slice(key), engine_slice(fallback), buffer,
                                         capacity);
    });
}

SrhStatus engine_config_entry(const SrzEngine *engine, const std::string &key, std::string &value) {
    value.clear();
    if (!engine)
        return SRH_UNAVAILABLE;
    uint64_t size = 0;
    const SrhStatus probe =
        srz80_engine_config_entry_get(engine, engine_slice(key), nullptr, 0, &size);
    if (probe != SRH_OK)
        return probe;
    std::vector<char> buffer(static_cast<size_t>(size) + 1, '\0');
    uint64_t written = 0;
    const SrhStatus status = srz80_engine_config_entry_get(engine, engine_slice(key), buffer.data(),
                                                           buffer.size(), &written);
    if (status != SRH_OK)
        return status;
    value.assign(buffer.data(), static_cast<size_t>(written));
    return SRH_OK;
}

namespace {
// The worker's run slice keeps a stalled callback from consuming the frame
// cadence. The engine measures this budget internally so no wall-clock value
// crosses the ABI.
constexpr uint64_t slice_wall_budget_ns = 2000000; // 2 ms

uint64_t checked_add(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a)
        return UINT64_MAX;
    return a + b;
}

std::string text_of(const char *value) { return value ? std::string(value) : std::string(); }

UiCardInfo to_ui(const SrzCardInfo &card) {
    UiCardInfo out;
    out.id = card.id;
    out.type = text_of(card.type);
    out.name = text_of(card.name);
    out.priority = card.priority;
    out.active = card.active != 0;
    out.parked = card.parked != 0;
    out.load_error = text_of(card.load_error);
    return out;
}

UiSpace to_ui(const SrzSpace &space) {
    UiSpace out;
    out.id = space.id;
    out.name = text_of(space.name);
    out.maximum = space.maximum;
    out.fallback = space.fallback;
    out.random = space.random != 0;
    out.resolver = space.resolver == SRZ_RESOLVER_BIT_OR ? UiResolver::bit_or : UiResolver::priority;
    return out;
}

} // namespace

std::filesystem::path SimulationController::executable_plugins_directory() {
    return srz80::executable_directory() / "plugins";
}

SimulationController::SimulationController(std::filesystem::path plugin_directory)
    : plugin_directory_(std::move(plugin_directory)) {}

SimulationController::~SimulationController() { shutdown(); }

void SimulationController::start() {
    if (thread_started_)
        return;
    shutdown_requested_ = false;
    { std::lock_guard lock(command_mutex_); accepting_commands_ = true; }
    thread_ = std::thread(&SimulationController::thread_main, this);
    thread_started_ = true;
}

void SimulationController::shutdown() {
    if (!thread_started_)
        return;
    {
        std::lock_guard lock(command_mutex_);
        shutdown_requested_ = true;
        accepting_commands_ = false;
    }
    command_cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    thread_started_ = false;
}

std::shared_ptr<const UiSnapshot> SimulationController::snapshot() const {
    std::lock_guard lock(snapshot_mutex_);
    if (!latest_) {
        // Publish a minimal initial snapshot without blocking.  The simulation
        // thread normally replaces this before the first UI frame.
        static const auto empty = std::make_shared<UiSnapshot>();
        return empty;
    }
    return latest_;
}

bool SimulationController::project_scoped(const CommandPayload &payload) {
    // Card-type discovery describes the host's plugin directory, not the
    // loaded project, so a generation change must not reject it.
    if (std::holds_alternative<CardTypes>(payload))
        return false;
    const auto *legacy = std::get_if<LegacyCommand>(&payload);
    if (!legacy) return true;
    switch (legacy->kind) {
    case CmdKind::LoadProjectJson: case CmdKind::NewProject: case CmdKind::RestoreState:
    case CmdKind::ConfigGet: case CmdKind::ConfigSet: case CmdKind::ConfigLoad:
    case CmdKind::ConfigSave: case CmdKind::Log:
    case CmdKind::AudioMasterVolume: case CmdKind::AudioSampleRate: case CmdKind::AudioResampling:
    case CmdKind::AudioQueueCapacity:
    case CmdKind::AudioSoftwareClipping: case CmdKind::AudioDcOffsetCorrection:
        return false;
    default:
        return true;
    }
}

bool SimulationController::requires_explicit_generation(const CommandPayload &payload) {
    const auto *legacy = std::get_if<LegacyCommand>(&payload);
    return std::holds_alternative<InputBatch>(payload) || std::holds_alternative<ProjectRuntimeRequest>(payload) ||
           (legacy && legacy->kind == CmdKind::ProviderCommand);
}

uint64_t SimulationController::post_command(Command command) {
    if (!command.generation && project_scoped(command.payload) && !requires_explicit_generation(command.payload))
        command.generation = snapshot()->generation;
    std::lock_guard lock(command_mutex_);
    const uint64_t seq = command.seq = next_seq_++;
    command.posted_at = std::chrono::steady_clock::now();
    if (!accepting_commands_) {
        Reply reply;
        reply.seq = seq;
        reply.status = SRH_UNAVAILABLE;
        reply.error = "Simulation controller is shut down";
        if (command.project_completion) {
            const auto &request = std::get<ProjectRuntimeRequest>(command.payload);
            reply.project.operation = request.operation;
            reply.project.expected_generation = request.generation;
            reply.project.status = reply.status;
            reply.project.error = reply.error;
            command.project_completion->set_value(std::move(reply.project));
        } else if (command.completion)
            command.completion->set_value(std::move(reply));
        else {
            std::lock_guard error_lock(error_mutex_);
            errors_.push_back({seq, command.generation, reply.status, reply.error});
        }
        return seq;
    }
    commands_.push_back(std::move(command));
    command_cv_.notify_one();
    return seq;
}

std::future<ProjectRuntimeResult> SimulationController::submit_project(ProjectRuntimeRequest request) {
    const auto generation = request.generation;
    Command command{std::move(request)};
    command.generation = generation;
    command.project_completion = std::make_shared<std::promise<ProjectRuntimeResult>>();
    auto future = command.project_completion->get_future();
    post_command(std::move(command));
    return future;
}

SimulationController::AsyncReply SimulationController::submit(Command command) {
    command.completion = std::make_shared<std::promise<Reply>>();
    auto future = command.completion->get_future();
    post_command(std::move(command));
    return future;
}

SimulationController::Reply SimulationController::submit_and_wait(Command command,
                                                                  std::chrono::milliseconds timeout) {
    auto future = submit(std::move(command));
    if (future.wait_for(timeout) == std::future_status::ready)
        return future.get();
    Reply reply;
    reply.status = SRH_UNAVAILABLE;
    reply.error = "Simulation request timed out; the queued operation may still complete";
    return reply;
}

std::vector<SimulationController::CommandError> SimulationController::take_errors() {
    std::lock_guard lock(error_mutex_);
    std::vector<CommandError> result;
    result.reserve(errors_.size());
    while (!errors_.empty()) {
        result.push_back(std::move(errors_.front()));
        errors_.pop_front();
    }
    return result;
}

SimulationController::AsyncReply SimulationController::input_cancel_async(uint64_t source, uint64_t identity, uint64_t generation) {
    Command command{InputBatch{{}, 0, {}, source, identity, true}};
    command.generation = generation;
    return submit(std::move(command));
}
SimulationController::AsyncReply SimulationController::input_release_async(
    std::string endpoint, std::vector<uint8_t> bytes, uint64_t generation,
    uint64_t source, uint64_t identity, Handle owner) {
    Command command{InputBatch{std::move(endpoint), UINT64_MAX, std::move(bytes), source, identity, true, owner, true}};
    command.generation = generation;
    return submit(std::move(command));
}
SimulationController::AsyncReply SimulationController::input_batch_async(
    std::string endpoint, uint64_t timestamp, std::vector<uint8_t> bytes, uint64_t generation, uint64_t source, uint64_t identity, Handle expected_owner) {
    Command command{InputBatch{std::move(endpoint), timestamp, std::move(bytes), source, identity, false, expected_owner}};
    command.generation = generation;
    return submit(std::move(command));
}
SimulationController::AsyncReply SimulationController::read_memory_async(
    Handle space, uint64_t base, uint32_t length, uint64_t generation) {
    Command c{ReadMemory{.space = space, .base = base, .length = length}};
    c.generation = generation;
    return submit(std::move(c));
}
SimulationController::AsyncReply SimulationController::disassemble_async(
    Handle card, Handle space, uint64_t start, uint32_t count, uint64_t generation, uint32_t backward_count) {
    Command c{DisassembleRange{.card = card, .space = space, .start = start, .count = count, .backward_count = backward_count}};
    c.generation = generation;
    return submit(std::move(c));
}
SimulationController::AsyncReply SimulationController::read_text_async(Handle card, uint64_t generation) {
    Command c{TextQuery{.card = card}};
    c.generation = generation;
    return submit(std::move(c));
}

SimulationController::AsyncReply SimulationController::discover_card_types_async() {
    return submit(Command{CardTypes{}});
}

uint32_t SimulationController::audio_available() const { return pcm_.available(); }
uint32_t SimulationController::audio_capacity() const { return pcm_.capacity(); }

uint32_t SimulationController::audio_read(int16_t *interleaved, uint32_t frames) {
    uint64_t epoch;
    return pcm_.pop(interleaved, frames, epoch);
}

uint32_t SimulationController::audio_read(int16_t *interleaved, uint32_t frames, uint64_t &epoch) {
    return pcm_.pop(interleaved, frames, epoch);
}
uint64_t SimulationController::audio_epoch() const { return pcm_.epoch(); }
void SimulationController::audio_recent(std::vector<int16_t> &interleaved, uint32_t frames) const {
    pcm_.recent(interleaved, frames);
}
void SimulationController::reset_audio() {
    pcm_.clear();
    input_present_.store(false);
    input_requested_.store(false);
    std::lock_guard lock(input_mutex_);
    input_pending_.clear();
}

std::string SimulationController::engine_message() const { return engine_last_message(engine_); }

bool SimulationController::engine_running() const {
    return engine_ && srz80_engine_run_state(engine_) == SRZ_RUNNING;
}

bool SimulationController::engine_stopped() const {
    return !engine_ || srz80_engine_run_state(engine_) == SRZ_STOPPED;
}

std::map<Handle, UiSpace> SimulationController::copy_spaces() {
    std::map<Handle, UiSpace> spaces;
    if (!engine_ || srz80_engine_spaces(engine_, result_) != SRH_OK)
        return spaces;
    uint32_t count = 0;
    const SrzSpace *list = srz80_engine_result_spaces(result_, &count);
    for (uint32_t index = 0; index < count; ++index)
        spaces.emplace(list[index].id, to_ui(list[index]));
    return spaces;
}

bool SimulationController::space_maximum(Handle space, uint64_t &maximum) {
    const auto spaces = copy_spaces();
    const auto found = spaces.find(space);
    if (found == spaces.end())
        return false;
    maximum = found->second.maximum;
    return true;
}

std::vector<UiCardInfo> SimulationController::copy_cards(bool all) {
    std::vector<UiCardInfo> cards;
    if (!engine_)
        return cards;
    const SrhStatus status = all ? srz80_engine_all_cards(engine_, result_)
                                 : srz80_engine_cards(engine_, result_);
    if (status != SRH_OK)
        return cards;
    uint32_t count = 0;
    const SrzCardInfo *list = all ? srz80_engine_result_all_cards(result_, &count)
                                  : srz80_engine_result_cards(result_, &count);
    cards.reserve(count);
    for (uint32_t index = 0; index < count; ++index)
        cards.push_back(to_ui(list[index]));
    return cards;
}

bool SimulationController::read_state(bool include_trace, std::string &json, std::string &error) {
    if (!engine_) {
        error = "Simulation engine is unavailable";
        return false;
    }
    uint64_t size = 0;
    if (srz80_engine_save_state(engine_, include_trace ? 1u : 0u, nullptr, 0, &size) != SRH_OK) {
        error = engine_message();
        return false;
    }
    std::vector<char> buffer(static_cast<size_t>(size) + 1, '\0');
    if (srz80_engine_save_state(engine_, include_trace ? 1u : 0u, buffer.data(), buffer.size(),
                                &size) != SRH_OK) {
        error = engine_message();
        return false;
    }
    json.assign(buffer.data(), static_cast<size_t>(size));
    return true;
}

void SimulationController::apply_runtime_options() {
    if (!engine_)
        return;
    srz80_engine_audio_set_sample_rate(engine_, audio_sample_rate_.load());
    srz80_engine_audio_set_resampling(engine_, audio_resampling_.load());
    srz80_engine_audio_set_queue_capacity(engine_, audio_queue_capacity_frames_.load());
    srz80_engine_audio_set_master_volume(engine_, audio_master_volume_.load());
    srz80_engine_audio_set_dc_offset_correction(
        engine_, audio_dc_offset_correction_.load() ? 1u : 0u);
    srz80_engine_audio_set_software_clipping(engine_,
                                             audio_software_clipping_.load() ? 1u : 0u);
    srz80_engine_set_trace_capture(engine_, trace_capture_requested_.load() ? 1u : 0u,
                                   trace_capture_operations_requested_.load());
}

bool SimulationController::install_candidate(SrzEngine *candidate,
                                             std::vector<ProjectCardAssociation> *cards,
                                             std::string &error) {
    struct CandidateGuard {
        SrzEngine *engine;
        ~CandidateGuard() {
            if (engine)
                srz80_engine_destroy(engine);
        }
    } guard{candidate};
    if (!candidate) {
        error = "Cannot create a project candidate";
        return false;
    }
    // Finish every fallible step -- runtime options and the association list the
    // session matches against -- before the active rack is touched.
    srz80_engine_audio_set_queue_capacity(candidate, audio_queue_capacity_frames_.load());
    srz80_engine_audio_set_master_volume(candidate, audio_master_volume_.load());
    srz80_engine_audio_set_dc_offset_correction(
        candidate, audio_dc_offset_correction_.load() ? 1u : 0u);
    srz80_engine_audio_set_software_clipping(candidate,
                                             audio_software_clipping_.load() ? 1u : 0u);
    srz80_engine_set_trace_capture(candidate, trace_capture_requested_.load() ? 1u : 0u,
                                   trace_capture_operations_requested_.load());
    if (cards) {
        cards->clear();
        if (srz80_engine_all_cards(candidate, result_) != SRH_OK) {
            error = engine_last_message(candidate);
            return false;
        }
        uint32_t count = 0;
        const SrzCardInfo *list = srz80_engine_result_all_cards(result_, &count);
        cards->reserve(count);
        for (uint32_t index = 0; index < count; ++index)
            cards->push_back({list[index].id, list[index].parked == 0,
                              text_of(list[index].load_error).empty()});
    }
    if (srz80_engine_replace(engine_, candidate) != SRH_OK) {
        error = engine_message();
        return false;
    }
    guard.engine = nullptr; // consumed by the replacement
    ++generation_;
    reset_audio();
    ++memory_revision_;
    force_video_publish_ = true;
    force_inspection_publish_ = true;
    force_control_publish_ = true;
    return true;
}

void SimulationController::thread_main() {
    try {
        if (latest_)
            ++generation_;
        last_control_ = last_inspection_ = last_video_ = last_provider_publish_ = {};
        const std::string plugins = engine_path_text(plugin_directory_);
        const SrzSlice plugins_slice{plugins.data(), plugins.size()};
        engine_ = srz80_engine_create(plugins_slice);
        if (!engine_)
            throw std::runtime_error("Cannot create the SRZ80 engine");
        result_ = srz80_engine_result_create(engine_);
        if (!result_)
            throw std::runtime_error("Cannot create the engine query arena");
        srz80_engine_set_trace_capture(engine_, 0, SRH_READ | SRH_WRITE);
        command_ack_seq_ = 0;
        force_control_publish_ = true;
        force_inspection_publish_ = true;
        publish_due();
        auto last_slice = std::chrono::steady_clock::now();
        SimulationPacing pacing;
        uint64_t pacing_generation = generation_;
        while (!shutdown_requested_) {
            drain_commands();
            drain_audio_input();
            if (shutdown_requested_)
                break;
            if (pacing_generation != generation_) {
                pacing.reset();
                pacing_generation = generation_;
                last_slice = std::chrono::steady_clock::now();
            }
            if (srz80_engine_run_state(engine_) != SRZ_RUNNING) {
                pacing.reset();
                publish_due();
                std::unique_lock lock(command_mutex_);
                command_cv_.wait_for(lock, std::chrono::milliseconds(20),
                                     [&] { return shutdown_requested_ || !commands_.empty(); });
                last_slice = std::chrono::steady_clock::now();
                continue;
            }

            const auto slice_start = std::chrono::steady_clock::now();
            const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(slice_start - last_slice).count();
            last_slice = slice_start;
            discarded_wall_ns_ += pacing.add_elapsed(static_cast<uint64_t>(std::max<int64_t>(elapsed, 0)));
            const uint64_t before_run = srz80_engine_now(engine_);
            const uint64_t target = checked_add(before_run, pacing.pending());
            const auto status = srz80_engine_run_slice(engine_, 100000, target, slice_wall_budget_ns);
            if (status != SRH_OK) {
                mark_command_error(engine_message());
                srz80_engine_pause(engine_);
            }
            const auto run_end = std::chrono::steady_clock::now();
            const auto run_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(run_end - slice_start).count();
            slice_wall_us_ = static_cast<uint64_t>(run_ns / 1000);
            const uint64_t after_run = srz80_engine_now(engine_);
            const auto advanced = after_run >= before_run ? after_run - before_run : 0;
            // Divide by work completed in simulated time, not the prior wall
            // interval: at saturation that interval grows with run cost and
            // falsely pins utilization just below 100%. Keep fractional smoothing
            // so rounding cannot trap the display several percentage points low.
            const auto deadline = slice_start + std::chrono::nanoseconds(slice_wall_budget_ns);
            const double instant = advanced ? std::min(999.0, 100.0 * run_ns / advanced)
                                            : (run_end >= deadline ? 999.0 : 0.0);
            smoothed_load_percent_ += (instant - smoothed_load_percent_) * 0.25;
            simulation_load_percent_ = static_cast<uint32_t>(std::lround(smoothed_load_percent_));
            pacing.advance(advanced);

            control_dirty_ = true;
            inspection_dirty_ = true;
            video_dirty_ = true;
            drain_commands();
            drain_audio();
            publish_due();
            if (pacing.pending())
                continue; // Catch up in another bounded slice before sleeping.
            std::unique_lock lock(command_mutex_);
            command_cv_.wait_until(lock, slice_start + std::chrono::milliseconds(1),
                                  [&] { return shutdown_requested_ || !commands_.empty(); });
        }
    } catch (const std::exception &e) {
        mark_command_error(e.what());
    } catch (...) {
        mark_command_error("Unexpected simulation worker failure");
    }
    // No callbacks or loaded card libraries survive the worker. Pending
    // request owners are woken with explicit cancellation replies.
    std::deque<Command> cancelled;
    { std::lock_guard lock(command_mutex_); accepting_commands_ = false; cancelled.swap(commands_); }
    for (auto &command : cancelled) {
        Reply reply;
        reply.status = SRH_UNAVAILABLE;
        reply.error = "Simulation shut down before executing request";
        finish_command(command, std::move(reply));
    }
    // The result arena only borrows the engine for its thread assertion, so it
    // is released first. Both handles die on the thread that created them.
    if (engine_ && result_) {
        srz80_engine_result_destroy(engine_, result_);
        result_ = nullptr;
    }
    srz80_engine_destroy(engine_);
    engine_ = nullptr;
    reset_audio();
}

void SimulationController::drain_commands() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
    for (unsigned count = 0; count < 64 && !shutdown_requested_; ++count) {
        Command command{LegacyCommand{}};
        {
            std::lock_guard lock(command_mutex_);
            if (commands_.empty())
                return;
            command = std::move(commands_.front());
            commands_.pop_front();
        }
        process_command(command);
        if (std::chrono::steady_clock::now() >= deadline)
            return;
    }
}

void SimulationController::mark_command_error(const std::string &error) {
    std::lock_guard lock(error_mutex_);
    last_command_error_ = error;
    errors_.push_back({0, generation_, SRH_ERROR, error});
}

void SimulationController::finish_command(Command &command, Reply reply) {
    reply.seq = command.seq;
    reply.generation = generation_;
    reply.memory_revision = memory_revision_;
    command_ack_seq_ = command.seq;
    command_latency_us_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - command.posted_at).count());
    control_dirty_ = true;
    if (reply.status != SRH_OK && reply.error.empty())
        reply.error = "Simulation command rejected (status " + std::to_string(reply.status) + ")";
    if (!reply.error.empty()) {
        std::lock_guard lock(error_mutex_);
        last_command_error_ = reply.error;
        if (!command.completion && !command.project_completion)
            errors_.push_back({command.seq, command.generation, reply.status, reply.error});
    }
    if (!shutdown_requested_ && (command.completion || command.project_completion) &&
        (force_control_publish_ || force_inspection_publish_ || force_video_publish_ || force_provider_publish_))
        publish_due();
    if (command.project_completion) {
        const auto &request = std::get<ProjectRuntimeRequest>(command.payload);
        reply.project.operation = request.operation;
        reply.project.expected_generation = request.generation;
        reply.project.generation = generation_;
        reply.project.status = reply.status;
        reply.project.error = std::move(reply.error);
        command.project_completion->set_value(std::move(reply.project));
    } else if (command.completion)
        command.completion->set_value(std::move(reply));
}

void SimulationController::drain_audio() {
    if (!engine_)
        return;
    SrzAudioDiagnostics diagnostics{};
    if (srz80_engine_audio_diagnostics(engine_, &diagnostics) != SRH_OK)
        return;
    if (!diagnostics.queued_frames)
        return;
    const uint32_t frames =
        static_cast<uint32_t>(std::min<uint64_t>(diagnostics.queued_frames, 8192));
    uint32_t got = srz80_engine_audio_read(engine_, audio_scratch_.data(), frames);
    if (got)
        pcm_.push(audio_scratch_.data(), got);
}

void SimulationController::audio_input_push(const float *samples, uint32_t frames,
                                             uint32_t rate, uint32_t channels) {
    std::lock_guard lock(input_mutex_);
    if (input_rate_ != rate || input_channels_ != channels || !frames)
        input_pending_.clear();
    input_rate_ = rate;
    input_channels_ = channels;
    if (!samples || !channels || channels > 8 || !input_requested_.load()) return;
    const size_t count = static_cast<size_t>(std::min(frames, 8192u)) * channels;
    const size_t limit = 8192u * channels;
    if (input_pending_.size() + count > limit)
        input_pending_.erase(input_pending_.begin(), input_pending_.begin() +
                             (input_pending_.size() + count - limit));
    input_pending_.insert(input_pending_.end(), samples, samples + count);
}

void SimulationController::drain_audio_input() {
    std::lock_guard lock(input_mutex_);
    const auto run_state = srz80_engine_run_state(engine_);
    bool present = false;
    if (run_state != SRZ_STOPPED && srz80_engine_cards(engine_, result_) == SRH_OK) {
        uint32_t count = 0;
        const SrzCardInfo *cards = srz80_engine_result_cards(result_, &count);
        for (uint32_t index = 0; index < count; ++index) {
            if (cards[index].active && cards[index].type &&
                std::strcmp(cards[index].type, "audio_input") == 0) {
                present = true;
                break;
            }
        }
    }
    const bool requested = run_state == SRZ_RUNNING &&
                           srz80_engine_audio_input_requested(engine_);
    if (!requested || input_generation_ != generation_) {
        input_pending_.clear();
        srz80_engine_audio_input_push(engine_, nullptr, 0, 0, 0);
    } else {
        srz80_engine_audio_input_push(engine_, input_pending_.data(),
            input_channels_ ? static_cast<uint32_t>(input_pending_.size() / input_channels_) : 0,
            input_rate_, input_channels_);
        input_pending_.clear();
    }
    input_generation_ = generation_;
    input_present_.store(present);
    input_requested_.store(requested);
}

} // namespace srz80::ui
