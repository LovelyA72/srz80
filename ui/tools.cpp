#include "gui.hpp"
#include <SDL3/SDL_dialog.h>
#include <algorithm>
#include <boundary.hpp>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <stdexcept>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace srz80::ui {
namespace {

// Callback owns only shared result storage, never App or a plugin instance.
// Releasing a request or destroying App while a dialog is open is safe.
void SDLCALL tool_file_callback(void *userdata, const char *const *files, int) noexcept {
    std::unique_ptr<std::shared_ptr<ToolDialogState>> owner(
        static_cast<std::shared_ptr<ToolDialogState> *>(userdata));
    auto &state = **owner;
    std::lock_guard lock(state.mutex);
    try {
        state.error = files == nullptr;
        state.cancelled = files && !files[0];
        if (files && files[0]) state.path = files[0];
    } catch (...) { state.error = true; }
    state.pending = false;
}

std::shared_ptr<void> open_tool_library(const std::filesystem::path &path) {
#ifdef _WIN32
    auto module = LoadLibraryW(path.c_str());
    if (!module)
        throw std::runtime_error("Cannot load tool " + path.string() + " (Windows error " +
                                 std::to_string(GetLastError()) + ")");
    return {module, [](void *handle) { FreeLibrary(static_cast<HMODULE>(handle)); }};
#else
    auto module = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!module)
        throw std::runtime_error("Cannot load tool " + path.string() + ": " + dlerror());
    return {module, [](void *handle) { dlclose(handle); }};
#endif
}

std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

std::string path_to_utf8(const std::filesystem::path &path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

SrhToolInit tool_entry(const std::shared_ptr<void> &library) {
#ifdef _WIN32
    auto address = GetProcAddress(static_cast<HMODULE>(library.get()), "srz80_tool_init");
    SrhToolInit result = nullptr;
    static_assert(sizeof(result) == sizeof(address));
    std::memcpy(&result, &address, sizeof(result));
    return result;
#else
    return reinterpret_cast<SrhToolInit>(dlsym(library.get(), "srz80_tool_init"));
#endif
}

SrhStatus copy_config_text(const std::string &text, char *value, uint32_t capacity) {
    if (!value || !capacity)
        return SRH_INVALID;
    const auto amount = std::min<size_t>(text.size(), capacity - 1);
    std::memcpy(value, text.data(), amount);
    value[amount] = 0;
    return SRH_OK;
}

} // namespace

SrhStatus App::request_tool_file_dialog(uint32_t save, const SrhToolFileFilter *descriptors,
                                         uint32_t descriptor_count, SrhHandle *request) {
    if (request)
        *request = 0;
    return sdk::guard([&]() -> SrhStatus {
        if (!request || save > 1 || !descriptor_count || descriptor_count > 16 || !descriptors)
            return SRH_INVALID;
        if (!next_tool_dialog || tool_dialogs.size() >= 16)
            return SRH_UNAVAILABLE;

        auto state = std::make_shared<ToolDialogState>();
        state->filters.reserve(descriptor_count);
        for (uint32_t index = 0; index < descriptor_count; ++index) {
            const auto &descriptor = descriptors[index];
            if (!descriptor.name || !*descriptor.name || !descriptor.patterns ||
                !*descriptor.patterns)
                return SRH_INVALID;
            state->filters.emplace_back(descriptor.name, descriptor.patterns);
        }
        state->native_filters.reserve(state->filters.size());
        for (const auto &[name, patterns] : state->filters)
            state->native_filters.push_back({name.c_str(), patterns.c_str()});

        auto callback_owner = std::make_unique<std::shared_ptr<ToolDialogState>>(state);
        auto id = next_tool_dialog++;
        tool_dialogs.emplace(id, state);
        if (save)
            SDL_ShowSaveFileDialog(tool_file_callback, callback_owner.release(), window,
                                   state->native_filters.data(),
                                   static_cast<int>(state->native_filters.size()), nullptr);
        else
            SDL_ShowOpenFileDialog(tool_file_callback, callback_owner.release(), window,
                                   state->native_filters.data(),
                                   static_cast<int>(state->native_filters.size()), nullptr, false);
        *request = id;
        return SRH_OK;
    });
}

