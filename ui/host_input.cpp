#include "host_input.hpp"
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>

namespace srz80::ui {
namespace {
constexpr size_t max_pending = 65536;
int32_t whole(double &fraction) {
    const auto value = static_cast<int32_t>(std::clamp(std::trunc(fraction),
        double(INT32_MIN), double(INT32_MAX)));
    fraction -= value;
    return value;
}
}
void HostInput::queue(Stream &stream, const uint8_t *bytes, size_t size) {
    if (!stream.owner || stream.releasing) return;
    if (stream.pending.size() + stream.submitted.size() + size > max_pending) {
        error_ = "Input queue is full; capture released.";
        return;
    }
    stream.pending.insert(stream.pending.end(), bytes, bytes + size);
}
void HostInput::pump(Stream &stream, SimulationController &controller) {
    if (stream.reply.valid()) {
        if (stream.reply.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        const auto result = stream.reply.get();
        if (result.status == SRH_OK) {
            stream.submitted.clear();
            if (stream.submitted_release) {
                stream.cleanup.clear(); stream.releasing = false; stream.owner = 0;
                stream.endpoint.clear();
            }
        } else if (result.status == SRH_UNAVAILABLE) {
            // Keep the rejected batch before later transitions. Cleanup replaces
            // rejected normal input, but is itself retried until accepted.
            if (!stream.releasing) {
                stream.submitted.insert(stream.submitted.end(), stream.pending.begin(), stream.pending.end());
                stream.pending = std::move(stream.submitted);
            }
        } else {
            if (result.status != SRH_NOT_FOUND && result.status != SRH_CONFLICT)
                error_ = "Input delivery failed (" + std::to_string(result.status) + ").";
            else if (active()) error_ = "Input destination is no longer available.";
            stream.submitted.clear();
            stream.pending.clear();
            // Generation/owner rejection means this stream cannot affect the
            // current rack. Never retry it against a newly resolved card.
            if (stream.submitted_release && (result.status == SRH_NOT_FOUND || result.status == SRH_CONFLICT)) {
                stream.cleanup.clear(); stream.releasing = false; stream.owner = 0;
            }
        }
    }
    if (stream.releasing) {
        stream.submitted.clear();
        stream.submitted_release = true;
        stream.reply = controller.input_release_async(stream.endpoint, stream.cleanup,
            stream.generation, stream.source, stream.identity, stream.owner);
    } else if (!stream.pending.empty()) {
        stream.submitted = std::move(stream.pending);
        stream.pending.clear(); stream.submitted_release = false;
        stream.reply = controller.input_batch_async(stream.endpoint, UINT64_MAX, stream.submitted,
            stream.generation, stream.source, stream.identity, stream.owner);
    }
}
void HostInput::update(SDL_Window *window, SimulationController &controller,
                       const ProjectSession &session, bool available) {
    auto snapshot = controller.snapshot();
    available_ = available && session.loaded() && !session.busy() && snapshot &&
                 snapshot->generation == session.generation() && !snapshot->paused();
    pump(keyboard_, controller); pump(mouse_, controller);
    if (!active()) return;
    bool valid = available_ && snapshot->generation == generation_;
    if (valid) {
        const auto routes = session.input_routes();
        valid = std::find(routes.begin(), routes.end(), route_) != routes.end();
        const auto devices = input_devices(*snapshot);
        auto supports = [&](const Stream &stream, bool mouse) {
            if (!stream.owner) return true;
            return std::any_of(devices.begin(), devices.end(), [&](const auto &device) {
                return device.owner == stream.owner && (mouse ? device.mouse : device.keyboard) == stream.endpoint &&
                       (!mouse || (relative_ ? device.relative : device.absolute));
            });
        };
        valid = valid && supports(keyboard_, false) && supports(mouse_, true);
    }
    if (!valid || !error_.empty()) release(window, controller);
}
void HostInput::release(SDL_Window *window, SimulationController &controller) {
    if (!active()) return;
    if (window) {
        SDL_SetWindowRelativeMouseMode(window, false);
        SDL_SetWindowKeyboardGrab(window, false);
    }
    if (changed_mouse_hint_) {
        if (mouse_auto_capture_hint_) SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, mouse_auto_capture_hint_->c_str());
        else SDL_ResetHint(SDL_HINT_MOUSE_AUTO_CAPTURE);
        changed_mouse_hint_ = false;
    }
    keyboard_.cleanup.clear(); mouse_.cleanup.clear();
    for (size_t usage = 0; usage < keys_.size(); ++usage) {
        if (touched_keys_[usage]) {
            keyboard_.cleanup.push_back(static_cast<uint8_t>(usage)); keyboard_.cleanup.push_back(0);
        }
        if (keys_[usage]) suppressed_keys_[usage] = true;
    }
    for (size_t button = 1; button < buttons_.size(); ++button) if (touched_buttons_[button]) {
        uint8_t packet[SRH_MOUSE_PACKET_SIZE];
        srh_mouse_packet(packet, SRH_MOUSE_BUTTON, 0, button, 0, 0, 0, 0);
        mouse_.cleanup.insert(mouse_.cleanup.end(), std::begin(packet), std::end(packet));
        if (buttons_[button]) suppressed_buttons_[button] = true;
    }
    if (mouse_.owner) {
        uint8_t packet[SRH_MOUSE_PACKET_SIZE];
        srh_mouse_packet(packet, SRH_MOUSE_LEAVE, 0, 0, 0, 0, 0, 0);
        mouse_.cleanup.insert(mouse_.cleanup.end(), std::begin(packet), std::end(packet));
    }
    for (auto *stream : {&keyboard_, &mouse_}) if (stream->owner) {
        stream->pending.clear(); stream->releasing = true; ++stream->identity;
        pump(*stream, controller);
    }
    active_ = {}; keys_.fill(false); buttons_.fill(false); inside_ = false;
    relative_ = false;
    if (ImGui::GetCurrentContext()) ImGui::GetIO().ClearInputKeys();
}
bool HostInput::acquire(const Surface &surface, SDL_Window *window, SimulationController &controller,
                        const ProjectSession &session) {
    if (!available_ || keyboard_.busy() || mouse_.busy()) return false;
    const auto routes = session.input_routes();
    const auto key = session.input_card_id(surface.owner);
    auto route = std::find_if(routes.begin(), routes.end(), [&](const auto &entry) {
        return entry.video_card == key && entry.surface == surface.ordinal;
    });
    if (route == routes.end()) return false;
    const auto snapshot = controller.snapshot();
    if (!snapshot || snapshot->generation != session.generation() || snapshot->paused()) return false;
    const auto devices = input_devices(*snapshot);
    const auto keyboard_owner = session.input_card_owner(route->keyboard);
    const auto mouse_owner = session.input_card_owner(route->mouse);
    const InputDevice *keyboard = nullptr, *mouse = nullptr;
    for (const auto &device : devices) {
        if (device.owner == keyboard_owner && !device.keyboard.empty()) keyboard = &device;
        if (device.owner == mouse_owner && !device.mouse.empty() &&
            (route->relative ? device.relative : device.absolute)) mouse = &device;
    }
    if (!keyboard && !mouse) return false;
    error_.clear();
    if (window && keyboard && !SDL_SetWindowKeyboardGrab(window, true)) {
        error_ = std::string("Cannot capture keyboard: ") + SDL_GetError(); return false;
    }
    relative_ = mouse && route->relative;
    if (window && relative_ && !SDL_SetWindowRelativeMouseMode(window, true)) {
        SDL_SetWindowKeyboardGrab(window, false);
        error_ = std::string("Cannot capture mouse: ") + SDL_GetError(); relative_ = false; return false;
    }
    if (window && mouse && !relative_) {
        const char *hint = SDL_GetHint(SDL_HINT_MOUSE_AUTO_CAPTURE);
        mouse_auto_capture_hint_ = hint ? std::optional<std::string>(hint) : std::nullopt;
        changed_mouse_hint_ = SDL_SetHint(SDL_HINT_MOUSE_AUTO_CAPTURE, "0");
        SDL_CaptureMouse(false);
    }
    generation_ = snapshot->generation; active_ = surface; route_ = *route;
    auto configure = [&](Stream &stream, uint64_t source, const InputDevice *device, bool mouse) {
        stream.source = source; stream.generation = generation_;
        stream.owner = device ? device->owner : 0;
        stream.endpoint = device ? (mouse ? device->mouse : device->keyboard) : std::string{};
    };
    configure(keyboard_, UINT64_MAX - 1, keyboard, false);
    configure(mouse_, UINT64_MAX - 2, mouse, true);
    keys_.fill(false); buttons_.fill(false);
    touched_keys_.fill(false); touched_buttons_.fill(false);
    relative_x_ = relative_y_ = wheel_x_ = wheel_y_ = 0;
    // Keys held before capture belong to the shell. Wait for their physical
    // release instead of sending unmatched key-ups or activating shortcuts.
    int count = 0; const bool *held = SDL_GetKeyboardState(&count);
    for (int i = 0; i < std::min(count, int(suppressed_keys_.size())); ++i) suppressed_keys_[i] = held[i];
    if (ImGui::GetCurrentContext()) {
        // SDL events received earlier in this batch may still be waiting for
        // ImGui::NewFrame. Clearing only current key state would replay them
        // into shell shortcuts after input ownership has changed.
        auto &events = ImGui::GetCurrentContext()->InputEventsQueue;
        for (int i = events.Size - 1; i >= 0; --i) {
            const auto type = events[i].Type;
            if (type == ImGuiInputEventType_Key || type == ImGuiInputEventType_Text ||
                type == ImGuiInputEventType_MouseButton || type == ImGuiInputEventType_MouseWheel)
                events.erase(events.Data + i);
        }
        ImGui::GetIO().ClearInputKeys(); ImGui::GetIO().ClearInputMouse();
    }
    if (surface.focus) surface.focus();
    return true;
}
void HostInput::mouse_packet(uint8_t type, int32_t x, int32_t y, uint16_t button, bool down) {
    uint8_t packet[SRH_MOUSE_PACKET_SIZE];
    srh_mouse_packet(packet, type, down, button, x, y,
        type == SRH_MOUSE_ABSOLUTE ? active_.width : 0,
        type == SRH_MOUSE_ABSOLUTE ? active_.height : 0);
    // Coalesce adjacent motion only, never across a button/wheel transition.
    if (type == SRH_MOUSE_RELATIVE && mouse_.pending.size() >= sizeof(packet) &&
        mouse_.pending[mouse_.pending.size() - sizeof(packet)] == SRH_MOUSE_RELATIVE) {
        auto *last = mouse_.pending.data() + mouse_.pending.size() - sizeof(packet);
        const int64_t dx = int64_t(static_cast<int32_t>(srh_input_get_u32(last + 4))) + x;
        const int64_t dy = int64_t(static_cast<int32_t>(srh_input_get_u32(last + 8))) + y;
        if (dx >= INT32_MIN && dx <= INT32_MAX && dy >= INT32_MIN && dy <= INT32_MAX) {
            srh_input_put_u32(last + 4, static_cast<uint32_t>(dx));
            srh_input_put_u32(last + 8, static_cast<uint32_t>(dy));
            return;
        }
    }
    if (type == SRH_MOUSE_ABSOLUTE && mouse_.pending.size() >= sizeof(packet) &&
        mouse_.pending[mouse_.pending.size() - sizeof(packet)] == SRH_MOUSE_ABSOLUTE) {
        std::copy(std::begin(packet), std::end(packet), mouse_.pending.end() - sizeof(packet));
    } else queue(mouse_, packet, sizeof(packet));
}
bool HostInput::position(float x, float y) {
    auto point = (!active_.hit_test || active_.hit_test(x, y))
        ? input_position(active_.image, active_.clip, active_.width, active_.height, x, y) : std::nullopt;
    if (point) {
        if (!inside_ || *point != last_position_ || last_width_ != active_.width || last_height_ != active_.height)
            mouse_packet(SRH_MOUSE_ABSOLUTE, point->first, point->second);
        last_position_ = *point; last_width_ = active_.width; last_height_ = active_.height; inside_ = true;
        return true;
    }
    if (inside_) {
        // Absolute mode never needs OS mouse capture: end guest drags on exit,
        // including an exit from the application where no button-up may arrive.
        for (size_t button = 1; button < buttons_.size(); ++button) if (buttons_[button]) {
            mouse_packet(SRH_MOUSE_BUTTON, 0, 0, button, false);
            buttons_[button] = false; suppressed_buttons_[button] = true;
        }
        mouse_packet(SRH_MOUSE_LEAVE); inside_ = false;
    }
    return false;
}
void HostInput::finish_frame(SDL_Window *window, SimulationController &controller) {
    if (active()) {
        auto found = std::find_if(surfaces_.begin(), surfaces_.end(), [&](const auto &s) { return s.id == active_.id; });
        if (found == surfaces_.end()) release(window, controller);
        else {
            active_ = *found;
            if (!relative_ && mouse_.owner) {
                float x = 0, y = 0; SDL_GetMouseState(&x, &y); position(x, y);
            }
        }
    }
    pump(keyboard_, controller); pump(mouse_, controller);
}
bool HostInput::event(const SDL_Event &event, SDL_Window *window, SimulationController &controller,
                      const ProjectSession &session) {
    const auto window_id = window ? SDL_GetWindowID(window) : 0;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST || event.type == SDL_EVENT_WINDOW_HIDDEN ||
        event.type == SDL_EVENT_WINDOW_MINIMIZED || event.type == SDL_EVENT_WINDOW_RESIZED ||
        event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED || event.type == SDL_EVENT_QUIT) {
        release(window, controller); surfaces_.clear();
        if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) { suppressed_keys_.fill(false); suppressed_buttons_.fill(false); }
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
        if (window_id && event.key.windowID != window_id) return false;
        const auto scan = static_cast<unsigned>(event.key.scancode);
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        if (scan >= suppressed_keys_.size()) return active();
        if (suppressed_keys_[scan]) { if (!down) suppressed_keys_[scan] = false; return true; }
        if (!active()) return false;
        // Check physical Ctrl/Alt events, not the layout-dependent key symbol.
        if (scan < keys_.size()) keys_[scan] = down;
        if (down && !(event.key.mod & SDL_KMOD_MODE) &&
            (keys_[SDL_SCANCODE_LCTRL] || keys_[SDL_SCANCODE_RCTRL]) &&
            (keys_[SDL_SCANCODE_LALT] || keys_[SDL_SCANCODE_RALT])) {
            release(window, controller); return true;
        }
        if (keyboard_.owner && scan >= SDL_SCANCODE_A && scan <= SDL_SCANCODE_RGUI) {
            if (down) touched_keys_[scan] = true;
            const uint8_t packet[]{static_cast<uint8_t>(scan), static_cast<uint8_t>((down ? SRH_KEY_DOWN : 0) |
                (down && event.key.repeat ? SRH_KEY_REPEAT : 0))};
            queue(keyboard_, packet, sizeof(packet));
        }
        return true;
    }
    if (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_TEXT_EDITING) return active();
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        if (window_id && event.button.windowID != window_id) return false;
        const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        const auto button = event.button.button;
        if (button < suppressed_buttons_.size() && suppressed_buttons_[button]) {
            if (!down) suppressed_buttons_[button] = false;
            return true;
        }
        if (!active() && down && button == SDL_BUTTON_LEFT) {
            for (const auto &surface : surfaces_) {
                if ((!surface.hit_test || surface.hit_test(event.button.x, event.button.y)) &&
                    input_position(surface.image, surface.clip, surface.width, surface.height,
                                                       event.button.x, event.button.y) &&
                    acquire(surface, window, controller, session)) {
                    // Activation click belongs to capture, not the guest.
                    suppressed_buttons_[button] = true; return true;
                }
            }
        }
        if (!active()) return false;
        const bool over = relative_ || position(event.button.x, event.button.y);
        if (!over && down) { release(window, controller); return false; }
        if (button >= buttons_.size()) return over;
        if (mouse_.owner && (over || (!down && buttons_[button]))) {
            if (down) touched_buttons_[button] = true;
            buttons_[button] = down; mouse_packet(SRH_MOUSE_BUTTON, 0, 0, button, down); return true;
        }
        return over;
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && active()) {
        if (window_id && event.motion.windowID != window_id) return false;
        if (relative_) {
            relative_x_ += event.motion.xrel; relative_y_ += event.motion.yrel;
            const auto x = whole(relative_x_), y = whole(relative_y_);
            if (x || y) mouse_packet(SRH_MOUSE_RELATIVE, x, y);
            return true;
        }
        if (mouse_.owner) position(event.motion.x, event.motion.y);
        return false; // Free cursor still drives shell hover outside the image.
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL && active()) {
        if (window_id && event.wheel.windowID != window_id) return false;
        float x, y; SDL_GetMouseState(&x, &y);
        if (!mouse_.owner || (!relative_ && !position(x, y))) return false;
        const double direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1;
        wheel_x_ += event.wheel.x * 120.0 * direction; wheel_y_ += event.wheel.y * 120.0 * direction;
        const auto dx = whole(wheel_x_), dy = whole(wheel_y_);
        if (dx || dy) mouse_packet(SRH_MOUSE_WHEEL, dx, dy);
        return true;
    }
    return false;
}
}
