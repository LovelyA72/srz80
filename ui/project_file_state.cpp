#include "project_file_state.hpp"
#include <array>
#include <fstream>

namespace srz80::ui {
ProjectFileState read_project_source(const std::filesystem::path &path) {
    ProjectFileState result;
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        result.kind = ProjectFileState::Kind::missing;
        return result;
    }
    if (error || !std::filesystem::is_regular_file(status)) {
        result.kind = ProjectFileState::Kind::unreadable;
        result.error = error ? error.message() : "Not a regular file";
        return result;
    }
    std::ifstream input(path, std::ios::binary);
    std::array<char, 65536> buffer{};
    if (input) input.read(buffer.data(), buffer.size());
    if (!input.eof() || input.bad()) {
        result.kind = ProjectFileState::Kind::unreadable;
        result.error = input.gcount() == static_cast<std::streamsize>(buffer.size())
                           ? "File exceeds the editor's 65535-byte limit" : "Cannot read file";
        return result;
    }
    result.text.assign(buffer.data(), static_cast<size_t>(input.gcount()));
    if (result.text.find('\0') != std::string::npos) {
        result.kind = ProjectFileState::Kind::unreadable;
        result.error = "File contains binary data";
        result.text.clear();
    }
    return result;
}
std::vector<ProjectFileProbe> probe_project_sources(std::vector<ProjectFileProbe> probes) {
    for (auto &probe : probes) probe.observed = read_project_source(probe.path);
    return probes;
}
}
