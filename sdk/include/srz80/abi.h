#ifndef SRZ80_ABI_H
#define SRZ80_ABI_H
#include <stddef.h>
#include <stdint.h>
#if defined(_WIN32)
#define SRH_CALL __cdecl
#ifdef SRZ80_PLUGIN_BUILD
#define SRH_EXPORT __declspec(dllexport)
#else
#define SRH_EXPORT
#endif
#else
#define SRH_CALL
#define SRH_EXPORT __attribute__((visibility("default")))
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define SRH_ABI 1u
#define SRH_HEADER                                                                                           \
    uint32_t abi_version;                                                                                    \
    uint32_t struct_size
#define SRH_INIT(T) SRH_ABI, (uint32_t)sizeof(T)
typedef uint64_t SrhHandle;
typedef int32_t SrhStatus;
enum {
    SRH_OK = 0,
    SRH_ERROR = 1,
    SRH_INVALID = 2,
    SRH_NOT_FOUND = 3,
    SRH_UNAVAILABLE = 4,
    SRH_STOP = 5,
    SRH_CONFLICT = 6
};
enum { SRH_READ = 1, SRH_WRITE = 2, SRH_PEEK = 4 };
enum { SRH_UNSIGNED = 0, SRH_SIGNED = 1, SRH_BOOLEAN = 2, SRH_FIXED = 3, SRH_ENUM = 4, SRH_TEXT = 5 };
/* UTF-8 strings and input buffers are borrowed only for the duration of a call.
   Contexts are opaque cookies, never dereferenced by the other module.
   ABI v1 requires matching OS, architecture and C calling convention. */
typedef SrhStatus(SRH_CALL *SrhRead)(void *, uint64_t, uint8_t *);
typedef SrhStatus(SRH_CALL *SrhWrite)(void *, uint64_t, uint8_t);
typedef SrhStatus(SRH_CALL *SrhReadWord)(void *, uint64_t, uint32_t *);
typedef SrhStatus(SRH_CALL *SrhCallback)(void *);
typedef SrhStatus(SRH_CALL *SrhSignalCallback)(void *, int32_t);
/* Optional state persistence tail callbacks.  save_state writes the plugin's
   private execution state into a caller-owned buffer.  Pass buffer == NULL to
   query the required size.  load_state restores the state previously produced
   by save_state. */
typedef SrhStatus(SRH_CALL *SrhSaveState)(void *, uint8_t *, uint64_t *);
typedef SrhStatus(SRH_CALL *SrhLoadState)(void *, const uint8_t *, uint64_t);
typedef struct SrhImagePart {
    SRH_HEADER;
    const uint8_t *data;
    uint64_t size;
} SrhImagePart;
typedef struct SrhMapping {
    SRH_HEADER;
    SrhHandle space;
    uint64_t first, last;
    int32_t priority;
    void *context;
    SrhRead read;
    SrhWrite write;
    SrhRead peek;
    /* Optional contiguous, side-effect-free little-endian read. */
    SrhReadWord read_word;
} SrhMapping;
typedef struct SrhConfig {
    SRH_HEADER;
    SrhHandle space;
    uint64_t base, size, reset_vector;
    int32_t priority;
    uint32_t clock;
    const uint8_t *image;
    uint64_t image_size;
    /* Appended in SRH_ABI 1 (tail fields, guarded by struct_size).
       UTF-8 JSON object text owned by the host and borrowed only during
       create(). Absence is NULL, 0, meaning {}. Plugins must not retain
       the pointer. */
    const char *config_json;
    uint64_t config_json_size;
    /* Optional ordered image parts, owned by the host and borrowed only during
       create(). Plugins interpret their relationship (concatenation, banks,
       interleaving, etc.); the host does not merge them. */
    const SrhImagePart *images;
    uint32_t image_count;
    /* Optional create() diagnostic output. Cards must check struct_size before
       reading these fields. The host initializes the buffer to an empty string
       before calling create(). A card may replace it with a brief UTF-8 reason
       when create() fails. Capacity includes the trailing NUL; cards must
       always terminate non-empty output. */
    char *error_message;
    uint32_t error_message_capacity;
} SrhConfig;
typedef struct SrhValue {
    SRH_HEADER;
    uint64_t unsigned_value;
    int64_t signed_value;
    char text[128];
} SrhValue;
typedef struct SrhProperty {
    SRH_HEADER;
    const char *name;
    const char *group;
    const char *description; /* optional tooltip; may be NULL or empty */
    uint32_t kind, bits, base, editable;
    const char *enum_labels; /* labels separated by |, indexed from zero */
    /* UI hints (appended in SRH_ABI 1, guarded by struct_size). */
    uint32_t ui_flags;
} SrhProperty;

