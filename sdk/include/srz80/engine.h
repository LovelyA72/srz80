#ifndef SRZ80_ENGINE_H
#define SRZ80_ENGINE_H
#include <stddef.h>
#include <stdint.h>
#include <srz80/abi.h>
#ifdef __cplusplus
extern "C" {
#endif

/* SRZ80 engine ABI.
 *
 * The engine owns the rack: spaces, cards, bus routing, clocks, events,
 * debugger state, inputs, video/text/audio/provider registrations, project
 * loading, execution state and host configuration.  The GUI, the headless
 * application, tools and tests reach the engine only through this header.
 * Card plugins keep the separate, unchanged card ABI in <srz80/abi.h>.
 *
 * Thread affinity: an engine instance and every card it loads belong to the
 * thread that created the engine.  Every entry point below must be called from
 * that thread, including result accessors.  Engines are never shared between
 * threads and are never entered concurrently.  The caller may hold an engine
 * handle on another thread, but must never call into it there.
 *
 * Lifetime rules:
 *   - An SrzEngine is created with srz80_engine_create (or
 *     srz80_engine_candidate_create) and released with srz80_engine_destroy.
 *   - An SrzResult is created with srz80_engine_result_create on the engine's
 *     thread, filled by query calls, read through accessors, and released with
 *     srz80_engine_result_destroy.  Every pointer an accessor returns points
 *     into the result's arena and stays valid until the next fill or clear
 *     call on that result, or until the result is destroyed.  No pointer
 *     returned here points into engine storage.
 *   - Single-value text outputs use a caller-owned buffer.  Pass NULL to query
 *     the required size.  The returned length excludes the terminating NUL.
 *
 * All byte slices are UTF-8 unless documented otherwise.  No C++ type, STL
 * container, exception, Rust type or Rust allocation crosses this boundary.
 * Failures are reported as SrhStatus; srz80_engine_last_error returns the
 * message for the most recent failing call on the engine's thread.
 */

#if defined(_WIN32)
#if defined(SRZ80_ENGINE_BUILD)
#define SRZ_EXPORT __declspec(dllexport)
#elif defined(SRZ80_ENGINE_IMPORT)
#define SRZ_EXPORT __declspec(dllimport)
#else
#define SRZ_EXPORT
#endif
#define SRZ_CALL __cdecl
#else
#define SRZ_EXPORT __attribute__((visibility("default")))
#define SRZ_CALL
#endif

#define SRZ80_ENGINE_ABI 3u
#define SRZ_HEADER                                                                                 \
    uint32_t abi_version;                                                                          \
    uint32_t struct_size
#define SRZ_INIT(T) SRZ80_ENGINE_ABI, (uint32_t)sizeof(T)

typedef struct SrzEngine SrzEngine;
typedef struct SrzResult SrzResult;

/* Borrowed UTF-8 byte slice.  The pointed-to bytes are valid only for the
   duration of the call that received the slice. */
typedef struct SrzSlice {
    const char *data;
    uint64_t size;
} SrzSlice;

/* Engine capability bits reported by srz80_engine_capabilities. */
enum {
    SRZ_CAP_PROJECT = 1u << 0,
    SRZ_CAP_PROJECT_CANDIDATE = 1u << 1,
    SRZ_CAP_STATE = 1u << 2,
    SRZ_CAP_TRACE = 1u << 3,
    SRZ_CAP_DEBUGGER = 1u << 4,
    SRZ_CAP_SIGNALS = 1u << 5,
    SRZ_CAP_INPUT = 1u << 6,
    SRZ_CAP_TEXT = 1u << 7,
    SRZ_CAP_VIDEO = 1u << 8,
    SRZ_CAP_AUDIO = 1u << 9,
    SRZ_CAP_PROVIDERS = 1u << 10,
    SRZ_CAP_CONFIG = 1u << 11,
    SRZ_CAP_PLUGIN_DISCOVERY = 1u << 12,
    SRZ_CAP_PLUGIN_DATA = 1u << 13
};

typedef uint32_t SrzRunState;
enum { SRZ_STOPPED = 0, SRZ_PAUSED = 1, SRZ_RUNNING = 2 };
typedef uint32_t SrzTimeMode;
enum { SRZ_TIME_PROJECT = 0, SRZ_TIME_FIXED = 1, SRZ_TIME_SYSTEM = 2 };
typedef uint32_t SrzResolver;
enum { SRZ_RESOLVER_PRIORITY = 0, SRZ_RESOLVER_BIT_OR = 1 };

/* ------------------------------------------------------------------ */
/* Result records.  Every record begins with abi_version/struct_size.  */
/* The engine fills both fields when a record is produced, and checks  */
/* them when a caller supplies a record or structure as input.         */
/* ------------------------------------------------------------------ */

typedef struct SrzCardInfo {
    SRZ_HEADER;
    SrhHandle id;
    const char *type;
    const char *name;
    int32_t priority;
    uint32_t active;
    uint32_t parked;
    /* Nonempty for an unavailable slot. It has active=0, no native instance,
       and retains its intended rack/parked placement. Reload retries loading. */
    const char *load_error;
} SrzCardInfo;

typedef struct SrzSpace {
    SRZ_HEADER;
    SrhHandle id;
    const char *name;
    uint64_t maximum;
    uint8_t fallback;
    uint8_t reserved[3];
    uint32_t random;
    SrzResolver resolver;
} SrzSpace;

typedef struct SrzClock {
    SRZ_HEADER;
    uint32_t hz;
    uint32_t reserved;
    uint64_t ticks;
    uint64_t phase;
    uint64_t order;
} SrzClock;

typedef struct SrzTrace {
    SRZ_HEADER;
    uint64_t sequence;
    uint64_t parent;
    uint64_t time;
    uint64_t ticks[3];
    SrhHandle master;
    SrhHandle space;
    uint64_t address;
    uint32_t operation;
    uint32_t depth;
    uint32_t kind;
    uint64_t instruction;
    uint8_t value;
    SrhStatus result;
    /* Responder handles live in the result's handle arena. */
    uint32_t responder_offset;
    uint32_t responder_count;
} SrzTrace;

typedef struct SrzBreakpoint {
    SRZ_HEADER;
    uint64_t id;
    SrhHandle card;
    SrhHandle space;
    uint64_t first;
    uint64_t last;
    uint32_t operations;
    uint32_t enabled;
} SrzBreakpoint;

typedef struct SrzProperty {
    SRZ_HEADER;
    const char *name;
    const char *group;
    const char *description;
    const char *enum_labels;
    uint32_t kind;
    uint32_t bits;
    uint32_t base;
    uint32_t ui_flags;
    uint32_t editable;
    SrhValue value;
} SrzProperty;

/* Plugin setting metadata.  Callback pointers and contexts never cross this
   boundary: only the owning plugin keeps them. */
typedef struct SrzConfigEntry {
    SRZ_HEADER;
    const char *category;
    const char *name;
    const char *label;
    const char *description;
    const char *enum_labels;
    const char *default_value;
    const char *provider;
    uint32_t type;
    SrhHandle owner;
} SrzConfigEntry;

typedef struct SrzTextEndpoint {
    SRZ_HEADER;
    SrhHandle card;
    /* Input names live in the result's string-handle arena. */
    uint32_t input_offset;
    uint32_t input_count;
} SrzTextEndpoint;

typedef struct SrzVideoSurface {
    SRZ_HEADER;
    SrhHandle id;
    SrhHandle owner;
    uint32_t width;
    uint32_t height;
    SrhVideoFormat format;
    SrhVideoFlags flags;
    uint32_t reserved;
} SrzVideoSurface;

typedef struct SrzAudioSource {
    SRZ_HEADER;
    SrhHandle id;
    SrhHandle owner;
    const char *name;
    uint32_t volume_percent;
    uint32_t muted;
    uint32_t active;
    /* Peak after source gain, pan and mute, before summing; S16 full scale is 32768. */
    uint32_t level_peak;
} SrzAudioSource;

typedef struct SrzAudioDiagnostics {
    SRZ_HEADER;
    uint64_t queued_frames;
    uint64_t queue_capacity_frames;
    uint64_t dropped_frames;
    uint64_t underflow_frames;
    uint64_t source_errors;
} SrzAudioDiagnostics;

/* Opaque provider payload copied from the owning card.  The engine never
   interprets `data`; only a matching tool knows its schema. */
typedef struct SrzProviderData {
    SRZ_HEADER;
    SrhHandle owner;
    const char *name;
    const char *protocol;
    const char *data;
    uint64_t data_size;
    uint32_t flags;
    uint32_t reserved;
} SrzProviderData;

/* Plugin-owned project chunk, hex encoded exactly as stored in project JSON. */
typedef struct SrzPluginData {
    SRZ_HEADER;
    SrhHandle owner;
    const char *hex;
    uint64_t size;
} SrzPluginData;

typedef struct SrzInputRecord {
    SRZ_HEADER;
    uint64_t timestamp;
    const char *endpoint;
    uint8_t value;
    uint8_t reserved[7];
} SrzInputRecord;

/* Add-card metadata copied from a card plugin descriptor.  The plugin library
   is unloaded before this call returns. */
typedef struct SrzPluginDescriptor {
    SRZ_HEADER;
    const char *path;
    const char *id;
    const char *category;
    const char *name;
    const char *description;
    const char *default_config_json;
    const char *io_space_config_key;
    const char *base_config_key;
    uint64_t default_base;
    uint64_t default_size;
    uint64_t default_reset_vector;
    int32_t default_priority;
    uint32_t default_clock;
    uint32_t flags;
    uint32_t image_slot_offset;
    uint32_t image_slot_count;
} SrzPluginDescriptor;

typedef struct SrzDisassembly {
    SRZ_HEADER;
    uint32_t ok;
    uint32_t instruction_bytes;
    uint32_t cycles;
    uint32_t byte_count;
    uint8_t bytes[32];
    char text[256];
} SrzDisassembly;

typedef struct SrzError {
    SRZ_HEADER;
    SrhStatus status;
    uint32_t reserved;
    uint64_t sequence;
    char message[512];
} SrzError;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_abi_version(void);
/* Product version of the engine, NUL-terminated and valid for the process
   lifetime. This is distinct from srz80_engine_abi_version(), which reports
   the C ABI contract revision. */
SRZ_EXPORT const char *SRZ_CALL srz80_engine_version(void);
/* Capability bits, valid for any engine instance. */
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_capabilities(const SrzEngine *engine);
/* Creates an engine in the explicitly stopped state.  An empty slice uses the
   executable-relative `plugins` directory. */
SRZ_EXPORT SrzEngine *SRZ_CALL srz80_engine_create(SrzSlice plugins_directory);
/* Creates a detached engine that shares `engine`'s plugin directory and host
   configuration.  A candidate owns no rack until a load succeeds; loading into
   it never disturbs the active engine. */
SRZ_EXPORT SrzEngine *SRZ_CALL srz80_engine_candidate_create(const SrzEngine *engine);
/* Installs the candidate's rack into `engine` and releases the candidate.  On
   failure both engines stay valid and unchanged.  Card instances prepared by
   the candidate move with it; no project, state or path is re-interpreted. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_replace(SrzEngine *engine, SrzEngine *candidate);
/* Releases an engine and every card it owns.  Also accepts a candidate that
   was never installed. */
SRZ_EXPORT void SRZ_CALL srz80_engine_destroy(SrzEngine *engine);
/* Message for the most recent failing call on this engine's thread.  Always
   NUL-terminates a non-empty buffer; returns the full length. */
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_last_error(const SrzEngine *engine, char *buffer,
                                                     uint64_t capacity);

/* ------------------------------------------------------------------ */
/* Result arena                                                        */
/* ------------------------------------------------------------------ */

SRZ_EXPORT SrzResult *SRZ_CALL srz80_engine_result_create(SrzEngine *engine);
SRZ_EXPORT void SRZ_CALL srz80_engine_result_destroy(SrzEngine *engine, SrzResult *result);
/* Invalidates every pointer previously returned by an accessor for this
   result.  Subsequent fills reuse the retained storage. */
SRZ_EXPORT void SRZ_CALL srz80_engine_result_clear(SrzResult *result);

SRZ_EXPORT const SrzCardInfo *SRZ_CALL srz80_engine_result_cards(const SrzResult *result,
                                                                 uint32_t *count);
SRZ_EXPORT const SrzCardInfo *SRZ_CALL srz80_engine_result_all_cards(const SrzResult *result,
                                                                     uint32_t *count);
SRZ_EXPORT const SrzCardInfo *SRZ_CALL srz80_engine_result_removed_cards(const SrzResult *result,
                                                                         uint32_t *count);
SRZ_EXPORT const SrzSpace *SRZ_CALL srz80_engine_result_spaces(const SrzResult *result,
                                                               uint32_t *count);
SRZ_EXPORT const SrzBreakpoint *SRZ_CALL srz80_engine_result_breakpoints(const SrzResult *result,
                                                                         uint32_t *count);
SRZ_EXPORT const SrzTrace *SRZ_CALL srz80_engine_result_trace(const SrzResult *result,
                                                              uint32_t *count);
/* Responder handles referenced by SrzTrace.responder_offset/count. */
SRZ_EXPORT const SrhHandle *SRZ_CALL srz80_engine_result_handles(const SrzResult *result,
                                                                 uint32_t *count);
SRZ_EXPORT const char *const *SRZ_CALL srz80_engine_result_logs(const SrzResult *result,
                                                                uint32_t *count);
SRZ_EXPORT const SrzProperty *SRZ_CALL srz80_engine_result_properties(const SrzResult *result,
                                                                      uint32_t *count);
SRZ_EXPORT const SrzConfigEntry *SRZ_CALL srz80_engine_result_config_entries(
    const SrzResult *result, uint32_t *count);
SRZ_EXPORT const SrzTextEndpoint *SRZ_CALL srz80_engine_result_text_endpoints(
    const SrzResult *result, uint32_t *count);
/* Input names referenced by SrzTextEndpoint.input_offset/count. */
SRZ_EXPORT const char *const *SRZ_CALL srz80_engine_result_input_names(const SrzResult *result,
                                                                       uint32_t *count);
SRZ_EXPORT const SrzVideoSurface *SRZ_CALL srz80_engine_result_video_surfaces(
    const SrzResult *result, uint32_t *count);
SRZ_EXPORT const SrzAudioSource *SRZ_CALL srz80_engine_result_audio_sources(
    const SrzResult *result, uint32_t *count);
SRZ_EXPORT const SrzProviderData *SRZ_CALL srz80_engine_result_providers(
    const SrzResult *result, uint32_t *count);
SRZ_EXPORT const SrzPluginData *SRZ_CALL srz80_engine_result_plugin_data(
    const SrzResult *result, uint32_t *count);
SRZ_EXPORT const char *const *SRZ_CALL srz80_engine_result_paths(const SrzResult *result,
                                                                 uint32_t *count);
SRZ_EXPORT const char *const *SRZ_CALL srz80_engine_result_endpoints(const SrzResult *result,
                                                                     uint32_t *count);
SRZ_EXPORT const SrzInputRecord *SRZ_CALL srz80_engine_result_input_records(
    const SrzResult *result, uint32_t *count);
SRZ_EXPORT const SrzPluginDescriptor *SRZ_CALL srz80_engine_result_plugin_descriptor(
    const SrzResult *result);
/* Image-slot labels referenced by SrzPluginDescriptor.image_slot_offset/count. */
SRZ_EXPORT const char *const *SRZ_CALL srz80_engine_result_image_slots(const SrzResult *result,
                                                                       uint32_t *count);

/* ------------------------------------------------------------------ */
/* Project, execution state and plugin-owned project data              */
/* ------------------------------------------------------------------ */

/* Replaces the rack with the default empty machine and performs a cold reset. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_new_project(SrzEngine *engine);
/* Loads a project file. Unreadable images and unresolved/unloadable libraries
   become inert unavailable slots. Invalid documents and card creation failures
   reject the candidate and preserve the active rack. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_load_project(SrzEngine *engine, SrzSlice project_path,
                                                        SrzSlice plugins_directory,
                                                        SrzRunState initial_state);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_load_project_json(SrzEngine *engine, SrzSlice json,
                                                             SrzSlice project_directory,
                                                             SrzSlice plugins_directory,
                                                             SrzRunState initial_state);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_load_state(SrzEngine *engine, SrzSlice json);
/* Writes the execution-state JSON.  Pass buffer == NULL to query the size. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_save_state(SrzEngine *engine, uint32_t include_trace,
                                                      char *buffer, uint64_t capacity,
                                                      uint64_t *size);
/* Opaque per-card project chunks collected from the active rack. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_plugin_project_data(SrzEngine *engine,
                                                               SrzResult *result);
/* Restores one card's opaque project chunk after card creation. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_load_plugin_data(SrzEngine *engine, SrhHandle owner,
                                                            SrzSlice hex);

/* ------------------------------------------------------------------ */
/* Rack and cards                                                      */
/* ------------------------------------------------------------------ */

typedef struct SrzImagePart {
    SRZ_HEADER;
    const uint8_t *data;
    uint64_t size;
} SrzImagePart;

typedef struct SrzCardRequest {
    SRZ_HEADER;
    SrhHandle space;
    uint64_t base;
    uint64_t size;
    uint64_t reset_vector;
    int32_t priority;
    uint32_t clock;
    /* Plugin id, for example "memory_ram". */
    SrzSlice type;
    SrzSlice config_json;
    /* Optional override of the engine's plugin directory. */
    SrzSlice plugin_directory;
    /* Ordered image parts borrowed only for the duration of the call. */
    const SrzImagePart *images;
    uint32_t image_count;
    uint32_t reserved;
} SrzCardRequest;

SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_add_card(SrzEngine *engine,
                                                    const SrzCardRequest *request,
                                                    SrhHandle *card);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_park(SrzEngine *engine, SrhHandle card);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_plug(SrzEngine *engine, SrhHandle card);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_put_away(SrzEngine *engine, SrhHandle card);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_remove(SrzEngine *engine, SrhHandle card);
/* Rack order is presentation and persistence order, independent of routing.
   A valid permutation of the existing cards preserves instances, handles,
   running state, clocks, memory and bus routing. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_reorder_cards(SrzEngine *engine, const SrhHandle *order,
                                                         uint32_t count);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_card_alive(const SrzEngine *engine, SrhHandle card);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_set_card_name(SrzEngine *engine, SrhHandle card,
                                                         SrzSlice name);
/* Moves a card's clock subscriptions to another master clock without
   recreating the card or resetting the rack. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_set_card_clock(SrzEngine *engine, SrhHandle card,
                                                          uint32_t clock);

/* Card enumerations write their own slots in `result`. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_cards(const SrzEngine *engine, SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_all_cards(const SrzEngine *engine, SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_removed_cards(const SrzEngine *engine,
                                                         SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_spaces(const SrzEngine *engine, SrzResult *result);
/* Looks up an address space by name.  A missing space returns SRH_NOT_FOUND and
   leaves `space` untouched. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_find_space(const SrzEngine *engine, SrzSlice name,
                                                      SrhHandle *space);
/* Fills all three master clocks. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_clocks(const SrzEngine *engine, SrzClock *clocks,
                                                  uint32_t capacity, uint32_t *count);

/* Resolves a plugin id inside `directory` (empty means the engine's plugin
   directory).  Caller-owned path buffer; NULL queries the length. */
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_resolve_plugin(const SrzEngine *engine,
                                                         SrzSlice directory, SrzSlice type,
                                                         char *buffer, uint64_t capacity);
/* Plugin library paths found in `directory`. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_discover_plugins(const SrzEngine *engine,
                                                            SrzSlice directory,
                                                            SrzResult *result);
/* Reads one plugin's descriptor by loading the library, querying metadata and
   unloading it again.  Descriptor strings are copied into the result. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_inspect_plugin(const SrzEngine *engine,
                                                          SrzSlice path, SrzResult *result);

/* ------------------------------------------------------------------ */
/* Run, clock and scheduler operations                                 */
/* ------------------------------------------------------------------ */

SRZ_EXPORT SrzRunState SRZ_CALL srz80_engine_run_state(const SrzEngine *engine);
SRZ_EXPORT void SRZ_CALL srz80_engine_pause(SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_resume(SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_stop(SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_reset(SrzEngine *engine, uint32_t cold);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_step(SrzEngine *engine, uint32_t clock);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_stop_clocks(SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_frequency(SrzEngine *engine, uint32_t clock,
                                                     uint32_t hz);
/* Synchronous run.  Never reads the wall clock. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_run(SrzEngine *engine, uint64_t event_budget,
                                               uint64_t until);
/* Paced run used by the simulation worker.  `wall_budget_ns` bounds the wall
   time spent inside this call; callbacks are never preempted. The Rust backend
   checks the deadline before dispatch and at most every 32 scheduler dispatches,
   so a call can overrun by one batch. Pause and event budgets remain per-dispatch. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_run_slice(SrzEngine *engine, uint64_t event_budget,
                                                     uint64_t until, uint64_t wall_budget_ns);
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_now(const SrzEngine *engine);
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_time_ns(const SrzEngine *engine);
SRZ_EXPORT SrzTimeMode SRZ_CALL srz80_engine_time_mode(const SrzEngine *engine);
SRZ_EXPORT void SRZ_CALL srz80_engine_set_time_mode(SrzEngine *engine, SrzTimeMode mode,
                                                    uint64_t epoch);
SRZ_EXPORT void SRZ_CALL srz80_engine_seed(SrzEngine *engine, uint64_t value);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_random(SrzEngine *engine, uint64_t *value);
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_stop_reason(const SrzEngine *engine, char *buffer,
                                                      uint64_t capacity);
/* True when any card or breakpoint needs an instruction-boundary call. */
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_boundary_required(const SrzEngine *engine);

/* Host-owned callback registration.  Callbacks run on the engine's thread
   during dispatch.  Contexts stay valid until cancelled or the engine dies. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_subscribe(SrzEngine *engine, SrhHandle owner,
                                                     uint32_t clock, SrhCallback callback,
                                                     void *context, SrhHandle *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_schedule(SrzEngine *engine, SrhHandle owner,
                                                    uint64_t delay, SrhCallback callback,
                                                    void *context, SrhHandle *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_cancel(SrzEngine *engine, SrhHandle event);

/* ------------------------------------------------------------------ */
/* Bus                                                                 */
/* ------------------------------------------------------------------ */

SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_read(SrzEngine *engine, SrhHandle master,
                                                SrhHandle space, uint64_t address, uint8_t *value,
                                                uint32_t peek);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_write(SrzEngine *engine, SrhHandle master,
                                                 SrhHandle space, uint64_t address, uint8_t value);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_read_word(SrzEngine *engine, SrhHandle master,
                                                     SrhHandle space, uint64_t address,
                                                     uint32_t *value);

/* ------------------------------------------------------------------ */
/* Debugger: breakpoints, boundaries, trace and disassembly            */
/* ------------------------------------------------------------------ */

SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_add_breakpoint(SrzEngine *engine, SrhHandle card,
                                                         SrhHandle space, uint64_t first,
                                                         uint64_t last, uint32_t operations);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_remove_breakpoint(SrzEngine *engine, uint64_t id);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_set_breakpoint_enabled(SrzEngine *engine, uint64_t id,
                                                                  uint32_t enabled);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_breakpoints(const SrzEngine *engine, SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_boundary(SrzEngine *engine, SrhHandle card,
                                                    SrhHandle space, uint64_t pc);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_boundary_ex(SrzEngine *engine, SrhHandle card,
                                                       SrhHandle space, uint64_t pc,
                                                       uint32_t instruction_bytes,
                                                       uint32_t cycles);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_request_stop(SrzEngine *engine, SrhHandle card,
                                                        SrzSlice reason);
/* Availability is independent of decode success. Revision changes on live
   policy changes; clients must discard cached/results from older revisions. */
#define SRZ_DISASSEMBLY_UNSUPPORTED 0u
#define SRZ_DISASSEMBLY_ENABLED 1u
#define SRZ_DISASSEMBLY_DISABLED 2u
typedef struct SrzDisassemblyAvailability {
    SRZ_HEADER;
    uint32_t state;
    uint32_t reserved;
    uint64_t revision;
    char message[256];
} SrzDisassemblyAvailability;
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_disassembly_availability(
    SrzEngine *engine, SrhHandle card, SrzDisassemblyAvailability *out);
/* Disabled requests return SRH_UNAVAILABLE without probing memory/decoding. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_disassemble(SrzEngine *engine, SrhHandle card,
                                                       SrhHandle space, uint64_t pc,
                                                       SrzDisassembly *out);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_trace(const SrzEngine *engine, SrzResult *result);
SRZ_EXPORT void SRZ_CALL srz80_engine_clear_trace(SrzEngine *engine);
/* `operations` is a mask of SRH_READ / SRH_WRITE. */
SRZ_EXPORT void SRZ_CALL srz80_engine_set_trace_capture(SrzEngine *engine, uint32_t enabled,
                                                        uint32_t operations);
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_trace_capture(const SrzEngine *engine);
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_dropped(const SrzEngine *engine);

/* ------------------------------------------------------------------ */
/* Signals                                                             */
/* ------------------------------------------------------------------ */

SRZ_EXPORT SrhHandle SRZ_CALL srz80_engine_signal(SrzEngine *engine, SrzSlice name, int32_t idle);
SRZ_EXPORT SrhHandle SRZ_CALL srz80_engine_find_signal(const SrzEngine *engine, SrzSlice name);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_drive_signal(SrzEngine *engine, SrhHandle owner,
                                                        SrhHandle signal, int32_t value,
                                                        int32_t priority);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_release_signal(SrzEngine *engine, SrhHandle owner,
                                                          SrhHandle signal);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_sample_signal(const SrzEngine *engine, SrhHandle signal,
                                                         int32_t *value);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_subscribe_signal(SrzEngine *engine, SrhHandle owner,
                                                            SrhHandle signal,
                                                            SrhSignalCallback callback,
                                                            void *context, SrhHandle *result);

/* ------------------------------------------------------------------ */
/* Deterministic input                                                 */
/* ------------------------------------------------------------------ */

SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_register_input(SrzEngine *engine, SrhHandle owner,
                                                          SrzSlice name, uint32_t capacity,
                                                          SrhHandle *endpoint);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_subscribe_input_due(SrzEngine *engine,
                                                               SrhHandle owner,
                                                               SrhHandle endpoint,
                                                               SrhCallback callback,
                                                               void *context, SrhHandle *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_enqueue_input(SrzEngine *engine, SrzSlice endpoint,
                                                         uint64_t timestamp, uint8_t value);
/* `source` is an opaque tool-owned cancellation identity; `expected_owner`
   rejects the batch when the endpoint changed owner. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_enqueue_input_batch(SrzEngine *engine,
                                                               SrzSlice endpoint,
                                                               uint64_t timestamp,
                                                               const uint8_t *bytes,
                                                               uint64_t size, uint64_t source,
                                                               SrhHandle expected_owner);
SRZ_EXPORT void SRZ_CALL srz80_engine_cancel_input_source(SrzEngine *engine, uint64_t source);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_input_due(const SrzEngine *engine, SrhHandle endpoint,
                                                     uint64_t *count);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_input_pop(SrzEngine *engine, SrhHandle endpoint,
                                                     uint64_t *timestamp, uint8_t *value);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_input_endpoints(const SrzEngine *engine,
                                                           SrzResult *result);
/* Replaces the persistent deterministic input script.  Each record is an
   (timestamp, endpoint, value) tuple. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_set_input_records(SrzEngine *engine,
                                                             const SrzInputRecord *records,
                                                             uint32_t count);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_input_records(const SrzEngine *engine,
                                                         uint32_t persistent_only,
                                                         SrzResult *result);

/* ------------------------------------------------------------------ */
/* Text and video extensions                                           */
/* ------------------------------------------------------------------ */

/* Bounded host-copied text query.  `size` carries the buffer capacity in and
   the copied size out; `total` receives the endpoint's total length. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_text_query(const SrzEngine *engine, SrhHandle card,
                                                      uint64_t offset, uint8_t *buffer,
                                                      uint32_t *size, uint32_t *total);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_text_endpoints(const SrzEngine *engine,
                                                          SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_video_surfaces(const SrzEngine *engine,
                                                          SrzResult *result);
/* Caller-owned RGBA8 copy; `size` carries capacity in and copied bytes out. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_video_read(SrzEngine *engine, SrhHandle surface,
                                                      uint64_t offset, uint8_t *buffer,
                                                      uint32_t *size, uint32_t *total);

/* Query immediately after video_read on the same engine thread, without
   advancing simulation, to pair pixels and scanout. Untimed surfaces return
   SRH_UNAVAILABLE. Output is committed only on success. Initialize SRH_HEADER. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_video_timing(SrzEngine *engine, SrhHandle surface,
                                                        SrhVideoTiming *timing);

/* ------------------------------------------------------------------ */
/* Audio                                                               */
/* ------------------------------------------------------------------ */

SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_audio_sample_rate(const SrzEngine *engine);
/* Sets the canonical mixer rate (8000..384000 Hz), resets queued audio and
   preserves every registered source's declared native rate. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_set_sample_rate(SrzEngine *engine,
                                                                 uint32_t sample_rate);
typedef uint32_t SrzAudioResampling;
enum {
    SRZ_AUDIO_RESAMPLE_NONE = 0,
    SRZ_AUDIO_RESAMPLE_LINEAR = 1,
    SRZ_AUDIO_RESAMPLE_BOXCAR = 2,
    SRZ_AUDIO_RESAMPLE_COSINE = 3,
    SRZ_AUDIO_RESAMPLE_SINC = 4
};
SRZ_EXPORT SrzAudioResampling SRZ_CALL srz80_engine_audio_resampling(const SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_set_resampling(SrzEngine *engine,
                                                                SrzAudioResampling method);
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_audio_channels(const SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_register_source(SrzEngine *engine,
                                                                 SrhHandle owner,
                                                                 uint32_t sample_rate,
                                                                 uint32_t channels,
                                                                 SrhAudioFormat format,
                                                                 SrzSlice name,
                                                                 SrhAudioRender render,
                                                                 void *render_context,
                                                                 SrhHandle *source);
/* Drains up to `frames` interleaved S16 stereo frames from the bounded queue. */
/* Capture frontend bridge; engine-thread only. A zero format clears capture.
   push copies at most 8192 frames; samples must contain frames * channels floats. */
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_audio_input_requested(SrzEngine *engine);
SRZ_EXPORT void SRZ_CALL srz80_engine_audio_input_push(SrzEngine *engine,
    const float *samples, uint32_t frames, uint32_t rate, uint32_t channels);
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_audio_read(SrzEngine *engine, int16_t *interleaved,
                                                     uint32_t frames);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_diagnostics(const SrzEngine *engine,
                                                             SrzAudioDiagnostics *out);
SRZ_EXPORT void SRZ_CALL srz80_engine_audio_set_queue_capacity(SrzEngine *engine, uint64_t frames);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_sources(const SrzEngine *engine,
                                                         SrzResult *result);
/* Source volume is limited to 0..150 percent. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_set_source_volume(SrzEngine *engine,
                                                                   SrhHandle source,
                                                                   uint32_t percent);
/* Stereo balance: -64 (left), 0 (center), +63 (right). Unknown sources return 0. */
SRZ_EXPORT int32_t SRZ_CALL srz80_engine_audio_source_pan(const SrzEngine *engine, SrhHandle source);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_set_source_pan(SrzEngine *engine,
                                                              SrhHandle source, int32_t pan);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_set_source_muted(SrzEngine *engine,
                                                                  SrhHandle source,
                                                                  uint32_t muted);
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_audio_master_volume(const SrzEngine *engine);
/* Peaks in the final stereo PCM since the previous query; querying clears them. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_audio_master_levels(const SrzEngine *engine,
                                                               uint32_t *left, uint32_t *right);
SRZ_EXPORT void SRZ_CALL srz80_engine_audio_set_master_volume(SrzEngine *engine, uint32_t percent);
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_audio_dc_offset_correction(const SrzEngine *engine);
SRZ_EXPORT void SRZ_CALL srz80_engine_audio_set_dc_offset_correction(SrzEngine *engine,
                                                                     uint32_t enabled);
SRZ_EXPORT uint32_t SRZ_CALL srz80_engine_audio_software_clipping(const SrzEngine *engine);
SRZ_EXPORT void SRZ_CALL srz80_engine_audio_set_software_clipping(SrzEngine *engine,
                                                                  uint32_t enabled);

/* ------------------------------------------------------------------ */
/* Card properties                                                     */
/* ------------------------------------------------------------------ */

SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_properties(SrzEngine *engine, SrhHandle card,
                                                      SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_edit_property(SrzEngine *engine, SrhHandle card,
                                                         uint32_t index, const SrhValue *value);

/* ------------------------------------------------------------------ */
/* Host configuration                                                  */
/* ------------------------------------------------------------------ */

SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_load_config(SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_save_config(const SrzEngine *engine);
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_config_path(const SrzEngine *engine, char *buffer,
                                                      uint64_t capacity);
/* Reads one key; `fallback` is used when the key is absent. */
SRZ_EXPORT uint64_t SRZ_CALL srz80_engine_config_value(const SrzEngine *engine, SrzSlice key,
                                                       SrzSlice fallback, char *buffer,
                                                       uint64_t capacity);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_config_set(SrzEngine *engine, SrzSlice key,
                                                      SrzSlice value);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_config_entries(const SrzEngine *engine,
                                                          SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_config_entry_get(const SrzEngine *engine, SrzSlice key,
                                                            char *buffer, uint64_t capacity,
                                                            uint64_t *size);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_config_entry_set(SrzEngine *engine, SrzSlice key,
                                                            SrzSlice value);
/* Registers plugin-owned setting metadata.  The engine copies the metadata and
   keeps the callbacks; `context` identifies the registration for removal. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_register_config_entry(SrzEngine *engine,
                                                                 const SrhConfigEntry *entry);
SRZ_EXPORT void SRZ_CALL srz80_engine_unregister_config_entries(SrzEngine *engine,
                                                                void *context);

/* ------------------------------------------------------------------ */
/* Logs and card data providers                                        */
/* ------------------------------------------------------------------ */

SRZ_EXPORT void SRZ_CALL srz80_engine_log(SrzEngine *engine, SrzSlice message);
SRZ_EXPORT void SRZ_CALL srz80_engine_clear_logs(SrzEngine *engine);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_logs(const SrzEngine *engine, SrzResult *result);
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_provider_data(SrzEngine *engine, SrzResult *result);
/* Opaque provider command.  `kind` is SRH_PROVIDER_INTERACTION or
   SRH_PROVIDER_CONFIGURE; the engine transports it without interpretation. */
SRZ_EXPORT SrhStatus SRZ_CALL srz80_engine_provider_command(SrzEngine *engine, SrhHandle owner,
                                                            uint32_t kind, uint64_t revision,
                                                            SrzSlice payload);

#ifdef __cplusplus
}
#endif
#endif
