#include "gui.hpp"
#include "theme.hpp"
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct WindowBounds {
    int x, y, width, height;
};

std::optional<int> config_int(srz80::ui::App &app, const char *name) {
    const auto value = app.controller.config_get(name);
    if (value.empty())
        return std::nullopt;
    try {
        size_t used = 0;
        const int result = std::stoi(value, &used);
        if (used == value.size())
            return result;
    } catch (const std::exception &) {
    }
    return std::nullopt;
}

WindowBounds fit_to_displays(WindowBounds bounds) {
    int display_count = 0;
    SDL_DisplayID *displays = SDL_GetDisplays(&display_count);
    SDL_Rect best{};
    long long best_overlap = 0;
    for (int i = 0; displays && i < display_count; ++i) {
        SDL_Rect usable{};
        if (!SDL_GetDisplayUsableBounds(displays[i], &usable))
            continue;
        const int overlap_width = std::max(0, std::min(bounds.x + bounds.width, usable.x + usable.w) -
                                                  std::max(bounds.x, usable.x));
        const int overlap_height = std::max(0, std::min(bounds.y + bounds.height, usable.y + usable.h) -
                                                   std::max(bounds.y, usable.y));
        const long long overlap = static_cast<long long>(overlap_width) * overlap_height;
        if (overlap > best_overlap) {
            best = usable;
            best_overlap = overlap;
        }
    }
    SDL_free(displays);

    if (best_overlap == 0) {
        const SDL_DisplayID primary = SDL_GetPrimaryDisplay();
        if (!primary || !SDL_GetDisplayUsableBounds(primary, &best))
            return bounds;
    }

    bounds.width = std::clamp(bounds.width, 1, best.w);
    bounds.height = std::clamp(bounds.height, 1, best.h);
    bounds.x = std::clamp(bounds.x, best.x, best.x + best.w - bounds.width);
    bounds.y = std::clamp(bounds.y, best.y, best.y + best.h - bounds.height);
    return bounds;
}

void restore_window_bounds(srz80::ui::App &app, SDL_Window *window) {
    const auto x = config_int(app, "window.x");
    const auto y = config_int(app, "window.y");
    const auto width = config_int(app, "window.width");
    const auto height = config_int(app, "window.height");
    WindowBounds requested{};
    if (x && y && width && height && *width > 0 && *height > 0) {
        requested = {*x, *y, *width, *height};
    } else if (!SDL_GetWindowPosition(window, &requested.x, &requested.y) ||
               !SDL_GetWindowSize(window, &requested.width, &requested.height)) {
        return;
    }
    const auto bounds = fit_to_displays(requested);
    SDL_SetWindowSize(window, bounds.width, bounds.height);
    SDL_SetWindowPosition(window, bounds.x, bounds.y);
}

void save_window_bounds(srz80::ui::App &app, SDL_Window *window) {
    int x = 0, y = 0, width = 0, height = 0;
    if (!SDL_GetWindowPosition(window, &x, &y) || !SDL_GetWindowSize(window, &width, &height))
        return;
    app.controller.config_set("window.x", std::to_string(x));
    app.controller.config_set("window.y", std::to_string(y));
    app.controller.config_set("window.width", std::to_string(width));
    app.controller.config_set("window.height", std::to_string(height));
}
} // namespace

