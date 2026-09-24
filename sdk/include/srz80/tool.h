#ifndef SRZ80_TOOL_H
#define SRZ80_TOOL_H
#include <srz80/providers.h>

#include <srz80/abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tool plugins are trusted GUI extensions. Their service boundary is a
   versioned C ABI, but their draw callback is deliberately build-locked to
   the exact Dear ImGui version reported by the host. */
enum { SRT_STOPPED = 0, SRT_PAUSED = 1, SRT_RUNNING = 2 };

/* Tool capabilities appended to SrhToolPlugin. */
enum SrhToolFlags {
    /* A focused tool owns the host's Ctrl+N/O/S/Shift+S file shortcuts. */
    Srh_TOOL_CLAIM_FILE_SHORTCUTS = 1u << 0,
    /* Project state is opaque UTF-8 text, not a project-relative file path. */
    Srh_TOOL_PROJECT_STATE_TEXT = 1u << 1,
};

/* SrhToolTextFormat capability: the registered file is opaque binary data
   interpreted only by the registering tool. */
enum SrhToolTextFormatFlags {
    SRH_TEXT_FORMAT_BINARY = 1u << 0,
};

typedef struct SrhToolSpace {
    SRH_HEADER;
    SrhHandle id;
    uint64_t maximum;
    char name[128];
} SrhToolSpace;

typedef struct SrhToolMemorySegment {
    SRH_HEADER;
    uint64_t address;
    const uint8_t *bytes;
    uint64_t size;
} SrhToolMemorySegment;

/* A tool may advertise a project text format. The host includes registered
   formats in the project tree's New File menu, creates the selected file, and
   passes its host-owned text document to the tool. Extensions omit the dot. */
typedef SrhStatus(SRH_CALL *SrhToolProjectFileOpen)(void *context, const char *path,
                                                    const char *text, uint64_t size,
                                                    uint64_t cursor);
typedef struct SrhToolTextFormat {
    SRH_HEADER;
    const char *plugin_id;
    const char *extension;
    const char *label;
    void *handler_context;
    SrhToolProjectFileOpen open;
    /* Appended in SRH_ABI 1 (tail field, guarded by struct_size). */
    uint32_t flags;
} SrhToolTextFormat;

/* Active card description returned by the optional card-inspection service. */
typedef struct SrhToolCard {
    SRH_HEADER;
    SrhHandle id;
    uint32_t active;
    char type[128];
    char name[128];
} SrhToolCard;

/* Generic card property descriptor returned by card-property inspection. */
typedef struct SrhToolProperty {
    SRH_HEADER;
    char name[128];
    char group[128];
    char description[256];
    char enum_labels[512];
    uint32_t kind, bits, base, editable;
    uint32_t ui_flags;
} SrhToolProperty;

/* Dialogs are asynchronous. Poll until pending=0, then release the request.
   cancelled=1 means no path was approved. Paths use UTF-8; poll with a NULL
   path and zero capacity to query required_size (including NUL). */
typedef struct SrhToolFileResult {
    SRH_HEADER;
    uint32_t pending;
    uint32_t cancelled;
    uint64_t required_size;
} SrhToolFileResult;

/* File-dialog filters are borrowed for the duration of the request call.
   `patterns` uses SDL's semicolon-separated extension syntax, e.g.
   "bin;hex;ihx". The host copies both strings for asynchronous use. */
typedef struct SrhToolFileFilter {
    const char *name;
    const char *patterns;
} SrhToolFileFilter;

/* Named input: at most 64 pending requests / 256 KiB per client; 64 KiB
   per batch. UINT64_MAX timestamps at worker acceptance. release does not
   cancel a submitted operation. client is a stable tool-owned identity. */