/* Property only exposed to dedicated tools/debuggers, not the generic
   Device Inspector. */
#define SRH_PROPERTY_HIDE_UI 1u
/* Property may be edited while the simulation is running (live tweaks). */
#define SRH_PROPERTY_LIVE_EDIT 2u
/* Property is transient runtime state and is never written to the project. */
#define SRH_PROPERTY_RUNTIME 4u
/* Property is written to the card's project `config` object after a successful
   generic Device Inspector edit. The property name is the configuration key. */
#define SRH_PROPERTY_PERSISTENT 8u
/* Runtime availability: keep metadata/type, do not read or edit the value. */
#define SRH_PROPERTY_UNAVAILABLE 16u
/* Host extension tables.  Each carries its own version/size header and is
   obtained through the base host query callback.  A plugin requiring an
   extension validates abi_version and struct_size before using it. */
typedef SrhStatus(SRH_CALL *SrhQueryExtension)(void *, const char *, const void **);

/* Generic named-handle lookup.  Kind is "space" or "signal" in Phase 1. */
typedef SrhStatus(SRH_CALL *SrhLookupNamed)(void *, const char *, const char *, SrhHandle *);
typedef struct SrhHostResourcesV1 {
    SRH_HEADER;
    void *context;
    SrhLookupNamed lookup;
} SrhHostResourcesV1;

/* Debugger extension: instruction boundaries with length/cycle metadata,
   plugin-owned disassembly providers, and explicit debugger stops. */
typedef SrhStatus(SRH_CALL *SrhBoundaryEx)(void *, SrhHandle, SrhHandle, uint64_t, uint32_t,
                                            uint32_t);
typedef SrhStatus(SRH_CALL *SrhSetTraceKind)(void *, SrhHandle, uint32_t);
typedef int(SRH_CALL *SrhDebugFlag)(void *);
typedef SrhStatus(SRH_CALL *SrhDisasm)(void *, SrhHandle, SrhHandle, uint64_t, const uint8_t *,
                                        uint32_t, uint32_t *, uint32_t *, char *, uint32_t);
typedef SrhStatus(SRH_CALL *SrhRegisterDisasm)(void *, SrhHandle, SrhDisasm, void *);
typedef SrhStatus(SRH_CALL *SrhRequestStop)(void *, SrhHandle, const char *);
typedef struct SrhHostDebugV1 {
    SRH_HEADER;
    void *context;
    SrhRegisterDisasm register_disasm;
    SrhBoundaryEx boundary_ex;
    SrhRequestStop request_stop;
    SrhSetTraceKind set_trace_kind;
    /* Appended in SRH_ABI 1. Use only after checking struct_size. */
    SrhDebugFlag trace_enabled;
    SrhDebugFlag boundary_required;
    /* Live per-card switch. Message is copied by the host. NULL clears it.
       Defaults to enabled. Use only after checking struct_size. */
    SrhStatus(SRH_CALL *set_disassembly_enabled)(void *, SrhHandle, uint32_t, const char *);
} SrhHostDebugV1;

/* Optional host memory extension. A word read succeeds only when it can
   preserve byte-bus semantics (one contiguous mapping, no active tracing or
   breakpoints); clients must fall back to byte reads otherwise. */
typedef SrhStatus(SRH_CALL *SrhHostReadWord)(void *, SrhHandle, SrhHandle, uint64_t, uint32_t *);
typedef struct SrhHostMemoryV1 {
    SRH_HEADER;
    void *context;
    SrhHostReadWord read_word;
} SrhHostMemoryV1;

/* Deterministic byte-input queues owned by the host. */
typedef SrhStatus(SRH_CALL *SrhInputRegister)(void *, SrhHandle, const char *, uint32_t,
                                               SrhHandle *);
