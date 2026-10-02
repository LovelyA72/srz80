#pragma once
#include "ui_types.hpp"
#include <srz80/abi.h>
#include <nlohmann/json.hpp>
#include <stdexcept>

namespace srz80::ui {

inline nlohmann::json editable_card_configuration(nlohmann::json config, const UiCardType &type) {
    if (!config.is_object())
        throw std::invalid_argument("Plugin configuration must be a JSON object");
    if (type.flags & SRH_CARD_REQUIRES_IO_SPACE)
        config.erase(type.io_space_config_key);
    if (!type.memory_space_config_key.empty())
        config.erase(type.memory_space_config_key);
    if (!type.base_config_key.empty())
        config.erase(type.base_config_key);
    return config;
}

inline void bind_card_configuration(nlohmann::json &config, const UiCardType &type,
                                    const std::string &io_space, const std::string &memory_space,
                                    uint64_t base) {
    if (!config.is_object())
        throw std::invalid_argument("Plugin configuration must be a JSON object");
    if (type.flags & SRH_CARD_REQUIRES_IO_SPACE) {
        if (type.io_space_config_key.empty())
            throw std::invalid_argument("Card does not define its I/O space config key");
        if (io_space.empty())
            throw std::invalid_argument("Card requires an I/O address space");
        config[type.io_space_config_key] = io_space;
    }
    if (!type.memory_space_config_key.empty()) {
        if (memory_space.empty())
            throw std::invalid_argument("Card requires a memory address space");
        config[type.memory_space_config_key] = memory_space;
    }
    if (!type.base_config_key.empty())
        config[type.base_config_key] = base;
}

} // namespace srz80::ui
