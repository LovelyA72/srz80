#pragma once
#include "video_shader_uniforms.hpp"
#include "tool_dialogs.hpp"
#include "tool_inputs.hpp"
#include "host_input.hpp"
#include "audio_backend.hpp"
#include "platform_paths.hpp"
#include "simulation_controller.hpp"
#include "trace_view.hpp"
#include "project_workspace.hpp"
#include "project_text_editor.hpp"
#include "project_session.hpp"
#include "rack_thumbnail.hpp"
#include "project_dialogs.hpp"
#include "video_shader.hpp"
#include <SDL3/SDL.h>
#include <srz80/tool.h>
#include <array>
#include <cstdint>
#include <filesystem>
#include <future>
#include <imgui.h>
#include <memory>
#include <map>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace srz80::ui {

class App;
struct ProjectTextFormat {
    std::string plugin_id, extension, label;
    void *context = nullptr;
    SrhToolProjectFileOpen open = nullptr;
    bool enabled = true;
    /* The tool interprets the file itself. The host never loads it as a
       project text document. */
    bool binary = false;
    std::filesystem::path active_path;
};

inline bool project_file_config_key(std::string_view key) {
    constexpr std::string_view suffix = "_file";
    return key.size() > suffix.size() &&
           key.substr(key.size() - suffix.size()) == suffix;
}

struct LoadedTool {
    std::shared_ptr<void> library;
    const SrhToolPlugin *api = nullptr;
    void *instance = nullptr;
    std::string category = "Misc";
    bool open = false;
    bool background_failed = false;
};

struct VideoShaderDrawData {
    SDL_Renderer *renderer = nullptr;
    SDL_GPURenderState *state = nullptr;
    SDL_Texture *source_texture = nullptr;
    SDL_Texture *output_texture = nullptr;
    VideoShaderUniforms uniforms{};
};

// One row of the Settings window.  It flattens either a host/tool registration
// (with its own callbacks) or a simulation-owned plugin setting whose metadata
// arrived in the inspection snapshot.
struct SettingsEntry {
    std::string category, name, label, description, enum_labels, default_value;
    uint32_t type = Srh_CONFIG_STRING;
    void *context = nullptr;
    SrhConfigGet get = nullptr;
    SrhConfigSet set = nullptr;
    bool simulation_owned = false;

    SettingsEntry(const UiConfigRegistration &entry, bool remote = false)
        : category(entry.category), name(entry.name), label(entry.label),
          description(entry.description), enum_labels(entry.enum_labels),
          default_value(entry.default_value), type(entry.type), context(entry.context),
          get(entry.get), set(entry.set), simulation_owned(remote) {}
    SettingsEntry(const UiConfigEntry &entry, bool remote = false)
        : category(entry.category), name(entry.name), label(entry.label),
          description(entry.description), enum_labels(entry.enum_labels),
          default_value(entry.default_value), type(entry.type), simulation_owned(remote) {}
};

// Main application shell. Panel rendering lives in ui/panels/.
class App {
  public:
    App();
    ~App();
    bool load(const std::filesystem::path &path);
    void draw(bool draw_when_hidden = false);
    void request_quit();
    bool file_dialog_pending() const;
    bool host_input_available() const { return show_video && !show_welcome && !file_dialog_pending(); }
    size_t loaded_tool_count() const { return tools.size(); }
    bool open_tool(const std::string &id) {
        for (auto &tool : tools) if (id == tool.api->id) { tool.open = true; return true; }
        return false;
    }

