#pragma once
#include "ui_snapshot.hpp"
#include "project_runtime.hpp"
#include "pcm_queue.hpp"
#include <srz80/engine.h>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <variant>
#include <type_traits>
#include <utility>

namespace srz80::ui {

class SimulationController : public ProjectRuntime {
  public:
    struct MemoryLoadProgress {
        uint32_t failed_segment = UINT32_MAX;
        uint64_t written = 0;
    };

    struct Reply {
        uint64_t seq = 0;
        uint64_t generation = 0;
        uint64_t memory_revision = 0;
        SrhStatus status = SRH_OK;
        std::string error;
        std::string text1;
        nlohmann::json json = nlohmann::json::object();
        std::vector<uint8_t> bytes;
        std::vector<MemoryCell> memory;
        std::vector<DisasmLine> disasm;
        uint64_t disassembly_revision = 0;
        std::vector<UiProperty> properties;
        std::vector<UiCardType> card_types;
        MemoryLoadProgress memory_load;
        Handle handle1 = 0;
        ProjectRuntimeResult project;
    };

    std::future<ProjectRuntimeResult> submit_project(ProjectRuntimeRequest request) override;
    using AsyncReply = std::future<Reply>;
    AsyncReply input_batch_async(std::string endpoint, uint64_t timestamp,
                                 std::vector<uint8_t> bytes, uint64_t generation, uint64_t source = 0, uint64_t identity = 0, Handle expected_owner = 0);
    AsyncReply input_cancel_async(uint64_t source, uint64_t identity, uint64_t generation);
    // Atomically discard this source's pending events and enqueue releases for
    // state already delivered. Cleanup is accepted while paused/stopped too.
    AsyncReply input_release_async(std::string endpoint, std::vector<uint8_t> bytes,
                                    uint64_t generation, uint64_t source, uint64_t identity, Handle owner);
    void provider_interest() {
        provider_interest_until_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() + 1000;
    }
    AsyncReply read_memory_async(Handle space, uint64_t base, uint32_t length, uint64_t generation);
    AsyncReply disassemble_async(Handle card, Handle space, uint64_t start, uint32_t count, uint64_t generation, uint32_t backward_count = 0);
    AsyncReply read_text_async(Handle card, uint64_t generation);
    // Reads the engine's card plugin descriptors on the simulation thread and
    // returns owned add-card metadata.  The GUI never loads a card library.
    AsyncReply discover_card_types_async();
    struct CommandError { uint64_t seq, generation; SrhStatus status; std::string message; };
    std::vector<CommandError> take_errors();

    explicit SimulationController(std::filesystem::path plugin_directory = executable_plugins_directory());
    ~SimulationController();
    SimulationController(const SimulationController &) = delete;
    SimulationController &operator=(const SimulationController &) = delete;

    // Executable-relative plugin directory.  Discovery, tool loading and card
    // insertion all resolve against it, so changing the working directory never
    // changes runtime lookup.
    static std::filesystem::path executable_plugins_directory();

    void start();
    void shutdown();
    bool started() const { return thread_started_; }

    std::shared_ptr<const UiSnapshot> snapshot() const;
    uint64_t memory_revision() const { return memory_revision_.load(); }

    // Frequent actions are posted and acknowledged asynchronously through the
    // control snapshot.  They never block the UI on simulation progress.
    nlohmann::json plugin_project_data();
    SrhStatus provider_command(Handle owner, uint64_t generation, uint32_t kind, uint64_t revision, const std::string &payload);
    uint64_t request_pause();
    uint64_t request_resume();
    uint64_t request_stop();
    uint64_t request_reset(bool cold);
    uint64_t request_step(uint32_t clock);
    uint64_t request_frequency(uint32_t clock, uint32_t hz);
    uint64_t request_stop_clocks();
    bool stop_and_wait(std::string &error);
    void enqueue_input(const std::string &endpoint, uint64_t timestamp, uint8_t value, uint64_t generation = 0);
    void clear_trace();
    void set_trace_capture(bool enabled, uint32_t operations = SRH_READ | SRH_WRITE);
    void clear_logs();
    void add_breakpoint(Handle card, Handle space, uint64_t first, uint64_t last,
                        uint32_t operations, uint64_t generation = 0);
    void remove_breakpoint(uint64_t id, uint64_t generation = 0);
    void set_breakpoint_enabled(uint64_t id, bool enabled, uint64_t generation = 0);
    void set_audio_master_volume(uint32_t percent);
    bool set_audio_sample_rate(uint32_t sample_rate, std::string &error);
    bool set_audio_resampling(SrzAudioResampling method, std::string &error);
    void set_audio_source_volume(Handle source, uint32_t percent, uint64_t generation = 0);
    void set_audio_source_pan(Handle source, int32_t pan, uint64_t generation = 0);
    void set_audio_source_muted(Handle source, bool muted, uint64_t generation = 0);
    void set_audio_software_clipping(bool enabled);
    void set_audio_dc_offset_correction(bool enabled);
    void set_audio_queue_capacity(uint64_t frames);
    void log_message(const std::string &message);

