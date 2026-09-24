#pragma once
#include "project_file_state.hpp"
#include <optional>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace srz80::ui {

struct ProjectDocument {
    std::filesystem::path path;
    std::array<char, 65536> text{};
    std::string saved_text;
    ProjectFileState expected_disk;
    std::optional<ProjectFileState> external_change;
    uint64_t cursor = 0;
    uint64_t id = 0;
    bool dirty() const { return saved_text != text.data(); }
};

struct ProjectEdit {
    std::filesystem::path path;
    std::string before, after;
    uint64_t before_cursor = 0, after_cursor = 0;
};

class ProjectWorkspace {
  public:
    ProjectDocument *find(uint64_t id);
    ProjectDocument &open(const std::filesystem::path &path);
    ProjectDocument *find(const std::filesystem::path &path);
    const ProjectDocument *find(const std::filesystem::path &path) const;
    bool update(const std::filesystem::path &path, std::string_view text, uint64_t cursor,
                bool record = true);
    bool undo(ProjectEdit &edit);
    bool redo(ProjectEdit &edit);
    void save_all();
    std::vector<ProjectFileProbe> file_probes() const;
    bool observe_file(const ProjectFileProbe &probe);
    bool reload_file(const ProjectFileProbe &probe);
    bool allow_overwrite(const ProjectFileProbe &probe);
    ProjectDocument *rename(const std::filesystem::path &from, const std::filesystem::path &to);
    void relocate(const std::filesystem::path &from_root, const std::filesystem::path &to_root);
    void forget(const std::filesystem::path &path);
    void forget_under(const std::filesystem::path &directory);
    void clear();
    bool can_undo() const { return !undo_.empty(); }
    bool can_redo() const { return !redo_.empty(); }
    bool dirty() const;
    const auto &documents() const { return documents_; }

  private:
    uint64_t next_id_ = 1;
    static std::filesystem::path key(const std::filesystem::path &path);
    std::map<std::filesystem::path, std::unique_ptr<ProjectDocument>> documents_;
    std::vector<ProjectEdit> undo_, redo_;
};
} // namespace srz80::ui
