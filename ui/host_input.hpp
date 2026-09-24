#pragma once
#include "input_routing.hpp"
#include "project_session.hpp"
#include "simulation_controller.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <functional>

namespace srz80::ui {
// Main-thread capture and delivery owner. Each stream has one worker request
// in flight; a failed enqueue retains its whole batch for retry.
class HostInput {
public:
    struct Surface {
        Handle id = 0, owner = 0;
        uint32_t ordinal = 0, width = 0, height = 0;
        InputRect image, clip;
        // Evaluate occlusion at the event position, not at last frame's mouse
        // position (motion and a click can arrive in the same SDL batch).
        std::function<bool(float, float)> hit_test;
        std::function<void()> focus;
    };
    void update(SDL_Window *, SimulationController &, const ProjectSession &, bool available);
    bool event(const SDL_Event &, SDL_Window *, SimulationController &, const ProjectSession &);
    void begin_frame() { surfaces_.clear(); }
    void surface(Surface value) { surfaces_.push_back(value); }
    void finish_frame(SDL_Window *, SimulationController &);
    void release(SDL_Window *, SimulationController &);
    bool active() const { return active_.id != 0; }
    bool owns(Handle surface) const { return active_.id == surface; }
    const std::string &error() const { return error_; }
private:
    struct Stream {
        uint64_t source = 0, generation = 0, identity = 1;
        Handle owner = 0;
        std::string endpoint;
        std::vector<uint8_t> pending, submitted, cleanup;
        SimulationController::AsyncReply reply;
        bool releasing = false, submitted_release = false;
        bool busy() const { return reply.valid() || !pending.empty() || releasing; }
    };
    Stream keyboard_{}, mouse_{};
    std::array<bool, 256> keys_{};
    std::array<bool, 6> buttons_{};
    // A physical up can still be queued when capture ends. Release every key
    // touched in this capture, not merely the keys still physically held.
    std::array<bool, 256> touched_keys_{};
    std::array<bool, 6> touched_buttons_{};
    std::array<bool, SDL_SCANCODE_COUNT> suppressed_keys_{};
    std::array<bool, 6> suppressed_buttons_{};
    std::vector<Surface> surfaces_;
    Surface active_{};
    InputRoute route_{};
    uint64_t generation_ = 0;
    bool relative_ = false, inside_ = false, available_ = false;
    std::pair<int32_t, int32_t> last_position_{};
    uint32_t last_width_ = 0, last_height_ = 0;
    std::optional<std::string> mouse_auto_capture_hint_;
    bool changed_mouse_hint_ = false;
    double relative_x_ = 0, relative_y_ = 0, wheel_x_ = 0, wheel_y_ = 0;
    std::string error_;
    void pump(Stream &, SimulationController &);
    bool acquire(const Surface &, SDL_Window *, SimulationController &, const ProjectSession &);
    void queue(Stream &, const uint8_t *, size_t);
    void mouse_packet(uint8_t type, int32_t x = 0, int32_t y = 0, uint16_t button = 0, bool down = false);
    bool position(float x, float y);
};
}