    // UI-thread -> controller state used only for publication policy.
    void set_video_visible(bool visible);
    void set_inspection_visible(bool visible);
    enum InspectionInterest : uint32_t { InspectProperties = 1, InspectTrace = 2, InspectLogs = 4, InspectAll = 7 };
    void set_inspection_interest(uint32_t interests);
    void set_ui_visible(bool visible) { ui_visible_ = visible; }
    void set_snapshot_cadence(uint64_t normal_interval_ms, uint64_t high_clock_interval_ms,
                              uint32_t threshold_hz);
    void request_manual_refresh();

    // Blocking request/reply operations.  These wait for a reply from the
    // simulation thread but never hold ImGui/tool/file-dialog locks.
    bool load_project_json(const std::string &json, const std::filesystem::path &project_dir,
                           std::string &error);
    bool new_project(std::string &error);
    bool restore_state(const std::string &project_json, const std::string &project_dir,
                       const std::string &execution_json, bool pause_after, std::string &error);
    bool save_state(bool include_trace, std::string &json, std::string &error);
    bool park(Handle card, std::string &error, uint64_t generation = 0);
    bool plug(Handle card, std::string &error, uint64_t generation = 0);
    bool put_away(Handle card, std::string &error, uint64_t generation = 0);
    bool remove_card(Handle card, std::string &error, uint64_t generation = 0);

    struct MemorySegment { uint64_t address; std::vector<uint8_t> bytes; };
    struct MemoryLoadResult {
        SrhStatus status = SRH_OK;
        std::string error;
        uint32_t failed_segment = UINT32_MAX;
        uint64_t written = 0;
    };
    MemoryLoadResult load_memory(Handle space, std::vector<MemorySegment> segments,
                                 bool reset, uint64_t generation);

    using CardRequest = ProjectCardRequest;
    bool add_card(const CardRequest &request, Handle &new_card, std::string &error, uint64_t generation = 0);

    std::vector<UiProperty> request_properties(Handle card, uint64_t generation = 0);
    SrhStatus request_edit(Handle card, uint32_t index, const SrhValue &value,
                           std::string &error, uint64_t generation = 0);
    std::vector<MemoryCell> request_memory(Handle space, uint64_t base, uint32_t length, uint64_t generation = 0);
    SrhStatus request_write_memory(Handle space, uint64_t base, const std::vector<uint8_t> &bytes,
                                   std::string &error, uint64_t generation = 0);
    std::vector<DisasmLine> request_disassembly(Handle card, Handle space, uint64_t start,
                                                uint32_t count, uint64_t generation = 0, uint32_t backward_count = 0);
    std::vector<uint8_t> request_text(Handle card, SrhStatus &status, uint64_t generation = 0);

    std::string config_get(const std::string &key, const std::string &fallback = "");
    std::string config_entry_get(const std::string &key, const std::string &fallback = "");
    SrhStatus config_entry_set(const std::string &key, const std::string &value);
    void config_set(const std::string &key, const std::string &value);
    void post_config_set(const std::string &key, const std::string &value);
    void config_load();
    void config_save();
    void post_config_save();

    uint32_t current_audio_master_volume() const { return audio_master_volume_.load(); }

    // Single-producer/single-consumer PCM boundary.  Producer: simulation
    // thread.  Consumer: SDL audio callback (PCM only, no engine access).
    bool audio_input_present() const { return input_present_.load(); }
    bool audio_input_requested() const { return input_requested_.load(); }
    void audio_input_push(const float *samples, uint32_t frames, uint32_t rate, uint32_t channels);
    uint32_t audio_available() const;
    uint32_t audio_capacity() const;
    uint32_t audio_read(int16_t *interleaved, uint32_t frames);
    uint32_t audio_read(int16_t *interleaved, uint32_t frames, uint64_t &epoch);
    uint64_t audio_epoch() const;
    void audio_recent(std::vector<int16_t> &interleaved, uint32_t frames) const;