typedef SrhStatus(SRH_CALL *SrhInputDue)(void *, SrhHandle, uint64_t *);
typedef SrhStatus(SRH_CALL *SrhInputPop)(void *, SrhHandle, uint64_t *, uint8_t *);
typedef struct SrhHostInputV1 {
    SRH_HEADER;
    void *context;
    SrhInputRegister register_input;
    SrhInputDue input_due;
    SrhInputPop input_pop;
    /* Optional ABI-1 tail. One subscriber per endpoint. Cancel with host.cancel.
       Callback runs at a scheduler boundary, once per empty-to-due transition. */
    SrhStatus(SRH_CALL *subscribe_due)(void *, SrhHandle, SrhHandle, SrhCallback,
                                      void *, SrhHandle *);
} SrhHostInputV1;

/* Bounded host-copied text/byte-blob query for plugin-owned buffers larger
   than SrhValue::text. */
typedef SrhStatus(SRH_CALL *SrhTextQuery)(void *, uint64_t, uint8_t *, uint32_t *, uint32_t *);
typedef SrhStatus(SRH_CALL *SrhRegisterText)(void *, SrhHandle, SrhTextQuery, void *);
typedef struct SrhHostTextV1 {
    SRH_HEADER;
    void *context;
    SrhRegisterText register_text;
} SrhHostTextV1;

/* Host-copied video surfaces.  Pixel storage and graphics-library objects stay
   on their owning side of the ABI: a provider renders bytes into the caller's
   buffer and never receives an SDL/ImGui object. */
typedef uint32_t SrhVideoFormat;
enum { SRH_VIDEO_RGBA8 = 1 };
typedef uint32_t SrhVideoFlags;
/* The host may apply a user-selected post-processing shader to this surface.
   This is deliberately opt-in: an ordinary video registration is never
   shader-enabled just because the host has a shader selected. */
enum { SRH_VIDEO_ALLOW_SHADER = 1u };
typedef SrhStatus(SRH_CALL *SrhVideoQuery)(void *, uint64_t, uint8_t *, uint32_t *, uint32_t *);
typedef SrhStatus(SRH_CALL *SrhVideoRegister)(void *, SrhHandle, uint32_t, uint32_t,
                                               SrhVideoFormat, SrhVideoQuery, void *,
                                               SrhHandle *);
typedef SrhStatus(SRH_CALL *SrhVideoRegisterEx)(void *, SrhHandle, uint32_t, uint32_t,
                                                  SrhVideoFormat, SrhVideoQuery, void *,
                                                  SrhVideoFlags, SrhHandle *);
/* Card-owned scanout position, copied on the simulation thread. Frame number
   advances at each field/frame boundary, including when pixels do not change.
   Reset may restart it. scanline is the next line to scan; line_count > 0. */
typedef struct SrhVideoTiming {
    SRH_HEADER;
    uint64_t frame_number;
    uint32_t scanline;
    uint32_t line_count;
} SrhVideoTiming;
typedef SrhStatus(SRH_CALL *SrhVideoTimingQuery)(void *, SrhVideoTiming *);
typedef struct SrhHostVideoV1 {
    SRH_HEADER;
    void *context;
    SrhVideoRegister register_video;
    /* Optional tail. Check struct_size before reading this field. */
    SrhVideoRegisterEx register_video_ex;
    /* Optional tail. Attach card scanout timing to an owned surface. No shader
       eligibility without both ALLOW_SHADER and a successful timing query. */
    SrhStatus(SRH_CALL *set_video_timing)(void *, SrhHandle, SrhVideoTimingQuery, void *);
} SrhHostVideoV1;

/* Host audio source registration. Each source declares its native sample rate.
   the host resamples it to sample_rate before mixing. Audio callbacks are
   invoked by the host's simulation thread, never by a device callback.
   start_frame and frames use the source's native-rate timeline. The interleaved
   buffer is owned by the host and is valid only for the duration of the call. */
typedef uint32_t SrhAudioFormat;
enum { SRH_AUDIO_S16_STEREO = 1 };
typedef SrhStatus(SRH_CALL *SrhAudioRender)(void *, uint64_t, uint32_t, int16_t *);
typedef SrhStatus(SRH_CALL *SrhAudioRegister)(void *, SrhHandle, uint32_t, uint32_t,
                                               SrhAudioFormat, const char *, SrhAudioRender,
                                               void *, SrhHandle *);
typedef struct SrhHostAudioV1 {
    SRH_HEADER;
    void *context;
    uint32_t sample_rate;
    uint32_t channels;
    SrhAudioFormat format;
    SrhAudioRegister register_source;
} SrhHostAudioV1;