    std::filesystem::path base = srz80::executable_directory();
    SimulationController controller;
    HostInput host_input;
    std::shared_ptr<const UiSnapshot> snapshot;
    // Add-card metadata published by the simulation worker.
    std::vector<UiCardType> card_types;
    bool discovering_card_types() const { return card_types_request.valid(); }
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    using PathBuffer = std::array<char, 1024>;
    std::vector<PathBuffer> rom_paths, info_rom_paths;
    std::map<std::string, PathBuffer> add_project_file_paths, info_project_file_paths;
    char info_name[256]{}, info_config[4096]{};
    std::string error;
    srz80::Handle selected = 0, space = 0, add_io_space = 0;
    uint64_t memory_base = 0x1000, add_base = 0x1000, add_size = 256, bp_first = 0xC,
             bp_last = 0xC;
    uint64_t memory_selection_anchor = 0, memory_selection_cursor = 0;
    int memory_length = 256, add_type = 0, add_type_seen = -1, add_priority = 0, add_clock = 0, bp_type = 0,
        filter_op = 0;
    int memory_editor_columns = 16;
    uint64_t filter_master = 0, filter_space = 0;
    int filter_kind = 0;
    uint8_t memory_fill_value = 0;
    srz80::Handle memory_selection_space = 0;
    bool memory_has_selection = false, memory_drag_selecting = false;
    bool memory_editor_show_preview = true, memory_editor_show_hexii = false,
         memory_editor_show_ascii = true, memory_editor_grey_zeroes = true,
         memory_editor_uppercase_hex = true;
    bool group_trace = true;
    bool record_trace = false;
    char console_input[256] = {};
    bool console_live_mode = false;
    bool console_autoscroll = true;
    int console_max_lines = 1000;
    bool quit_requested = false;
    ProjectSession project_session{&controller, base};
    bool show_rack = true, show_rack_info = true, show_memory_buses = true, show_clocks = true, show_inspector = true, show_memory = true;
    bool show_breakpoints = true, show_monitor = true, show_console = true, show_disassembly = true,
         show_video = true, show_log = true, show_mixer = true;
    bool show_oscilloscope = false;
    bool show_project_files = true, show_text_editor = false;
    ProjectTextEditor project_text_view;
    std::string editor_theme = "Dark";
    std::string editor_font_path;
    int editor_font_size = 18;
    bool editor_show_whitespace = false;
    bool editor_show_eol = false;
    bool editor_highlight_cursor_line = false;
    ImFont *editor_font = nullptr;
    double editor_font_save_at = 0;
    bool show_license = false;
    bool show_about = false;
    bool show_add_card = false;
    bool show_card_info = false;
    bool show_settings = false;
    bool open_project_settings_requested = false;
    uint64_t project_settings_generation = 0;
    ProjectSession::RackInfo project_settings_draft;
    std::vector<InputRoute> project_input_draft;
    ImGuiTextFilter project_settings_filter;
    int project_settings_category = 0;
    void open_project_settings();
    void project_settings();
    bool settings_dirty = false;
    bool settings_draft_active = false;
    std::map<std::string, std::string> settings_draft;
    std::map<std::string, std::string> settings_original;
    bool video_vsync = true;
    int video_frame_rate_limit = 100;
    int snapshot_normal_ms = 33;
    int snapshot_high_ms = 66;
    int snapshot_high_threshold_hz = 100;
    bool video_display_render_time = false;
    bool video_late_render_clear = false;
    bool video_power_saving = false;
    double video_frame_time_ms = 0.0;
    double video_display_frame_time_ms = 0.0;
    std::string video_render_backend = "default";
    std::string video_render_backend_labels = "default";
    float video_scale = 1.0f;
    std::string video_scale_filter = "Nearest integer";
    std::string video_shader_path;
    VideoShaderProgram video_shader_program;
    std::string video_shader_loaded_path;
    std::string video_shader_error;
    SDL_GPUShader *video_gpu_shader = nullptr;
    SDL_GPURenderState *video_gpu_state = nullptr;
    std::vector<SDL_GPUTexture *> video_shader_textures;
    std::vector<SDL_GPUSampler *> video_shader_samplers;
    float ui_scale = 1.0f;
    std::string ui_theme = "Dark Red";
    std::string ui_font_path;
    int ui_font_size = 18;
    bool ui_font_antialias = true;
    int ui_font_hinting = 0;
    ImFont *project_dirty_font = nullptr;
    bool ui_font_loaded = false;
    bool font_awesome_loaded = false;
    bool audio_input_enabled = true;
    std::string audio_input_device = "<System default>";
    int audio_input_channels = 1;
    int audio_input_volume = 90;
    bool audio_enabled = true;
    bool loading_config = false;
    int audio_queue_frames = 44100;
    std::string audio_backend_name = "SDL";
    std::string audio_driver = "Automatic";
    std::string audio_device = "<System default>";
    std::string audio_driver_labels = "Automatic";
    std::string audio_device_labels = "<System default>";
    int audio_sample_rate = 44100;
    std::string audio_resampling = "Linear";
    int audio_outputs = 2;
    int audio_buffer_size = 1024;
    bool audio_low_latency = false;
    bool audio_force_mono = false;
    bool audio_software_clipping = false;
    bool audio_dc_offset_correction = true;
    int scope_timebase_ms = 20;
    float scope_gain = 1.0f;
    int scope_trigger_level = 0;
    int scope_trigger_channel = 0;
    bool scope_trigger_rising = true;
    bool scope_show_left = true, scope_show_right = true, scope_frozen = false, scope_xy_mode = false,
         scope_stereo = true;
    std::vector<int16_t> scope_samples;

