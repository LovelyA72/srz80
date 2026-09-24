#pragma once

#include "project_runtime.hpp"
#include "project_workspace.hpp"
#include "project_persistence.hpp"
#include "input_routing.hpp"
#include <chrono>
#include <optional>
#include <functional>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

namespace srz80::ui {

// GUI-thread document owner. Runtime execution and tool callbacks stay in their
// shell adapters. Filesystem tasks receive owned snapshots; only this owner commits results.
class ProjectSession {
  public:
    enum class DocumentState { unloaded, loaded };
    using SourceReader = std::function<std::future<std::vector<ProjectFileProbe>>(std::vector<ProjectFileProbe>)>;
    using Writer = std::function<std::future<ProjectWriteResult>(ProjectWriteRequest)>;
    explicit ProjectSession(ProjectRuntime *runtime = nullptr, std::filesystem::path default_directory = {},
                            Writer writer = {}, SourceReader reader = {})
        : reader_(std::move(reader)), writer_(std::move(writer)), runtime_(runtime), default_directory_(std::move(default_directory)) {}
    ~ProjectSession();
    enum class ActionKind { new_project, open, close_project, save, save_as, quit, insert, park, plug,
                            put_away, reorder, configure, edit_spaces, reload_roms, clock, stop_clocks, property, open_state, save_state, save_state_as };
    enum class Phase { idle, awaiting_unsaved, awaiting_path, preparing, awaiting_shell,
                       awaiting_runtime, checking_sources, saving, awaiting_conflict, unresolved, shutdown };
    enum class Decision { save, discard, cancel };
    enum class ConflictDecision { overwrite, reload, cancel };
    struct CardSettings {
        std::string name, space_name;
        std::vector<std::string> images;
        uint64_t base = 0, size = 0, reset_vector = 0;
        int32_t priority = 0;
        uint32_t clock = 0;
        nlohmann::json config = nlohmann::json::object();
    };
    struct RackInfo {
        std::string name, author, version, notes, thumbnail;
        bool operator==(const RackInfo &) const = default;
    };
    struct Action {
        ActionKind kind = ActionKind::new_project;
        std::filesystem::path path;
        uint64_t card = 0, target = 0, selected = 0;
        bool after = false;
        ProjectCardRequest insertion;
        CardSettings settings;
        uint32_t index = 0, hz = 0;
        SrhValue value{};
        nlohmann::json config;
    };
    struct Effect {
        enum class Kind { new_project_path, open_path, save_path, save, capture_state, before_replace, installed, closed,
                          replacement_finished, changed, source_reloaded, saved, state_saved, open_state_path, save_state_path, cancelled, error, quit };
        Kind kind;
        uint64_t operation = 0;
        std::filesystem::path path;
        uint64_t selected = 0;
        bool clear_workspace = false;
        std::string message;
        std::filesystem::path previous_path;
    };
    bool request(Action action, bool workspace_dirty = false);
    bool request(ActionKind kind, const std::filesystem::path &path = {}) {
        Action action; action.kind = kind; action.path = path;
        return request(std::move(action));
    }
    void resolve_unsaved(uint64_t operation, Decision decision);
    void resolve_path(uint64_t operation, const std::filesystem::path &path,
                      const std::string &error = {});
    void prepare_save(uint64_t operation, const std::string &error = {});
    const std::filesystem::path &state_path() const { return state_path_; }
    DocumentState document_state() const { return document_state_; }
    bool loaded() const { return document_state_ == DocumentState::loaded; }
    void confirm_replace(uint64_t operation);
    void finish_save(uint64_t operation, bool success, const std::string &error,
                     bool workspace_dirty);
    void poll();
    void resolve_conflicts(uint64_t operation, ConflictDecision decision);
    const std::vector<ProjectFileProbe> &save_conflicts() const { return save_conflicts_; }
    void shutdown();
    std::vector<Effect> take_effects();
    Phase phase() const { return phase_; }
    bool busy() const { return phase_ != Phase::idle; }
    bool blocks_simulation() const;
    bool transitioning() const;
    uint64_t operation_id() const { return operation_ ? operation_->id : 0; }
    const std::string &operation_error() const { return operation_error_; }
    std::optional<uint32_t> pending_clock(uint32_t index) const;
    void initialize_generation(uint64_t generation);

    using CardRecords = std::vector<std::pair<uint64_t, nlohmann::json>>;
    struct SaveIdentity {
        uint64_t generation;
        uint64_t revision;
    };