/* Optional query: "host.audio_input.v1". Existing ABI tables are unchanged.
   Simulation-thread only. request(owner, rate) starts a subscription at 8000..384000 Hz; request(owner,
   0) stops and discards it. The owner must be alive; removal stops capture.
   Repeating a request with the same rate preserves data; a new rate clears it.
   Resampling is strictly nearest-exact: source frame floor((n + 0.5) *
   source_rate / requested_rate), continuous across blocks, without filtering.
   SDL preserves native device rate; its conversion is only format/channels.
   read is nonblocking, returns interleaved float PCM in [-1, 1], and writes the
   current rate/channels even when no frames are available. capacity is in FLOAT
   SAMPLES, not frames; return count is FRAMES. Always use the returned format:
   user device/channel/rate changes discard queued data. Each owner has its own
   bounded queue (oldest frames drop on overflow). Disabled/unavailable input
   returns zero frames. Capture follows wall time, not emulated time; it is not
   saved in snapshots. Check query for NULL on older hosts. */
typedef struct SrhHostAudioInputV1 {
    SRH_HEADER;
    void *context;
    SrhStatus (SRH_CALL *request)(void *, SrhHandle owner, uint32_t sample_rate);
    uint32_t (SRH_CALL *read)(void *, SrhHandle owner, float *samples,
                            uint32_t capacity, uint32_t *rate, uint32_t *channels);
} SrhHostAudioInputV1;

/* Configuration entries exposed by trusted native plugins/tools in the host
   Settings window.  The host copies metadata at registration time and owns the
   canonical key=value store.  get/set use UTF-8 text values; the UI interprets
   them according to `type`.  Strings are borrowed only for the duration of each
   call.  `owner` lets the host prune entries when a card is physically removed;
   global entries (including tool entries) use 0. */
enum {
    Srh_CONFIG_BOOL = 0,
    Srh_CONFIG_INT = 1,
    Srh_CONFIG_FLOAT = 2,
    Srh_CONFIG_STRING = 3,
    Srh_CONFIG_ENUM = 4,
    Srh_CONFIG_PATH = 5
};
typedef SrhStatus(SRH_CALL *SrhConfigGet)(void *, char *, uint32_t);
typedef SrhStatus(SRH_CALL *SrhConfigSet)(void *, const char *);
typedef struct SrhConfigEntry {
    SRH_HEADER;
    const char *category;     /* UI category, e.g. "General" */
    const char *name;         /* canonical config key */
    const char *label;        /* UI label */
    const char *description;  /* optional tooltip; may be NULL */
    uint32_t type;            /* SrhConfigType */
    const char *enum_labels;  /* "A|B|C" for Srh_CONFIG_ENUM; may be NULL */
    const char *default_value;/* optional initial value; may be NULL */
    void *context;            /* callback context, usually the plugin/tool instance */
    SrhHandle owner;          /* card handle for plugin entries; 0 for global/tools */
    SrhConfigGet get;
    SrhConfigSet set;
} SrhConfigEntry;
typedef SrhStatus(SRH_CALL *SrhConfigRegister)(void *, const SrhConfigEntry *);
typedef SrhStatus(SRH_CALL *SrhConfigUnregister)(void *, void *);
typedef SrhStatus(SRH_CALL *SrhConfigGetValue)(void *, const char *, char *, uint32_t);
typedef SrhStatus(SRH_CALL *SrhConfigSetValue)(void *, const char *, const char *);
typedef struct SrhHostConfigV1 {
    SRH_HEADER;
    void *context;
    SrhConfigRegister register_entry;
    SrhConfigUnregister unregister_context;
    SrhConfigGetValue get_value;
    SrhConfigSetValue set_value;
} SrhHostConfigV1;

/* Optional metadata used by hosts that offer an "add card" workflow.  The
   descriptor contains insertion defaults only; it does not create a card or
   claim any host resources.  Strings are owned by the plugin and remain valid
   while its library is loaded. */