    void apply_video_settings();
    void select_video_shader_file_dialog();
    void apply_ui_settings();
    void register_app_config();
    void monitor_audio_input() { audio_input_monitor_visible = true; }
    void update_audio_input() {
        audio_backend.set_input_monitoring(audio_input_monitor_visible && show_settings);
        audio_backend.update_input();
    }
    void audio_input_device_removed(SDL_AudioDeviceID id) { audio_backend.input_device_removed(id); }
    float audio_input_level() const { return audio_backend.input_level(); }
    bool recording_audio() const { return audio_backend.recording(); }
    const std::string &audio_input_error() const { return audio_backend.input_error(); }
    void preview_input_volume(uint32_t percent) { audio_backend.set_input_volume(percent); }
    void retry_audio_input() { audio_backend.retry_input(); }
    void start_audio();
    void stop_audio();
    std::string video_backend_info() const;
    srz80::Handle find_space(const std::string &name) const;
    uint64_t snapshot_interval_ms() const;
    void refresh_memory_cache();
    void invalidate_disassembly_cache();
    void sync_disassembly_memory_revision();
    void refresh_console_text();
    void invalidate_console_text() { console_text_dirty = true; }
    void post_config_if_changed(const std::string &key, const std::string &value);

  private:
    void poll_project_operation();
    void draw_project_modals();
    void show_new_project_dialog(uint64_t operation);
    void browse_new_project_parent();
    void welcome();
    void draw_modal_windows();
    void invalidate_project_views(bool clear_workspace);
    void remember_last_used_directory(const std::filesystem::path &path);
    void remember_recent_project(const std::filesystem::path &path);

    bool project_operation_restarts_audio = false;
    std::filesystem::path last_project_directory;
    std::filesystem::path last_projects_directory;
    uint64_t new_project_dialog_operation = 0;
    bool open_new_project_dialog = false;
    char new_project_name[256]{};
    char new_project_parent[1024]{};
    std::vector<std::filesystem::path> recent_projects;
    std::map<std::filesystem::path, std::string> recent_project_names;
    bool auto_load_last_project = false;
    bool show_welcome = true;
    bool welcome_project_request = false;
    SDL_Texture *welcome_banner = nullptr;
    SDL_Texture *welcome_logo = nullptr;
    int welcome_banner_width = 0, welcome_banner_height = 0;
    int welcome_logo_width = 0, welcome_logo_height = 0;

    void settings();
    void initialize_settings_draft(const std::vector<SettingsEntry> &entries);
    bool apply_settings_draft(const std::vector<SettingsEntry> &entries);
    void discard_settings_draft();
    void load_config();
    void load_layout_from_config();
    void save_config();
    void save_layout_to_config();
    void import_layout_dialog();
    void export_layout_dialog();
    void begin_layout_import(std::filesystem::path path);
    void begin_layout_export(std::filesystem::path path);
    void poll_layout_file_operation(bool finish = false);
    void advance_layout_import();
    void reset_layout();
    void restore_tool_config();
    void sync_window_states();
    using FileDialogAction = ProjectDialogAction;
    using PendingFileDialog = ProjectDialogResult;
    using FileDialogState = ProjectDialogState;
    using FileDialogRequest = ProjectDialogRequest;
    bool combo_space(const char *label, srz80::Handle &current);
    void select_space(const char *label);
    void menu();
    void load_tools();
    SrhStatus request_tool_file_dialog(uint32_t save, const SrhToolFileFilter *filters,
                                       uint32_t filter_count, SrhHandle *request);
    void draw_tools();
    void tick_tools(bool visible);
    void project_files();
    void text_editor();
    void open_project_file(const std::filesystem::path &path);
    void request_project_file(const std::filesystem::path &directory, std::string extension);
    void save_tool_project_files();
    void update_project_document(void *handler_context, const std::filesystem::path &path,
                                 std::string_view text, uint64_t cursor, bool record = true);
    void restore_tool_project_state();
    void sync_tool_project_state(bool strict = false);
    void license();
    void about();
    void layout();
    void rack();
    void rack_info();
    void add_card_modal();
    void card_info_modal();
    void open_card_info(srz80::Handle card);