  private:
    enum class CmdKind {
        ProviderCommand,
        PluginProjectData,
        Pause,
        Resume,
        Stop,
        Reset,
        Step,
        Frequency,
        StopClocks,
        EnqueueInput,
        ClearTrace,
        SetTraceCapture,
        ClearLogs,
        AddBreakpoint,
        RemoveBreakpoint,
        SetBreakpointEnabled,
        AudioMasterVolume,
        AudioSampleRate,
        AudioResampling,
        AudioSourceVolume,
        AudioSourcePan,
        AudioSourceMuted,
        AudioSoftwareClipping,
        AudioDcOffsetCorrection,
        AudioQueueCapacity,
        Log,
        LoadProjectJson,
        NewProject,
        RestoreState,
        SaveState,
        Park,
        Plug,
        PutAway,
        Remove,
        AddCard,
        ConfigGet,
        ConfigEntryGet,
        ConfigEntrySet,
        ConfigSet,
        ConfigLoad,
        ConfigSave,
    };

    // Unmigrated domains retain their existing payload during incremental migration.
    struct LegacyCommand {
        CmdKind kind = CmdKind::Pause;
        bool flag = false;
        uint32_t clock = 0;
        uint32_t hz = 0;
        Handle handle1 = 0;
        Handle handle2 = 0;
        uint64_t value1 = 0;
        uint64_t value2 = 0;
        std::string text1;
        std::string text2;
        std::optional<CardRequest> card;
        std::filesystem::path path1;
    };

    struct Properties {
        Handle card;
    };
    struct EditProperty {
        Handle card;
        uint32_t index;
        SrhValue value;
    };
    struct ReadMemory {
        Handle space;
        uint64_t base;
        uint32_t length;
    };
    struct WriteMemory {
        Handle space;
        uint64_t base;
        std::vector<uint8_t> bytes;
    };
    struct LoadMemory {
        Handle space;
        std::vector<MemorySegment> segments;
        bool reset;
    };
    struct DisassembleRange {
        Handle card;
        Handle space;
        uint64_t start;
        uint32_t count;
        uint32_t backward_count = 0;
    };
    struct TextQuery {
        Handle card;
    };
    // Host plugin-directory metadata.  It describes the installation, not the
    // loaded project, so it carries no generation.
    struct CardTypes {};

    struct InputBatch { std::string endpoint; uint64_t timestamp; std::vector<uint8_t> bytes; uint64_t source, identity; bool cancel = false; Handle expected_owner = 0; bool release = false; };
    std::map<uint64_t, uint64_t> input_identities_;
    void execute(const InputBatch &, Reply &);
    std::atomic<int64_t> provider_interest_until_{0};
    using CommandPayload = std::variant<InputBatch, ProjectRuntimeRequest, LegacyCommand, Properties, EditProperty, ReadMemory, WriteMemory, LoadMemory, DisassembleRange, TextQuery, CardTypes>;

    // Transport owns sequencing/completion; payload type determines the operation.
    struct Command {
        explicit Command(CommandPayload value) : payload(std::move(value)) {}
        uint64_t seq = 0;
        uint64_t generation = 0;
        std::shared_ptr<std::promise<Reply>> completion;
        std::shared_ptr<std::promise<ProjectRuntimeResult>> project_completion;
        std::chrono::steady_clock::time_point posted_at{};
        CommandPayload payload;
    };


    uint64_t post_command(Command command);
    AsyncReply submit(Command command);
    Reply submit_and_wait(Command command, std::chrono::milliseconds timeout);
    static bool project_scoped(const CommandPayload &payload);
    static bool requires_explicit_generation(const CommandPayload &payload);
    void execute(const ProjectRuntimeRequest &request, Reply &reply);
    void execute(const LegacyCommand &command, Reply &reply);
    void invalidate(const LegacyCommand &command, const Reply &reply);
    void execute(const Properties &request, Reply &reply);
    void execute(const EditProperty &request, Reply &reply);
    void execute(const ReadMemory &request, Reply &reply);
    void execute(const WriteMemory &request, Reply &reply);
    void execute(const LoadMemory &request, Reply &reply);
    void execute(const DisassembleRange &request, Reply &reply);
    void execute(const TextQuery &request, Reply &reply);
    void execute(const CardTypes &request, Reply &reply);
    void finish_command(Command &command, Reply reply);
    void reset_audio();
    void thread_main();
    void drain_commands();
    void process_command(Command &command);
    void publish_due();
    void mark_command_error(const std::string &error);
    void drain_audio();