typedef uint32_t SrhCardFlags;
enum {
    SRH_CARD_REQUIRES_IMAGE = 1u,
    SRH_CARD_REQUIRES_IO_SPACE = 2u,
    SRH_CARD_SHOW_CLOCK = 4u
};
typedef struct SrhImageSlotDescriptor {
    SRH_HEADER;
    const char *label;
} SrhImageSlotDescriptor;
typedef struct SrhCardDescriptor {
    SRH_HEADER;
    const char *category;
    const char *name;
    const char *description;
    uint64_t default_base;
    uint64_t default_size;
    uint64_t default_reset_vector;
    int32_t default_priority;
    uint32_t default_clock;
    SrhCardFlags flags;
    const char *default_config_json;
    const char *io_space_config_key;
    /* Optional key in the plugin config object which mirrors the generic
       card base field. This keeps the host independent of plugin JSON names. */
    const char *base_config_key;
    /* Optional ordered file-picker presentation metadata. The host copies the
       labels during discovery and supplies one SrhImagePart per slot during
       create(); all image validation and interpretation belong to the plugin. */
    const SrhImageSlotDescriptor *image_slots;
    uint32_t image_slot_count;
} SrhCardDescriptor;

typedef struct ShouryoHost {
    SRH_HEADER;
    void *context;
    SrhStatus(SRH_CALL *log)(void *, SrhHandle, const char *);
    SrhStatus(SRH_CALL *map)(void *, SrhHandle, const SrhMapping *, SrhHandle *);
    SrhStatus(SRH_CALL *unmap)(void *, SrhHandle);
    SrhStatus(SRH_CALL *read)(void *, SrhHandle, SrhHandle, uint64_t, uint8_t *);
    SrhStatus(SRH_CALL *write)(void *, SrhHandle, SrhHandle, uint64_t, uint8_t);
    SrhStatus(SRH_CALL *subscribe_clock)(void *, SrhHandle, uint32_t, SrhCallback, void *, SrhHandle *);
    SrhStatus(SRH_CALL *schedule)(void *, SrhHandle, uint64_t, SrhCallback, void *, SrhHandle *);
    SrhStatus(SRH_CALL *cancel)(void *, SrhHandle);
    SrhStatus(SRH_CALL *boundary)(void *, SrhHandle, SrhHandle, uint64_t);
    SrhStatus(SRH_CALL *remove)(void *, SrhHandle);
    SrhStatus(SRH_CALL *alive)(void *, SrhHandle);
    SrhStatus(SRH_CALL *signal_find)(void *, const char *, SrhHandle *);
    SrhStatus(SRH_CALL *signal_drive)(void *, SrhHandle, SrhHandle, int32_t, int32_t);
    SrhStatus(SRH_CALL *signal_subscribe)(void *, SrhHandle, SrhHandle, SrhSignalCallback, void *,
                                          SrhHandle *);
    SrhStatus(SRH_CALL *signal_read)(void *, SrhHandle, int32_t *);
    SrhStatus(SRH_CALL *time_ns)(void *, uint64_t *);
    SrhStatus(SRH_CALL *random_u64)(void *, uint64_t *);
    /* Appended in SRH_ABI 1 (tail field, guarded by struct_size). */
    SrhQueryExtension query;
} ShouryoHost;
typedef struct SrhPlugin {
    SRH_HEADER;
    const char *id;
    SrhStatus(SRH_CALL *create)(const ShouryoHost *, SrhHandle, const SrhConfig *, void **);
    void(SRH_CALL *destroy)(void *);
    SrhStatus(SRH_CALL *reset)(void *, uint32_t cold);
    uint32_t(SRH_CALL *property_count)(void *);
    SrhStatus(SRH_CALL *property_info)(void *, uint32_t, SrhProperty *);
    SrhStatus(SRH_CALL *property_get)(void *, uint32_t, SrhValue *);
    SrhStatus(SRH_CALL *property_set)(void *, uint32_t, const SrhValue *);
    /* Appended in SRH_ABI 1 (tail fields, guarded by struct_size). */
    SrhSaveState save_state;
    SrhLoadState load_state;
    /* Optional add-card metadata, appended in SRH_ABI 1. */
    const SrhCardDescriptor *card_descriptor;
    /* Optional opaque project chunk, separate from transient execution state.
       Save supports a NULL buffer size query; sizes count bytes, no NUL needed.
       Load runs after create and before the project's initial reset. Plugins
       own versioning/validation and must reject invalid data without mutation. */
    SrhSaveState save_project_data;
    SrhLoadState load_project_data;
} SrhPlugin;
typedef const SrhPlugin *(SRH_CALL *SrhPluginInit)(const ShouryoHost *);
SRH_EXPORT const SrhPlugin *SRH_CALL srz80_plugin_init(const ShouryoHost *host);
#ifdef __cplusplus
}
#endif
#endif
