#pragma once
#include "project_file_state.hpp"
#include <optional>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

namespace srz80::ui {
struct ProjectSaveDestination {
    std::filesystem::path old_root, root, file;
    bool create_workspace = false;
};
std::string project_display_name(const nlohmann::json &document, const std::filesystem::path &file);
// Startup-only metadata read. Rendering uses cached recent-project names.
std::string read_project_name(const std::filesystem::path &file);
// A new destination is a folder. Ordinary Save passes the current manifest path.
ProjectSaveDestination project_save_destination(const std::filesystem::path &old_file,
                                                const std::filesystem::path &requested);
nlohmann::json project_with_absolute_assets(nlohmann::json document,
                                          const std::filesystem::path &project_file);
void write_project_document(const std::filesystem::path &file, const nlohmann::json &document);

struct ProjectSourceSnapshot {
    uint64_t id;
    std::filesystem::path path;
    std::string text;
    std::optional<ProjectFileState> expected_disk;
};
struct ProjectWriteRequest {
    std::filesystem::path old_file, requested;
    nlohmann::json document;
    std::vector<ProjectSourceSnapshot> sources;
    bool execution_state = false;
    std::vector<std::string> tool_path_states;
};
struct ProjectWriteResult {
    bool success = false;
    std::vector<ProjectFileProbe> conflicts;
    std::string error;
    std::string name;
    ProjectSaveDestination destination;
    nlohmann::json tool_state;
    std::vector<ProjectSourceSnapshot> sources;
};
// Owns all filesystem inputs. No GUI, tool or session references cross threads.
ProjectWriteResult write_project_snapshot(ProjectWriteRequest request);
}
