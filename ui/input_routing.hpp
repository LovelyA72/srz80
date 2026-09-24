#pragma once
#include "ui_snapshot.hpp"
#include <srz80/input.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <optional>

namespace srz80::ui {
struct InputDevice {
    Handle owner = 0;
    std::string name, keyboard, mouse;
    bool relative = false, absolute = false;
    bool operator==(const InputDevice &) const = default;
};
inline std::vector<InputDevice> input_devices(const UiSnapshot &snapshot) {
    std::vector<InputDevice> devices;
    for (const auto &[owner, provider] : *snapshot.providers) {
        if (provider.protocol != SRH_INPUT_PROTOCOL) continue;
        const auto data = nlohmann::json::parse(provider.data, nullptr, false);
        if (!data.is_object() || !data.contains("schema") || data["schema"] != 1) continue;
        auto endpoint = [](const nlohmann::json &object) -> std::string {
            if (!object.is_object() || !object.contains("endpoint") || !object["endpoint"].is_string()) return {};
            auto name = object["endpoint"].get<std::string>();
            return name.size() <= 127 && name.find('\0') == std::string::npos ? name : std::string{};
        };
        InputDevice device{owner, provider.name, {}, {}, false, false};
        if (data.contains("keyboard")) device.keyboard = endpoint(data["keyboard"]);
        if (data.contains("mouse")) {
            const auto &mouse = data["mouse"];
            device.mouse = endpoint(mouse);
            if (mouse.is_object()) {
                device.relative = mouse.contains("relative") && mouse["relative"] == true;
                device.absolute = mouse.contains("absolute") && mouse["absolute"] == true;
            }
        }
        // Streams have different framing and must not share an endpoint.
        if (!device.keyboard.empty() && device.keyboard == device.mouse) continue;
        if (!device.keyboard.empty() || (!device.mouse.empty() && (device.relative || device.absolute)))
            devices.push_back(std::move(device));
    }
    return devices;
}
struct InputRoute {
    std::string video_card;
    uint32_t surface = 0;
    std::string keyboard, mouse;
    bool relative = false;
    bool operator==(const InputRoute &) const = default;
};
struct InputRect {
    float x = 0, y = 0, width = 0, height = 0;
    bool contains(float px, float py) const {
        return width > 0 && height > 0 && px >= x && py >= y && px < x + width && py < y + height;
    }
};
inline std::optional<std::pair<int32_t, int32_t>> input_position(
    InputRect image, InputRect clip, uint32_t width, uint32_t height, float x, float y) {
    if (!width || !height || width > INT32_MAX || height > INT32_MAX ||
        !image.contains(x, y) || !clip.contains(x, y)) return {};
    return std::pair{static_cast<int32_t>(std::clamp(std::floor(double(x - image.x) * width / image.width), 0.0, double(width - 1))),
                     static_cast<int32_t>(std::clamp(std::floor(double(y - image.y) * height / image.height), 0.0, double(height - 1)))};
}
}