int main(int argc, char **argv) {
    bool smoke = argc > 1 && std::string(argv[1]) == "--smoke";
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) {
        std::cerr << SDL_GetError() << '\n';
        return 1;
    }
    // Keep startup hidden until persisted bounds have been corrected for the
    // displays that are currently connected.
    SDL_Window *window = SDL_CreateWindow("SRZ80 - Virtual computer rack", 1440, 960,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);
    if (!window) {
        std::cerr << SDL_GetError() << '\n';
        SDL_Quit();
        return 1;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
    // Content-only controls such as the source editor draw directly into an
    // ImGui child window and handle their own mouse selection.  Do not let a
    // drag starting on that content become a request to move the whole panel.
    ImGui::GetIO().ConfigWindowsMoveFromTitleBarOnly = true;
    // The application owns ImGui persistence. The layout is loaded from the
    // base64 "imgui" value in config.ini and refreshed only on clean exit;
    // standalone .ini files are used only for explicit import/export.
    ImGui::GetIO().IniFilename = nullptr;
    srz80::ui::apply_ui_theme("Dark Red");

    SDL_Renderer *renderer = nullptr;
    bool imgui_backends_initialized = false;
    int result = 0;
    try {
        srz80::ui::App app;
        app.window = window;
        // Select the persisted audio driver before enumerating its devices.
        app.start_audio();
        app.register_app_config();
        restore_window_bounds(app, window);
        if (!smoke)
            SDL_ShowWindow(window);

        // Read the persisted render backend before creating the renderer so a
        // changed setting takes effect on the next launch.
        const bool shader_requested = !app.video_shader_path.empty();
        const bool choose_backend =
            !app.video_render_backend.empty() && app.video_render_backend != "default";
        const char *requested_backend = choose_backend
                                            ? app.video_render_backend.c_str()
                                            : (shader_requested ? SDL_GPU_RENDERER : nullptr);
        SDL_PropertiesID renderer_properties = 0;
        if (requested_backend && std::string(requested_backend) == SDL_GPU_RENDERER) {
            renderer_properties = SDL_CreateProperties();
            SDL_SetStringProperty(renderer_properties, SDL_PROP_RENDERER_CREATE_NAME_STRING,
                                  SDL_GPU_RENDERER);
            SDL_SetBooleanProperty(renderer_properties,
                                   SDL_PROP_RENDERER_CREATE_GPU_SHADERS_SPIRV_BOOLEAN, true);
            SDL_SetBooleanProperty(renderer_properties,
                                   SDL_PROP_RENDERER_CREATE_GPU_SHADERS_DXIL_BOOLEAN, true);
            SDL_SetBooleanProperty(renderer_properties,
                                   SDL_PROP_RENDERER_CREATE_GPU_SHADERS_MSL_BOOLEAN, true);
            SDL_SetPointerProperty(renderer_properties, SDL_PROP_RENDERER_CREATE_WINDOW_POINTER,
                                   window);
            renderer = SDL_CreateRendererWithProperties(renderer_properties);
            SDL_DestroyProperties(renderer_properties);
        } else {
            renderer = SDL_CreateRenderer(window, requested_backend);
        }
        if (!renderer && requested_backend) {
            const std::string first_error = SDL_GetError() ? SDL_GetError() : "unknown error";
            renderer = SDL_CreateRenderer(window, nullptr);
            if (renderer) {
                app.video_render_backend = "default";
                app.controller.config_set("video.render_backend", "default");
                app.controller.config_save();
                app.controller.log_message(
                    "[video] Requested render backend '" + std::string(requested_backend) +
                    "' was unavailable; using the default backend. " + first_error);
            }
        }
        if (!renderer)
            throw std::runtime_error(std::string("Cannot create SDL renderer: ") +
                                     (SDL_GetError() ? SDL_GetError() : "unknown error"));

        app.renderer = renderer;
        app.apply_video_settings();
        ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
        ImGui_ImplSDLRenderer3_Init(renderer);
        imgui_backends_initialized = true;

        if (smoke) {
            if (!app.open_tool("ymw258_inspector"))
                throw std::runtime_error("GUI smoke: ymw258 inspector not loaded");
        }
        bool running = true;
        int frames = 0;
        auto last_input = std::chrono::steady_clock::now();
        while (running && !app.quit_requested) {
            auto frame_start = std::chrono::steady_clock::now();
            app.host_input.update(window, app.controller, app.project_session,
                app.host_input_available() &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel));
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (!app.host_input.event(event, window, app.controller, app.project_session))
                    ImGui_ImplSDL3_ProcessEvent(&event);
                if ((event.type == SDL_EVENT_AUDIO_DEVICE_REMOVED ||
                     event.type == SDL_EVENT_AUDIO_DEVICE_FORMAT_CHANGED) && event.adevice.recording)
                    app.audio_input_device_removed(event.adevice.which);
                if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    app.request_quit();
                    if (app.quit_requested)
                        running = false;
                }
                switch (event.type) {
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                case SDL_EVENT_TEXT_EDITING:
                case SDL_EVENT_TEXT_INPUT:
                case SDL_EVENT_MOUSE_MOTION:
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                case SDL_EVENT_MOUSE_WHEEL:
                case SDL_EVENT_FINGER_DOWN:
                case SDL_EVENT_FINGER_UP:
                case SDL_EVENT_FINGER_MOTION:
                    last_input = std::chrono::steady_clock::now();
                    break;
                default:
                    break;
                }
            }
            // The simulation thread runs independently.  The UI thread only
            // presents the newest published snapshot and never advances Core.
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            app.host_input.begin_frame();
            app.draw(smoke);
            app.host_input.finish_frame(window, app.controller);
            app.update_audio_input();
            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 9, 9, 10, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            if (smoke && frames == 30 && argc > 2) {
                SDL_Surface *pixels = SDL_RenderReadPixels(renderer, nullptr);
                if (!pixels)
                    throw std::runtime_error(SDL_GetError());
                bool saved = SDL_SaveBMP(pixels, argv[2]);
                SDL_DestroySurface(pixels);
                if (!saved)
                    throw std::runtime_error(SDL_GetError());
            }
            SDL_RenderPresent(renderer);
            if (app.video_late_render_clear) {
                SDL_SetRenderDrawColor(renderer, 9, 9, 10, 255);
                SDL_RenderClear(renderer);
            }
            // The smoke run also waits for the one startup card-plugin discovery
            // request, so a broken discovery path cannot pass as "frames drawn".
            if (smoke && ++frames >= 32 && (!app.discovering_card_types() || frames > 200)) {
                app.controller.request_pause();
                if (!app.error.empty())
                    throw std::runtime_error(app.error);
                if (app.discovering_card_types())
                    throw std::runtime_error("GUI smoke: card plugin discovery did not complete");
                running = false;
                std::cout << "GUI smoke: panels rendered; " << app.snapshot->inspection->cards.size()
                          << " cards, " << app.card_types.size() << " card types and "
                          << app.loaded_tool_count() << " tools loaded\n";
            }
            int target_fps = app.video_frame_rate_limit;
            const auto idle_for = std::chrono::steady_clock::now() - last_input;
            const bool input_idle = idle_for >= std::chrono::seconds(5);
            if (app.video_power_saving && !app.file_dialog_pending() && app.snapshot &&
                app.snapshot->paused() && input_idle &&
                (target_fps <= 0 || target_fps > 2))
                target_fps = 2;
            const auto presented = std::chrono::steady_clock::now();
            const auto frame_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(presented - frame_start).count();
            if (target_fps > 0) {
                const auto target_ns = 1000000000LL / target_fps;
                const auto remaining_ns = target_ns - frame_ns;
                if (remaining_ns > 0)
                    SDL_Delay(static_cast<uint32_t>((remaining_ns + 999999) / 1000000));
            } else {
                SDL_Delay(1);
            }
            // Report the full frame interval, including the power-saving /
            // frame-rate sleep, so the on-screen frame time reflects the actual
            // cadence rather than only the render time before the delay.
            const auto frame_end = std::chrono::steady_clock::now();
            const auto total_frame_ns =
                std::chrono::duration_cast<std::chrono::nanoseconds>(frame_end - frame_start).count();
            app.video_frame_time_ms = static_cast<double>(total_frame_ns) / 1000000.0;
            constexpr double display_smoothing = 0.10;
            if (app.video_display_frame_time_ms <= 0.0)
                app.video_display_frame_time_ms = app.video_frame_time_ms;
            else
                app.video_display_frame_time_ms +=
                    display_smoothing * (app.video_frame_time_ms - app.video_display_frame_time_ms);
        }
        app.controller.request_pause();
        save_window_bounds(app, window);
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    if (renderer && imgui_backends_initialized) {
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
    }
    ImGui::DestroyContext();
    if (renderer)
        SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return result;
}