void App::load_tools() {
    ImGuiMemAllocFunc allocator = nullptr;
    ImGuiMemFreeFunc deallocator = nullptr;
    void *allocator_context = nullptr;
    ImGui::GetAllocatorFunctions(&allocator, &deallocator, &allocator_context);
    tool_host = {
        SRH_INIT(SrhToolHostV1),
        this,
        ImGui::GetVersion(),
        ImGui::GetCurrentContext(),
        allocator,
        deallocator,
        allocator_context,
        [](void *context, const char *message) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !message)
                    return SRH_INVALID;
                static_cast<App *>(context)->controller.log_message("[tool] " + std::string(message));
                return SRH_OK;
            });
        },
        [](void *context) -> uint32_t {
            if (!context)
                return 0;
            auto *app = static_cast<App *>(context);
            if (!app->snapshot)
                return 0;
            const auto size = app->snapshot->inspection->spaces.size();
            return size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
        },
        [](void *context, uint32_t index, SrhToolSpace *result) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !sdk::valid(result))
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!app->snapshot)
                    return SRH_NOT_FOUND;
                const auto &spaces = app->snapshot->inspection->spaces;
                if (index >= spaces.size())
                    return SRH_NOT_FOUND;
                auto entry = spaces.begin();
                std::advance(entry, index);
                *result = {SRH_INIT(SrhToolSpace), entry->first, entry->second.maximum, {}};
                std::snprintf(result->name, sizeof(result->name), "%s", entry->second.name.c_str());
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle space, uint64_t address, uint8_t *value) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !value)
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                auto cells = app->controller.request_memory(space, address, 1, app->snapshot->generation);
                if (cells.size() != 1 || !cells[0].available())
                    return cells.empty() ? SRH_NOT_FOUND : cells[0].status;
                *value = cells[0].value;
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle space, uint64_t address, const uint8_t *bytes, uint64_t size,
           uint32_t reset_before_load, uint64_t *written) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (written) *written = 0;
                if (!context || !written || (!bytes && size) || size > SIZE_MAX || reset_before_load > 1)
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!app->snapshot)
                    return SRH_NOT_FOUND;
                if (!size)
                    return SRH_OK;
                SimulationController::MemorySegment segment{address,
                    std::vector<uint8_t>(bytes, bytes + static_cast<size_t>(size))};
                auto result = app->controller.load_memory(space, {std::move(segment)},
                    reset_before_load != 0, app->snapshot->generation);
                *written = result.written;
                return result.status;
            });
        },
        [](void *context) -> uint32_t {
            if (!context)
                return SRT_PAUSED;
            auto *app = static_cast<App *>(context);
            if (!app->snapshot)
                return SRT_PAUSED;
            switch (app->snapshot->run_state) {
            case UiRunState::stopped:
                return SRT_STOPPED;
            case UiRunState::paused:
                return SRT_PAUSED;
            case UiRunState::running:
                return SRT_RUNNING;
            }
            return SRT_PAUSED;
        },
        [](void *context) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context)
                    return SRH_INVALID;
                static_cast<App *>(context)->request_resume();
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle space, const SrhToolMemorySegment *segments, uint32_t count,
           uint32_t reset, uint32_t *failed, uint64_t *written) -> SrhStatus {
            if (failed)
                *failed = UINT32_MAX;
            if (written)
                *written = 0;
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !failed || !written || !segments || !count ||
                    count > SIZE_MAX / sizeof(*segments) || reset > 1)
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!app->snapshot)
                    return SRH_NOT_FOUND;
                std::vector<SimulationController::MemorySegment> copy;
                copy.reserve(count);
                for (uint32_t i = 0; i < count; ++i) {
                    const auto &segment = segments[i];
                    if (!sdk::valid(&segment) || !segment.bytes || !segment.size || segment.size > SIZE_MAX) {
                        *failed = i;
                        return SRH_INVALID;
                    }
                    copy.push_back({segment.address, std::vector<uint8_t>(segment.bytes,
                        segment.bytes + static_cast<size_t>(segment.size))});
                }
                auto result = app->controller.load_memory(space, std::move(copy), reset != 0,
                                                           app->snapshot->generation);
                *failed = result.failed_segment;
                *written = result.written;
                return result.status;
            });
        },
        [](void *context, uint32_t save, const SrhToolFileFilter *filters, uint32_t filter_count,
           SrhHandle *request) -> SrhStatus {
            if (!context)
                return SRH_INVALID;
            return static_cast<App *>(context)->request_tool_file_dialog(save, filters,
                                                                          filter_count, request);
        },
        [](void *context, SrhHandle request, SrhToolFileResult *result, char *path,
           uint64_t capacity) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !sdk::valid(result) || (!path && capacity)) return SRH_INVALID;
                *result = {SRH_INIT(SrhToolFileResult), 0, 0, 0};
                auto &dialogs = static_cast<App *>(context)->tool_dialogs;
                auto found = dialogs.find(request);
                if (found == dialogs.end()) return SRH_NOT_FOUND;
                auto &state = *found->second;
                std::lock_guard lock(state.mutex);
                result->pending = state.pending;
                result->cancelled = state.cancelled;
                if (state.pending) return SRH_OK;
                if (state.error) return SRH_ERROR;
                if (state.cancelled) return SRH_OK;
                result->required_size = state.path.size() + 1;
                if (!path && !capacity) return SRH_OK;
                if (capacity < result->required_size) return SRH_INVALID;
                std::memcpy(path, state.path.c_str(), static_cast<size_t>(result->required_size));
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle request) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context) return SRH_INVALID;
                return static_cast<App *>(context)->tool_dialogs.erase(request) ? SRH_OK : SRH_NOT_FOUND;
            });
        },
        [](void *context, const SrhConfigEntry *entry) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !sdk::valid(entry))
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!entry->name || !*entry->name || !entry->label || !entry->get || !entry->set ||
                    entry->type > Srh_CONFIG_PATH)
                    return SRH_INVALID;
                UiConfigRegistration stored;
                stored.category = entry->category && *entry->category ? entry->category : "Tools";
                stored.name = entry->name;
                stored.label = entry->label;
                stored.description = entry->description ? entry->description : "";
                stored.type = entry->type;
                stored.enum_labels = entry->enum_labels ? entry->enum_labels : "";
                stored.default_value = entry->default_value ? entry->default_value : "";
                stored.context = entry->context;
                stored.owner = entry->owner;
                stored.get = entry->get;
                stored.set = entry->set;
                auto existing = std::find_if(app->tool_config_entries.begin(),
                                             app->tool_config_entries.end(),
                                             [&](const auto &e) { return e.name == stored.name; });
                if (existing == app->tool_config_entries.end()) {
                    app->tool_config_entries.push_back(std::move(stored));
                    if (app->controller.config_get(entry->name).empty() && entry->default_value &&
                        *entry->default_value)
                        app->controller.config_set(entry->name, entry->default_value);
                }
                return SRH_OK;
            });
        },
        [](void *context, void *entry_context) -> SrhStatus {
            if (!context)
                return SRH_INVALID;
            auto *app = static_cast<App *>(context);
            std::erase_if(app->tool_config_entries,
                          [&](const UiConfigRegistration &e) { return e.context == entry_context; });
            return SRH_OK;
        },
        [](void *context, const char *key, char *value, uint32_t capacity) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !key || !value || !capacity)
                    return SRH_INVALID;
                return copy_config_text(static_cast<App *>(context)->controller.config_get(key),
                                        value, capacity);
            });
        },
        [](void *context, const char *key, const char *value) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !key || !value)
                    return SRH_INVALID;
                // This ABI reports transport acceptance, not the worker result.
                // Queue owned strings so preference changes cannot stall draw.
                static_cast<App *>(context)->controller.post_config_set(key, value);
                return SRH_OK;
            });
        },
        [](void *context) -> uint32_t {
            if (!context)
                return 0;
            auto *app = static_cast<App *>(context);
            if (!app->snapshot)
                return 0;
            const auto size = app->snapshot->inspection->cards.size();
            return size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
        },
        [](void *context, uint32_t index, SrhToolCard *result) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !srz80::sdk::valid(result))
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!app->snapshot || index >= app->snapshot->inspection->cards.size())
                    return SRH_NOT_FOUND;
                const auto &card = app->snapshot->inspection->cards[index];
                *result = {SRH_INIT(SrhToolCard), card.id, card.active ? 1u : 0u, {}, {}};
                std::snprintf(result->type, sizeof(result->type), "%s", card.type.c_str());
                std::snprintf(result->name, sizeof(result->name), "%s #%llu", card.display_name().c_str(),
                              static_cast<unsigned long long>(card.id));
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle card) -> uint32_t {
            if (!context)
                return 0;
            auto *app = static_cast<App *>(context);
            if (!app->snapshot || !app->snapshot->inspection->properties.contains(card))
                return 0;
            const auto size = app->snapshot->inspection->properties.at(card).size();
            return size > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(size);
        },
        [](void *context, SrhHandle card, uint32_t index, SrhToolProperty *result) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !srz80::sdk::valid(result))
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!app->snapshot || !app->snapshot->inspection->properties.contains(card))
                    return SRH_NOT_FOUND;
                const auto &properties = app->snapshot->inspection->properties.at(card);
                if (index >= properties.size())
                    return SRH_NOT_FOUND;
                const auto &property = properties[index];
                *result = {SRH_INIT(SrhToolProperty), {}, {}, {}, {}, property.kind, property.bits,
                           property.base, property.editable ? 1u : 0u, property.ui_flags};
                copy_config_text(property.name, result->name, sizeof(result->name));
                copy_config_text(property.group, result->group, sizeof(result->group));
                copy_config_text(property.description, result->description,
                                 sizeof(result->description));
                copy_config_text(property.enum_labels, result->enum_labels,
                                 sizeof(result->enum_labels));
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle card, uint32_t index, SrhValue *value) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !srz80::sdk::valid(value))
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!app->snapshot || !app->snapshot->inspection->properties.contains(card))
                    return SRH_NOT_FOUND;
                const auto &properties = app->snapshot->inspection->properties.at(card);
                if (index >= properties.size())
                    return SRH_NOT_FOUND;
                std::memcpy(value, &properties[index].value, sizeof(SrhValue));
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle card, uint32_t index, const SrhValue *value) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !srz80::sdk::valid(value))
                    return SRH_INVALID;
                auto *app = static_cast<App *>(context);
                if (!app->snapshot || !app->snapshot->inspection->properties.contains(card))
                    return SRH_NOT_FOUND;
                const auto &properties = app->snapshot->inspection->properties.at(card);
                if (index >= properties.size())
                    return SRH_NOT_FOUND;
                std::string edit_error;
                return app->controller.request_edit(card, index, *value, edit_error, app->snapshot->generation);
            });
        },
        [](void *context, char *buffer, uint64_t *size) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !size)
                    return SRH_INVALID;
                auto &app = *static_cast<App *>(context);
                app.controller.provider_interest();
                const auto snapshot = app.controller.snapshot();
                if (app.tool_provider_generation != snapshot->generation ||
                    app.tool_provider_sequence != snapshot->provider_sequence) {
                    nlohmann::json providers = nlohmann::json::array();
                    for (const auto &[owner, provider] : *snapshot->providers) {
                        std::string display_name = provider.name;
                        for (const auto &card : snapshot->inspection->cards)
                            if (card.id == owner && !card.name.empty()) {
                                display_name = card.name;
                                break;
                            }
                        providers.push_back({{"owner", owner},
                                             {"name", provider.name},
                                             {"flags", provider.flags},
                                             {"display_name", display_name},
                                             {"protocol", provider.protocol},
                                             {"data", provider.data}});
                    }
                    app.tool_provider_data = nlohmann::json(
                        {{"generation", snapshot->generation}, {"providers", providers}}).dump();
                    app.tool_provider_generation = snapshot->generation;
                    app.tool_provider_sequence = snapshot->provider_sequence;
                }
                const auto &text = app.tool_provider_data;
                const auto capacity = *size;
                *size = text.size() + 1;
                if (*size > SRH_PROVIDER_MAX_BYTES)
                    return SRH_UNAVAILABLE;
                if (!buffer)
                    return SRH_OK;
                if (capacity < *size)
                    return SRH_INVALID;
                std::memcpy(buffer, text.c_str(), static_cast<size_t>(*size));
                return SRH_OK;
            });
        },
        [](void *context, SrhHandle owner, uint64_t generation, uint32_t kind, uint64_t revision,
           const char *payload, uint64_t size) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !payload || !size || size >= SRH_PROVIDER_MAX_BYTES)
                    return SRH_INVALID;
                auto &app = *static_cast<App *>(context);
                if (kind == 1 && app.project_session.busy()) return SRH_CONFLICT;
                auto status = app.controller.provider_command(owner, generation, kind, revision,
                                                           std::string(payload, static_cast<size_t>(size)));
                if (status == SRH_OK && kind == 1) {
                    app.project_session.note_external_edit();

                }
                return status;
            });
        },
        [](void *context, char *path, uint64_t *size) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !size)
                    return SRH_INVALID;
                const auto &app = *static_cast<App *>(context);
                const std::filesystem::path root = !app.project_session.path().empty()
                                                       ? app.project_session.path().parent_path()
                                                       : std::filesystem::path{};
                const auto text = root.string();
                const auto capacity = *size;
                *size = text.size() + 1;
                if (!path)
                    return SRH_OK;
                if (capacity < *size)
                    return SRH_INVALID;
                std::memcpy(path, text.c_str(), static_cast<size_t>(*size));
                return SRH_OK;
            });
        },
        [](void *context, const SrhToolTextFormat *handler) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !sdk::valid(handler) || !handler->plugin_id || !*handler->plugin_id ||
                    !handler->extension || !*handler->extension || !handler->label || !handler->open)
                    return SRH_INVALID;
                std::string extension = handler->extension;
                if (extension.front() == '.')
                    extension.erase(extension.begin());
                if (extension.empty() || extension.find_first_not_of(
                                             "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_+-") !=
                                             std::string::npos)
                    return SRH_INVALID;
                std::transform(extension.begin(), extension.end(), extension.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                auto *app = static_cast<App *>(context);
                const auto duplicate = std::find_if(app->project_text_formats.begin(),
                                                    app->project_text_formats.end(),
                    [&](const auto &entry) {
                        return entry->plugin_id == handler->plugin_id && entry->extension == extension;
                    });
                if (duplicate != app->project_text_formats.end())
                    return SRH_INVALID;
                auto entry = std::make_shared<ProjectTextFormat>();
                entry->plugin_id = handler->plugin_id;
                entry->extension = std::move(extension);
                entry->label = handler->label;
                entry->context = handler->handler_context;
                entry->open = handler->open;
                entry->binary = sdk::has_field(handler, &SrhToolTextFormat::flags) &&
                                (handler->flags & SRH_TEXT_FORMAT_BINARY) != 0;
                entry->enabled = app->controller.config_get(
                    "plugin.text_format." + entry->plugin_id + "." + entry->extension, "1") != "0";
                app->project_text_formats.push_back(std::move(entry));
                return SRH_OK;
            });
        },
        [](void *context, void *handler_context) -> SrhStatus {
            if (!context)
                return SRH_INVALID;
            auto &handlers = static_cast<App *>(context)->project_text_formats;
            std::erase_if(handlers, [&](const auto &entry) { return entry->context == handler_context; });
            return SRH_OK;
        },
        [](void *context, void *handler_context, const char *path, const char *contents,
           uint64_t size, uint64_t cursor) -> SrhStatus {
            return sdk::guard([&]() -> SrhStatus {
                if (!context || !handler_context || !path || !contents || size >= 65536)
                    return SRH_INVALID;
                static_cast<App *>(context)->update_project_document(
                    handler_context, path, std::string_view(contents, static_cast<size_t>(size)), cursor);
                return SRH_OK;
            });
        },
        [](void *context, void *client, uint64_t generation, uint64_t identity, SrhHandle owner, const char *endpoint,
           uint64_t time, const uint8_t *bytes, uint64_t size, SrhHandle *out) -> SrhStatus {
            return sdk::guard([&] {
                auto &app = *static_cast<App *>(context);
                return app.tool_inputs.submit(app.controller, client, generation, identity, owner, endpoint, time, bytes, size, out);
            });
        },
        [](void *context, void *client, SrhHandle request, SrhToolInputResult *out) -> SrhStatus {
            return sdk::guard([&] { return static_cast<App *>(context)->tool_inputs.poll(client, request, out); });
        },
        [](void *context, void *client, SrhHandle request) -> SrhStatus {
            return sdk::guard([&] { return static_cast<App *>(context)->tool_inputs.release(client, request); });
        },
        [](void *context, void *client, uint64_t generation, uint64_t identity, SrhHandle *out) -> SrhStatus {
            return sdk::guard([&] {
                auto &app = *static_cast<App *>(context);
                return app.tool_inputs.cancel(app.controller, client, generation, identity, out);
            });
        },
        [](void *context, SrhToolRuntime *out) -> SrhStatus {
            if (!sdk::valid(out)) return SRH_INVALID;
            auto snapshot = static_cast<App *>(context)->controller.snapshot();
            out->generation = snapshot->generation; out->time_ns = snapshot->now;
            out->stopped = snapshot->stopped(); out->running = snapshot->run_state == UiRunState::running;
            return SRH_OK;
        }};

    const auto directory = base / "tools";
    if (!std::filesystem::exists(directory))
        return;
    std::vector<std::filesystem::path> paths;
    for (const auto &entry : std::filesystem::directory_iterator(directory)) {
#ifdef _WIN32
        constexpr const char *extension = ".dll";
#else
        constexpr const char *extension = ".so";
#endif
        if (entry.is_regular_file() && entry.path().extension() == extension)
            paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    for (const auto &path : paths) {
        try {
            auto library = open_tool_library(path);
            auto init = tool_entry(library);
            if (!init)
                throw std::runtime_error("Tool has no srz80_tool_init entry point");
            auto api = init(&tool_host);
            if (!sdk::valid(api) || !api->id || !api->name || !api->imgui_version ||
                !api->create || !api->destroy || !api->draw)
                throw std::runtime_error("Tool ABI is incomplete or incompatible");
            if (std::strcmp(api->imgui_version, ImGui::GetVersion()) != 0)
                throw std::runtime_error("Tool Dear ImGui version does not match the host");
            if (std::any_of(tools.begin(), tools.end(), [&](const auto &tool) {
                    return std::strcmp(tool.api->id, api->id) == 0;
                }))
                throw std::runtime_error("Duplicate tool id: " + std::string(api->id));
            void *instance = nullptr;
            auto status = api->create(&tool_host, &instance);
            if (status != SRH_OK || !instance)
                throw std::runtime_error("Tool creation failed with status " +
                                         std::to_string(status));
            const bool open =
                controller.config_get("tool." + std::string(api->id) + ".open", "0") != "0";
            std::string category = "Misc";
            if (sdk::has_field(api, &SrhToolPlugin::category) && api->category && *api->category)
                category = api->category;
            tools.push_back({std::move(library), api, instance, std::move(category), open});
            controller.log_message("[tool] Loaded " + std::string(api->name));
        } catch (const std::exception &exception) {
            controller.log_message("[tool] " + path.filename().string() + ": " + exception.what());
        }
    }
    restore_tool_project_state();
}

