#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace srz80::ui {

struct RackThumbnail {
    std::vector<uint8_t> pixels;
    int width = 0, height = 0;
    std::string encoded;
    std::string error;
};

RackThumbnail load_rack_thumbnail(const std::filesystem::path &path);
RackThumbnail decode_rack_thumbnail(const std::string &encoded);

} // namespace srz80::ui