typedef struct SrhToolInputResult {
    SRH_HEADER;
    uint32_t pending;
    SrhStatus status;
} SrhToolInputResult;
typedef struct SrhToolRuntime {
    SRH_HEADER;
    uint64_t generation, time_ns;
    uint32_t stopped, running;
} SrhToolRuntime;
typedef struct SrhToolHostV1 {
    SRH_HEADER;
    void *context;
    const char *imgui_version;
    void *imgui_context;
    void *(*imgui_alloc)(size_t size, void *user_data);
    void (*imgui_free)(void *pointer, void *user_data);
    void *imgui_allocator_context;
    SrhStatus(SRH_CALL *log)(void *context, const char *message);
    uint32_t(SRH_CALL *space_count)(void *context);
    SrhStatus(SRH_CALL *space_info)(void *context, uint32_t index, SrhToolSpace *result);
    SrhStatus(SRH_CALL *peek)(void *context, SrhHandle space, uint64_t address, uint8_t *value);
    /* The host rejects this call unless the rack is explicitly stopped. It
       preflights the range, optionally cold-resets, then performs semantic
       per-byte bus writes. `written` reports completed writes on failure. */
    SrhStatus(SRH_CALL *load_memory)(void *context, SrhHandle space, uint64_t address,
                                    const uint8_t *bytes, uint64_t size,
                                    uint32_t reset_before_load, uint64_t *written);
    uint32_t(SRH_CALL *run_state)(void *context);
    SrhStatus(SRH_CALL *run)(void *context);
    /* Appended services. Preflight failures write nothing and never reset.
       failed_segment is UINT32_MAX on success or a non-segment-specific error.
       written counts successful transactions across all segments, not bytes
       touched by a failing transaction. Segment arrays must be nonempty and
       sorted, with nonempty, nonoverlapping ranges. No rollback is attempted. */
    SrhStatus(SRH_CALL *load_memory_segments)(void *context, SrhHandle space,
        const SrhToolMemorySegment *segments, uint32_t segment_count,
        uint32_t reset_before_load, uint32_t *failed_segment, uint64_t *written);
    /* The plugin supplies its own nonempty filter list; the host copies it
       before starting the asynchronous native dialog. */
    SrhStatus(SRH_CALL *file_dialog_request)(void *context, uint32_t save,
        const SrhToolFileFilter *filters, uint32_t filter_count, SrhHandle *request);
    SrhStatus(SRH_CALL *file_dialog_poll)(void *context, SrhHandle request,
        SrhToolFileResult *result, char *path, uint64_t capacity);
    SrhStatus(SRH_CALL *file_dialog_release)(void *context, SrhHandle request);
    /* Configuration service (appended in SRH_ABI 1, guarded by struct_size).
       Tools use these to add entries to the same Settings window as card
       plugins.  entry_context is the pointer passed as SrhConfigEntry::context;
       unregister removes every entry registered with that context. */
    SrhStatus(SRH_CALL *config_register)(void *context, const SrhConfigEntry *entry);
    SrhStatus(SRH_CALL *config_unregister)(void *context, void *entry_context);
    SrhStatus(SRH_CALL *config_get)(void *context, const char *key, char *value, uint32_t capacity);
    /* Queues owned strings in controller order; SRH_OK is transport acceptance. */
    SrhStatus(SRH_CALL *config_set)(void *context, const char *key, const char *value);
    /* Card inspection service (appended in SRH_ABI 1, guarded by struct_size).
       Tools use these to discover active cards and inspect/edit their generic
       card properties. Property edits follow the same rule as the built-in
       Device Inspector: writes are rejected while the simulation is running. */
    uint32_t(SRH_CALL *card_count)(void *context);
    SrhStatus(SRH_CALL *card_info)(void *context, uint32_t index, SrhToolCard *result);
    uint32_t(SRH_CALL *card_property_count)(void *context, SrhHandle card);
    SrhStatus(SRH_CALL *card_property_info)(void *context, SrhHandle card, uint32_t index,
                                            SrhToolProperty *result);
    SrhStatus(SRH_CALL *card_property_get)(void *context, SrhHandle card, uint32_t index,
                                           SrhValue *value);
    SrhStatus(SRH_CALL *card_property_set)(void *context, SrhHandle card, uint32_t index,
                                           const SrhValue *value);
    /* Copied provider bundle {generation,
       providers:[{owner,name,display_name,protocol,flags,data}]}. `display_name` is the
       owning card's friendly name when configured, otherwise the provider
       name. `data` is an opaque UTF-8 string, interpreted only by the
       tool selected by provider protocol. Read only, no card pointers. Interaction commands wait briefly
       for an authoritative provider update; topology commands may wait longer. */
    SrhStatus(SRH_CALL *provider_data)(void *, char *, uint64_t *);
    SrhStatus(SRH_CALL *provider_command)(void *, SrhHandle, uint64_t generation,
        uint32_t kind, uint64_t revision, const char *, uint64_t);
    /* Returns the active project directory as UTF-8.  The usual two-call
       convention applies: pass NULL to obtain the required size including
       the terminator.  Empty means that the rack has not been saved yet. */
    SrhStatus(SRH_CALL *project_root)(void *context, char *path, uint64_t *size);
    SrhStatus(SRH_CALL *text_format_register)(void *context, const SrhToolTextFormat *format);
    SrhStatus(SRH_CALL *text_format_unregister)(void *context, void *handler_context);
    SrhStatus(SRH_CALL *project_text_update)(void *context, void *handler_context,
        const char *path, const char *text, uint64_t size, uint64_t cursor);
    SrhStatus(SRH_CALL *input_submit)(void *, void *client, uint64_t generation, uint64_t identity, SrhHandle endpoint_owner,
        const char *endpoint, uint64_t time_ns, const uint8_t *, uint64_t size, SrhHandle *request);
    SrhStatus(SRH_CALL *input_poll)(void *, void *client, SrhHandle request, SrhToolInputResult *);
    SrhStatus(SRH_CALL *input_release)(void *, void *client, SrhHandle request);
    /* Increment identity to cancel queued input from earlier playback. Cancellation
       is ordered on the worker; already delivered bytes cannot be retracted.
       The returned request is polled/released exactly like input_submit. */
    SrhStatus(SRH_CALL *input_cancel)(void *, void *client, uint64_t generation,
                                     uint64_t identity, SrhHandle *request);
    SrhStatus(SRH_CALL *runtime_info)(void *, SrhToolRuntime *);
} SrhToolHostV1;