void App::tick_tools(bool visible) {
    tool_inputs.collect();
    for (auto &tool : tools) {
        if (tool.background_failed || !sdk::has_field(tool.api, &SrhToolPlugin::background_tick) ||
            !tool.api->background_tick) continue;
        auto status = sdk::guard([&] { return tool.api->background_tick(tool.instance, visible && tool.open); });
        if (status != SRH_OK) {
            tool.background_failed = true;
            controller.log_message("[tool] " + std::string(tool.api->name) + " background routing disabled after callback failure");
        }
    }
}
void App::draw_tools() {
    for (auto &tool : tools) {
        if (!tool.open)
            continue;
        if (tool.api && tool.api->id && pending_tool_focus == tool.api->id) {
            ImGui::SetNextWindowFocus();
            pending_tool_focus.clear();
        }
        uint32_t open = 1;
        auto status = tool.api->draw(tool.instance, &open);
        tool.open = open != 0;
        if (status != SRH_OK) {
            controller.log_message("[tool] " + std::string(tool.api->name) +
                                   " draw failed (status " + std::to_string(status) + ")");
            tool.open = false;
        }
    }
    sync_tool_project_state();
}

void App::save_tool_project_files() {
    for (const auto &tool : tools) {
        if (!tool.api || !tool.instance || !sdk::has_field(tool.api, &SrhToolPlugin::project_save) ||
            !tool.api->project_save)
            continue;
        const auto status = tool.api->project_save(tool.instance);
        if (status != SRH_OK)
            throw std::runtime_error(std::string(tool.api->name) + " could not save its project files");
    }
}

