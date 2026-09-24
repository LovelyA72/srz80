#pragma once

#include <filesystem>
#include <string>

namespace srz80::ui::platform {

bool reveal_in_file_manager(const std::filesystem::path &path, std::string &error);
bool move_to_trash(const std::filesystem::path &path, std::string &error);

} // namespace srz80::ui::platform