    void clocks();
    void inspector();
    void memory();
    void memory_buses();
    void breakpoints();
    void monitor();
    void console();
    void disassembly();
    void video();
    void mixer();
    void oscilloscope();
    void log();
    void copy_console_text();
    bool send_console_input(const char *text);
    static int console_input_callback(ImGuiInputTextCallbackData *data);
    void note_text_input(char *buffer, size_t capacity);
    void edit_undo();
    void edit_redo();
    void push_text_undo(char *buffer, size_t capacity, const std::string &value);
    void request_focus_for_text_box(char *buffer, bool preserve_selection = false);
    void refocus_text_command_target();
    void replace_path_buffers(std::vector<PathBuffer> &buffers, size_t count);
    bool text_box_available(char *buffer) const;
    bool has_text_undo() const;
    bool has_text_redo() const;
    void edit_copy();
    void edit_cut();
    void edit_paste();
    void edit_select_all();
    void edit_delete();
    void open_project_dialog(uint64_t operation = 0);
    void save_project_as_dialog(uint64_t operation = 0);
    void save_state();
    void save_state_as_dialog(uint64_t operation = 0);
    void load_state_dialog(uint64_t operation = 0);
    void select_rom_file_dialog(bool for_card_info, uint32_t image_slot = 0);
    void select_project_file_dialog(FileDialogAction action, std::string config_key,
                                    srz80::Handle card = 0, uint32_t property_index = 0);
    std::string validated_project_file_path(const char *path) const;
    void select_rack_thumbnail_file_dialog();
    void destroy_video_shader();
    void reload_video_shader();
    void process_file_dialog_results();
    void remove_card(srz80::Handle id);
    void plug_card(srz80::Handle id);
    void put_away_card(srz80::Handle id);
    srz80::Handle console_card() const;
    static void hex_input(const char *label, uint64_t &value);
    bool file_input(const char *id, const char *label, char *path, size_t capacity);
    static ImU8 memory_editor_read(const ImU8 *, size_t offset, void *user_data);
    static void memory_editor_write(ImU8 *, size_t offset, ImU8 value, void *user_data);
    static ImU32 memory_editor_background(const ImU8 *, size_t offset, void *user_data);
    static const char *kind_name(uint32_t kind);
    static std::string escape_transcript_byte(uint8_t value);
    static void SDLCALL file_dialog_callback(void *userdata, const char *const *filelist, int filter);

    struct LayoutFileResult {
        FileDialogAction action = FileDialogAction::import_layout;
        std::string data, error;
    };
    std::future<LayoutFileResult> layout_file_operation;
    std::string pending_layout_import;
    std::vector<bool *> layout_windows_to_reopen;
    int layout_import_step = 0;
    bool layout_dialog_pending = false;
    bool layout_initialized = false;

    struct TextEditRecord {
        char *buffer;
        size_t capacity;
        std::string value;
    };
    char *active_text = nullptr;
    size_t active_text_capacity = 0;
    ImGuiID active_text_id = 0;
    std::vector<TextEditRecord> undo_stack;
    std::vector<TextEditRecord> redo_stack;
    bool focus_console_input = false;
    bool focus_rom_path = false;
    bool focus_project_text = false;
    bool restore_project_text_cursor = false;
    srz80::Handle info_card = 0;
    srz80::Handle info_space = 0;
    uint64_t info_base = 0, info_size = 0, info_reset_vector = 0;
    int info_priority = 0, info_clock = 0;

    uint64_t rack_info_generation = 0;
    uint64_t rack_thumbnail_generation = 0;
    bool rack_info_initialized = false;
    ProjectSession::RackInfo rack_info_draft;
    std::array<char, 256> rack_info_name{}, rack_info_author{};
    std::array<char, 128> rack_info_version{};
    std::array<char, 8192> rack_info_notes{};
    bool rack_thumbnail_import = false;
    std::future<RackThumbnail> rack_thumbnail_job;
    SDL_Texture *rack_thumbnail_texture = nullptr;
    int rack_thumbnail_width = 0, rack_thumbnail_height = 0;

