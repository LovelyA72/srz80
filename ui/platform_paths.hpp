#pragma once
#include <filesystem>

namespace srz80 {
// Host platform paths.  Plugin and tool binaries are always resolved relative
// to the running executable, so changing the current working directory never
// changes runtime discovery.  These helpers stay free of SDL, ImGui and engine
// dependencies because the project and simulation libraries use them.
std::filesystem::path executable_directory();
} // namespace srz80