void App::restore_tool_project_state() {
    const auto states = project_session.document().value("tool_state", nlohmann::json::object());
    for (const auto &tool : tools) {
        if (!tool.api || !tool.instance || !tool.api->id ||
            !sdk::has_field(tool.api, &SrhToolPlugin::project_state_load) ||
            !tool.api->project_state_load)
            continue;
        std::string value;
        if (states.is_object()) {
            const auto found = states.find(tool.api->id);
            if (found != states.end() && found->is_string())
                value = found->get<std::string>();
        }
        const bool text_state = sdk::has_field(tool.api, &SrhToolPlugin::flags) &&
                                (tool.api->flags & Srh_TOOL_PROJECT_STATE_TEXT);
        if (!text_state && !value.empty()) {
            auto source = path_from_utf8(value);
            if (source.is_relative() && !project_session.path().empty())
                source = (project_session.path().parent_path() / source).lexically_normal();
            open_project_file(source);
            continue;
        }
        if (tool.api->project_state_load(tool.instance, value.c_str()) != SRH_OK)
            controller.log_message("[tool] " + std::string(tool.api->name) +
                                   " could not restore its project state");
    }
}

void App::sync_tool_project_state(bool strict) {
    if (!project_session.document().is_object())
        return;
    nlohmann::json states = project_session.document().value("tool_state", nlohmann::json::object());
    if (!states.is_object())
        states = nlohmann::json::object();
    const auto state_path = project_session.path();
    std::vector<std::string> path_states;
    for (const auto &tool : tools) {
        if (!tool.api || !tool.instance || !tool.api->id ||
            !sdk::has_field(tool.api, &SrhToolPlugin::project_state_get) ||
            !tool.api->project_state_get)
            continue;
        uint64_t required = 0;
        auto status = tool.api->project_state_get(tool.instance, nullptr, &required);
        if (status != SRH_OK || !required || required > 64 * 1024) {
            if (strict) throw std::runtime_error(std::string(tool.api->name) + " returned invalid project state");
            controller.log_message("[tool] " + std::string(tool.api->name) +
                                   " returned invalid project state");
            continue;
        }
        std::string value;
        if (required) {
            std::vector<char> buffer(static_cast<size_t>(required));
            uint64_t capacity = required;
            status = tool.api->project_state_get(tool.instance, buffer.data(), &capacity);
            if (status != SRH_OK || !capacity || capacity > required || buffer.back() != '\0') {
                if (strict) throw std::runtime_error(std::string(tool.api->name) + " returned invalid project state");
                controller.log_message("[tool] " + std::string(tool.api->name) +
                                       " returned invalid project state");
                continue;
            }
            value.assign(buffer.data());
        }
        const bool text_state = sdk::has_field(tool.api, &SrhToolPlugin::flags) &&
                                (tool.api->flags & Srh_TOOL_PROJECT_STATE_TEXT);
        if (!text_state) path_states.emplace_back(tool.api->id);
        if (!text_state && !value.empty() && !state_path.empty()) {
            const auto source = path_from_utf8(value);
            if (source.is_absolute()) {
                std::error_code error;
                const auto relative = std::filesystem::relative(source, state_path.parent_path(), error);
                if (!error)
                    value = path_to_utf8(relative.lexically_normal());
            }
        }
        const auto found = states.find(tool.api->id);
        if (value.empty()) {
            if (found != states.end()) {
                states.erase(found);
            }
        } else if (found == states.end() || !found->is_string() || found->get<std::string>() != value) {
            states[tool.api->id] = value;
        }
    }
    project_session.collect_tool_state(std::move(states), std::move(path_states));
}

} // namespace srz80::ui