    ProjectWorkspace &workspace() { return workspace_; }
    const ProjectWorkspace &workspace() const { return workspace_; }
    const nlohmann::json &document() const { return document_; }
    const std::filesystem::path &path() const { return path_; }
    std::string name() const { return project_display_name(document_, path_); }
    const CardRecords &active_cards() const { return active_; }
    const CardRecords &removed_cards() const { return removed_; }
    uint64_t generation() const { return generation_; }
    uint64_t revision() const { return revision_; }
    uint64_t saved_revision() const { return saved_revision_; }
    bool dirty() const { return loaded() && revision_ != saved_revision_; }
    SaveIdentity save_identity() const { return {generation_, revision_}; }

    static nlohmann::json empty_document();
    void install(nlohmann::json document, const std::filesystem::path &path,
                 uint64_t generation, const std::vector<ProjectCardAssociation> &cards,
                 bool saved);
    bool commit_save(SaveIdentity captured, const std::filesystem::path &path);
    void note_external_edit();
    void commit_insert(uint64_t generation, uint64_t id, nlohmann::json card);
    void commit_park(uint64_t generation, uint64_t id);
    void commit_plug(uint64_t generation, uint64_t id);
    void commit_put_away(uint64_t generation, uint64_t id);
    void commit_card_config(uint64_t generation, uint64_t id, nlohmann::json config);
    void commit_card_document(uint64_t generation, uint64_t id, nlohmann::json card);
    void commit_clock(uint64_t generation, uint32_t index, uint32_t hz);
    // Mixer controls are machine settings. Source identity is the owning card's
    // persisted position plus its registration name/ordinal, never a transient
    // engine source handle.
    bool commit_mixer_master(uint32_t percent);
    RackInfo rack_info() const;
    bool commit_rack_info(RackInfo info);
    bool commit_mixer_source(uint64_t owner, std::string name, uint32_t ordinal,
                             uint32_t percent, bool muted, int32_t pan);
    bool video_shader_enabled(uint64_t owner, uint32_t ordinal) const;
    bool commit_video_shader(uint64_t generation, uint64_t owner, uint32_t ordinal, bool enabled);
    std::string input_card_id(uint64_t owner) const;
    uint64_t input_card_owner(const std::string &id) const;
    std::vector<InputRoute> input_routes() const;
    bool commit_input_routes(uint64_t generation, const std::vector<InputRoute> &routes);
    void collect_plugin_data(uint64_t generation, const nlohmann::json &data);
    void collect_tool_state(nlohmann::json states, std::vector<std::string> path_states = {});

  private:
    struct Operation {
        uint64_t id;
        Action action;
        std::optional<Action> after_save;
        ProjectRuntimeRequest runtime;
        std::filesystem::path restored_project_path;
        bool replacement_announced = false;
    };
    void advance();
    void prepare_mutation();
    void submit_runtime();
    void fail(const std::string &error, bool unresolved = false);
    void emit(Effect::Kind kind, const std::string &message = {});
    void start_writing(const ProjectRuntimeResult &result);
    void emit_source_reload(const ProjectFileProbe &probe);
    void begin_source_save();
    void poll_source_files();
    std::future<std::vector<ProjectFileProbe>> save_precheck_;
    uint64_t save_precheck_generation_ = 0;
    std::future<std::vector<ProjectFileProbe>> probing_;
    uint64_t probing_generation_ = 0;
    std::chrono::steady_clock::time_point next_probe_{};
    std::vector<ProjectFileProbe> save_conflicts_;
    SourceReader reader_;
    Writer writer_;
    std::future<ProjectWriteResult> writing_;
    SaveIdentity writing_identity_{};
    nlohmann::json writing_tool_state_;
    std::vector<std::string> tool_path_states_;
    std::filesystem::path state_path_;
    ProjectWorkspace workspace_;
    ProjectRuntime *runtime_ = nullptr;
    std::filesystem::path default_directory_;
    Phase phase_ = Phase::idle;
    std::optional<Operation> operation_;
    uint64_t next_operation_ = 1;
    std::future<nlohmann::json> preparation_;
    std::future<ProjectRuntimeResult> completion_;
    std::vector<Effect> effects_;
    std::string operation_error_;
    DocumentState document_state_ = DocumentState::unloaded;
    bool stopping_ = false;
    void require_generation(uint64_t generation) const;
    void refresh_cards();
    nlohmann::json document_ = nlohmann::json::object();
    std::filesystem::path path_;
    CardRecords active_, removed_;
    std::vector<uint64_t> order_;
    uint64_t generation_ = 0, revision_ = 0, saved_revision_ = 0, installed_revision_ = 0;
};

} // namespace srz80::ui