    std::shared_ptr<FileDialogState> file_dialog_state = std::make_shared<FileDialogState>();
    uint64_t file_dialog_epoch = 1;
    struct VideoTexture {
        SDL_Texture *texture = nullptr;
        SDL_Texture *cosine_texture = nullptr;
        SDL_Texture *shader_texture = nullptr;
        uint32_t width = 0, height = 0;
        uint32_t cosine_width = 0, cosine_height = 0;
        uint32_t shader_width = 0, shader_height = 0;
        uint64_t publication = 0;
        uint64_t cosine_publication = 0;
        size_t bytes = 0;
        uint32_t surface_ordinal = 0;
        bool shader_enabled = false;
        bool shader_state_initialized = false;
        VideoShaderDrawData shader_draw;
    };
    std::map<srz80::Handle, VideoTexture> video_textures;
    std::map<SrhHandle, std::shared_ptr<ToolDialogState>> tool_dialogs;
    SrhHandle next_tool_dialog = 1;
    SrhToolHostV1 tool_host{};
    ToolInputs tool_inputs;
    uint64_t tool_provider_generation = UINT64_MAX, tool_provider_sequence = UINT64_MAX;
    std::string tool_provider_data;
    std::vector<LoadedTool> tools;
    std::string pending_tool_focus;
    std::vector<std::shared_ptr<ProjectTextFormat>> project_text_formats;
    uint64_t active_project_document_id = 0;
    ProjectDocument *active_project_document() { return project_session.workspace().find(active_project_document_id); }
    const ProjectDocument *active_project_document() const {
        return const_cast<App *>(this)->active_project_document();
    }
    std::map<uint64_t, std::weak_ptr<ProjectTextFormat>> document_handlers;
    std::vector<UiConfigRegistration> app_config_entries;
    std::vector<UiConfigRegistration> tool_config_entries;
    std::map<std::string, std::string> posted_config;
    bool audio_input_monitor_visible = false;
    AudioBackend audio_backend;
    uint64_t pending_runtime_command = 0;
    uint64_t project_operation_pause_operation = 0;
    bool reload_changed_roms = false;
    bool reloading_roms = false;
    bool resume_after_rom_reload = false;
    std::map<std::filesystem::path, std::pair<uintmax_t, std::filesystem::file_time_type>> rom_file_states;
    void request_resume();
    bool rom_files_changed() const;
    void remember_rom_file_states();
    TraceView monitor_view;

    // UI-side caches of expensive inspection results.  These are refreshed at
    // the snapshot cadence (or invalidated after UI-side memory writes) and are
    // never used to mutate Core.
    SimulationController::AsyncReply clipboard_request;
    bool clipboard_is_memory = false;
    uint64_t clipboard_size = 0;
    void poll_clipboard();
    // Card descriptors are read by the simulation worker, never by the GUI.
    SimulationController::AsyncReply card_types_request;
    void poll_card_types();
    SimulationController::AsyncReply memory_request, console_request, stack_request, disasm_request;
    uint64_t memory_request_generation = 0, console_request_generation = 0;
    srz80::Handle console_request_card = 0;
    uint64_t stack_base = 0, stack_generation = 0;
    srz80::Handle stack_space = 0;
    std::vector<MemoryCell> stack_cache;
    std::chrono::steady_clock::time_point stack_cache_time{};
    enum class DisasmRequestKind {
        Reset,
        Append,
        Prepend
    };
    DisasmRequestKind disasm_request_kind = DisasmRequestKind::Reset;
    srz80::Handle disasm_request_card = 0, disasm_request_space = 0;
    uint64_t disasm_request_generation = 0, disasm_request_revision = 0;
    uint64_t disasm_availability_revision = 0, disasm_request_availability_revision = 0;
    uint64_t disasm_request_start = 0, disasm_request_view = 0;
    bool disasm_request_append = false, disasm_request_anchor_known = true;
    void queue_disassembly(srz80::Handle card, srz80::Handle space, uint64_t start,
                           uint32_t count, DisasmRequestKind kind = DisasmRequestKind::Reset,
                           bool anchor_known = true, uint32_t backward_count = 0);
    void poll_disassembly(srz80::Handle space);
    std::vector<MemoryCell> memory_cache;
    srz80::Handle memory_cache_space = 0;
    uint64_t memory_cache_base = 0;
    int memory_cache_length = 0;
    std::chrono::steady_clock::time_point memory_cache_time{};
    std::vector<uint8_t> console_text_cache;
    srz80::Handle console_text_card = 0;
    std::chrono::steady_clock::time_point console_text_time{};
    bool console_text_dirty = true;
    std::vector<DisasmLine> disasm_cache;
    srz80::Handle disasm_card = 0;
    srz80::Handle disasm_space = 0;
    uint64_t disasm_start = 0, disasm_maximum = 0;
    size_t disasm_view_row = 0;
    bool disasm_follow_pc = true, disasm_show_hex = false, disasm_anchor_known = true;
    uint64_t disasm_memory_revision = UINT64_MAX;
    std::chrono::steady_clock::time_point disasm_running_refresh_at_{};
    // Address spaces smaller than this many bytes are aligned by decoding from
    // address zero. Larger spaces decode from a typed address and warn when the
    // requested address is not a known instruction boundary.
    uint64_t disasm_align_from_zero_limit = 64 * 1024;
    std::vector<uint64_t> disasm_history;
    uint64_t disasm_last_pc = UINT64_MAX;
    uint64_t disasm_selected_address = UINT64_MAX;
};

} // namespace srz80::ui
