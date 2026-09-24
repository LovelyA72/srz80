#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace srz80::ui {
struct ProjectFileState {
    enum class Kind { present, missing, unreadable };
    Kind kind = Kind::present;
    std::string text, error;
    bool operator==(const ProjectFileState &) const = default;
};
// Bounded reads, also detecting atomic-replacement saves and same-size edits.
ProjectFileState read_project_source(const std::filesystem::path &path);
struct ProjectFileProbe {
    uint64_t id = 0;
    std::filesystem::path path;
    ProjectFileState expected, observed;
};
std::vector<ProjectFileProbe> probe_project_sources(std::vector<ProjectFileProbe> probes);
}
