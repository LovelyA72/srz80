#include "platform_paths.hpp"
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace srz80 {

std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(32768);
    auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (!length || length == buffer.size())
        throw std::runtime_error("Cannot locate executable");
    return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#else
    std::vector<char> buffer(32768);
    auto length = readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length < 0 || static_cast<size_t>(length) == buffer.size())
        throw std::runtime_error("Cannot locate executable");
    return std::filesystem::path(std::string(buffer.data(), static_cast<size_t>(length))).parent_path();
#endif
}

} // namespace srz80
