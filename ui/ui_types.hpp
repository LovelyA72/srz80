#pragma once
#include <srz80/abi.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace srz80 {
// Cards, spaces, clocks and every other rack object are referred to by their
// opaque 64-bit identity.  This is the only rack-facing value the UI shares
// with the engine, and it is a plain C ABI integer.
using Handle = SrhHandle;
} // namespace srz80

namespace srz80::ui {

// UI-owned value types.
//
// These mirror the data the simulation thread publishes, but they belong to
// the UI: they are copied out of the engine ABI on the simulation worker and
// then only read.  No engine type, engine pointer or card pointer may appear
// in a panel, in a published snapshot or in a tool service structure.

enum class UiRunState { stopped, paused, running };
enum class UiTimeMode { project, fixed, system };
enum class UiResolver { priority, bit_or };

struct UiSpace {
    Handle id = 0;
    std::string name;
    uint64_t maximum = 0;
    uint8_t fallback = 0;
    bool random = false;
    UiResolver resolver = UiResolver::priority;
};

struct UiClock {
    uint32_t hz = 0;
    uint64_t ticks = 0;
    uint64_t phase = 0;
    uint64_t order = 0;
};

struct UiCardInfo {
    Handle id = 0;
    std::string type;
    std::string name;
    int32_t priority = 0;
    bool active = true;
    bool parked = false;
    std::string load_error;

    const std::string &display_name() const { return name.empty() ? type : name; }
};

struct UiTrace {
    uint64_t sequence = 0;
    uint64_t parent = 0;
    uint64_t time = 0;
    std::array<uint64_t, 3> ticks{};
    Handle master = 0;
    Handle space = 0;
    uint64_t address = 0;
    uint32_t operation = 0;
    uint32_t depth = 0;
    uint32_t kind = 0;
    uint64_t instruction = 0; // instruction boundary sequence for the master
    uint8_t value = 0;
    SrhStatus result = SRH_OK;
    std::vector<Handle> responders;

    bool operator==(const UiTrace &) const = default;
};

struct UiBreakpoint {
    uint64_t id = 0;
    Handle card = 0;
    Handle space = 0;
    uint64_t first = 0;
    uint64_t last = 0;
    uint32_t operations = 0;
    bool enabled = true;
};

struct UiProperty {
    std::string name;
    std::string group;
    std::string description;
    std::string enum_labels;
    uint32_t kind = 0;
    uint32_t bits = 0;
    uint32_t base = 0;
    uint32_t ui_flags = 0;
    bool editable = false;
    SrhValue value{};
};

// Plugin-owned setting metadata.  Callback pointers stay with the plugin that
// registered them and never enter an inspection snapshot.
struct UiConfigEntry {
    std::string category;
    std::string name;
    std::string label;
    std::string description;
    std::string enum_labels;
    std::string default_value;
    std::string provider;
    uint32_t type = Srh_CONFIG_STRING;
    Handle owner = 0;
};

// A setting registered by this host or by a GUI tool, including the callbacks
// that read and write it.  These are host-process objects, created and consumed
// on the GUI thread only.
struct UiConfigRegistration {
    std::string category;
    std::string name;
    std::string label;
    std::string description;
    std::string enum_labels;
    std::string default_value;
    uint32_t type = Srh_CONFIG_STRING;
    void *context = nullptr;
    Handle owner = 0;
    SrhConfigGet get = nullptr;
    SrhConfigSet set = nullptr;
};

struct UiTextEndpoint {
    Handle card = 0;
    std::vector<std::string> inputs;
};

struct UiVideoSurface {
    Handle id = 0;
    Handle owner = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    SrhVideoFormat format = 0;
    SrhVideoFlags flags = 0;
};

struct UiAudioSourceInfo {
    Handle id = 0;
    Handle owner = 0;
    std::string name;
    uint32_t volume_percent = 100;
    int32_t pan = 0;
    bool muted = false;
    bool active = false;
    uint32_t level_peak = 0;
};

// Opaque card payload.  The host transports it; only a matching tool
// interprets its schema.
struct UiProviderData {
    std::string name;
    std::string protocol;
    std::string data;
    uint32_t flags = 0;
};

// Add-card metadata copied from a card plugin descriptor.  The simulation
// worker fills it from the engine's plugin-inspection result, so the GUI never
// loads a card library, resolves a plugin entry point or reads a descriptor.
struct UiCardType {
    std::string id;
    std::string name;
    std::string category = "Misc";
    std::string description;
    std::string config_json = "{}";
    std::string io_space_config_key;
    std::string base_config_key;
    uint64_t default_base = 0;
    uint64_t default_size = 0;
    uint64_t default_reset_vector = 0;
    int32_t default_priority = 0;
    uint32_t default_clock = 0;
    uint32_t flags = 0;
    std::vector<std::string> image_slots;
};

} // namespace srz80::ui