typedef struct SrhToolPlugin {
    SRH_HEADER;
    const char *id;
    const char *name;
    const char *imgui_version;
    SrhStatus(SRH_CALL *create)(const SrhToolHostV1 *host, void **instance);
    void(SRH_CALL *destroy)(void *instance);
    SrhStatus(SRH_CALL *draw)(void *instance, uint32_t *open);
    /* Optional parent menu category. Missing or empty categories appear under
       Misc in the host's Tools menu. */
    const char *category;
    /* Optional capabilities, appended for ABI compatibility. */
    uint32_t flags;
    /* Optional project-owned state. The host calls project_state_get before
       saving a project and stores the returned UTF-8 text under
       `tool_state[tool-id]`. `size` includes the terminating NUL. On loading
       a project it passes that text to project_state_load; an empty string
       means that no state was stored for this tool. Hosts resolve a stored
       relative path against the project before passing it to the tool, unless
       Srh_TOOL_PROJECT_STATE_TEXT is set. */
    SrhStatus(SRH_CALL *project_state_get)(void *instance, char *value, uint64_t *size);
    SrhStatus(SRH_CALL *project_state_load)(void *instance, const char *value);
    /* Called only by the host's Save Project operation.  Tools use this to
       flush project-owned files before the project document is committed. */
    SrhStatus(SRH_CALL *project_save)(void *instance);
    /* GUI-thread, bounded/nonblocking, once per frame even while hidden. */
    SrhStatus(SRH_CALL *background_tick)(void *instance, uint32_t window_visible);
} SrhToolPlugin;

typedef const SrhToolPlugin *(SRH_CALL *SrhToolInit)(const SrhToolHostV1 *host);
SRH_EXPORT const SrhToolPlugin *SRH_CALL srz80_tool_init(const SrhToolHostV1 *host);

#ifdef __cplusplus
}
#endif
#endif