    // Engine boundary helpers.  Every one of them runs on the simulation
    // thread; nothing here may be reached from the UI thread.
    std::string engine_message() const;
    bool engine_running() const;
    bool engine_stopped() const;
    bool space_maximum(Handle space, uint64_t &maximum);
    std::map<Handle, UiSpace> copy_spaces();
    std::vector<UiCardInfo> copy_cards(bool all);
    bool read_state(bool include_trace, std::string &json, std::string &error);
    // Reapplies the cached audio and trace options after a rack replacement.
    void apply_runtime_options();
    // Applies the cached runtime options to a fully loaded candidate, collects
    // its card associations and installs it as the active rack.  The candidate
    // is consumed on success and released on failure, so a failure before
    // installation always leaves the active engine and its state untouched.
    bool install_candidate(SrzEngine *candidate, std::vector<ProjectCardAssociation> *cards,
                           std::string &error);

    // The UI starts in a safe, explicitly stopped state.  Loading a project
    // and creating a new project preserve that state until the user resumes.
    SrzEngine *engine_ = nullptr;
    SrzResult *result_ = nullptr;
    std::filesystem::path plugin_directory_;
    std::thread thread_;
    std::atomic<bool> shutdown_requested_{false};
    std::atomic<bool> thread_started_{false};

    std::mutex command_mutex_;
    std::condition_variable command_cv_;
    std::deque<Command> commands_;

    uint64_t next_seq_ = 1;
    bool accepting_commands_ = false; // guarded by command_mutex_
    std::deque<CommandError> errors_; // guarded by error_mutex_

    mutable std::mutex snapshot_mutex_;
    std::shared_ptr<const UiSnapshot> latest_;

    uint64_t generation_ = 1;
    std::atomic<uint64_t> command_ack_seq_{0};
    std::atomic<uint64_t> snapshot_sequence_{0};
    std::atomic<uint64_t> memory_revision_{0};
    std::mutex error_mutex_;
    std::string last_command_error_;

    std::atomic<bool> control_dirty_{true};
    std::atomic<bool> inspection_dirty_{true};
    std::chrono::steady_clock::time_point last_provider_publish_{};
    std::atomic<bool> video_dirty_{true};
    std::atomic<bool> force_control_publish_{true};
    std::atomic<bool> force_inspection_publish_{true};
    std::atomic<bool> force_video_publish_{false};
    std::atomic<bool> force_provider_publish_{false};
    std::atomic<bool> manual_refresh_{false};
    std::atomic<bool> video_visible_{true};
    std::atomic<bool> inspection_visible_{true};
    std::atomic<bool> ui_visible_{true};
    std::atomic<uint32_t> inspection_interest_{InspectAll};
    std::atomic<bool> trace_capture_requested_{false};
    std::atomic<uint32_t> trace_capture_operations_requested_{SRH_READ | SRH_WRITE};
    std::atomic<uint64_t> normal_interval_ms_{33};
    std::atomic<uint64_t> high_clock_interval_ms_{66};
    std::atomic<uint32_t> high_clock_threshold_hz_{100};

    PcmQueue pcm_;
    std::atomic<uint64_t> audio_queue_capacity_frames_{44100};
    std::atomic<uint32_t> audio_sample_rate_{44100};
    std::atomic<SrzAudioResampling> audio_resampling_{SRZ_AUDIO_RESAMPLE_LINEAR};
    std::atomic<uint32_t> audio_master_volume_{100};
    std::atomic<bool> audio_dc_offset_correction_{true};
    std::atomic<bool> audio_software_clipping_{false};
    uint32_t simulation_load_percent_ = 0;
    double smoothed_load_percent_ = 0;
    uint64_t command_latency_us_ = 0, slice_wall_us_ = 0, discarded_wall_ns_ = 0;
    std::chrono::steady_clock::time_point last_control_{}, last_inspection_{}, last_video_{};
    std::atomic<bool> input_present_{false};
    std::atomic<bool> input_requested_{false};
    std::mutex input_mutex_;
    std::vector<float> input_pending_;
    uint32_t input_rate_ = 0, input_channels_ = 0;
    uint64_t input_generation_ = 0;
    void drain_audio_input();
    std::array<int16_t, 8192 * 2> audio_scratch_{};
};

} // namespace srz80::ui
