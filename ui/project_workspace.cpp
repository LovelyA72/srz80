#include "project_workspace.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <stdexcept>

namespace srz80::ui {
std::filesystem::path ProjectWorkspace::key(const std::filesystem::path &path) {
    return std::filesystem::absolute(path).lexically_normal();
}

ProjectDocument &ProjectWorkspace::open(const std::filesystem::path &path) {
    const auto normalized = key(path);
    if (auto found = documents_.find(normalized); found != documents_.end())
        return *found->second;
    std::ifstream input(normalized, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot open file: " + normalized.string());
    std::string text{std::istreambuf_iterator<char>(input), {}};
    if (text.find('\0') != std::string::npos)
        throw std::runtime_error("Only text project files can be edited");
    auto document = std::make_unique<ProjectDocument>();
    if (text.size() >= document->text.size())
        throw std::runtime_error("Text file is too large for the editor");
    document->id = next_id_++;
    document->path = normalized;
    std::memcpy(document->text.data(), text.data(), text.size());
    document->text[text.size()] = 0;
    document->saved_text = text;
    document->expected_disk.text = text;
    return *documents_.emplace(normalized, std::move(document)).first->second;
}

ProjectDocument *ProjectWorkspace::find(uint64_t id) {
    for (auto &[path, document] : documents_)
        if (document->id == id) return document.get();
    return nullptr;
}

ProjectDocument *ProjectWorkspace::find(const std::filesystem::path &path) {
    const auto found = documents_.find(key(path));
    return found == documents_.end() ? nullptr : found->second.get();
}
const ProjectDocument *ProjectWorkspace::find(const std::filesystem::path &path) const {
    const auto found = documents_.find(key(path));
    return found == documents_.end() ? nullptr : found->second.get();
}

bool ProjectWorkspace::update(const std::filesystem::path &path, std::string_view text,
                              uint64_t cursor, bool record) {
    auto *document = find(path);
    if (!document || text.size() >= document->text.size())
        return false;
    const std::string before = document->text.data();
    const auto before_cursor = document->cursor;
    if (before == text) {
        document->cursor = std::min<uint64_t>(cursor, text.size());
        return false;
    }
    if (record) {
        undo_.push_back({document->path, before, std::string(text), before_cursor, cursor});
        if (undo_.size() > 512)
            undo_.erase(undo_.begin());
        redo_.clear();
    }
    std::memcpy(document->text.data(), text.data(), text.size());
    document->text[text.size()] = 0;
    document->cursor = std::min<uint64_t>(cursor, text.size());
    return true;
}

bool ProjectWorkspace::undo(ProjectEdit &edit) {
    if (undo_.empty()) return false;
    edit = undo_.back(); undo_.pop_back(); redo_.push_back(edit);
    return update(edit.path, edit.before, edit.before_cursor, false);
}
bool ProjectWorkspace::redo(ProjectEdit &edit) {
    if (redo_.empty()) return false;
    edit = redo_.back(); redo_.pop_back(); undo_.push_back(edit);
    return update(edit.path, edit.after, edit.after_cursor, false);
}

void ProjectWorkspace::save_all() {
    for (auto &[path, document] : documents_) {
        if (!document->dirty()) continue;
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write project file: " + path.string());
        output << document->text.data();
        if (!output) throw std::runtime_error("Cannot finish writing project file: " + path.string());
        output.close();
        if (!output) throw std::runtime_error("Cannot close project file: " + path.string());
        document->saved_text = document->text.data();
        document->expected_disk = {ProjectFileState::Kind::present, document->saved_text, {}};
        document->external_change.reset();
    }
}

std::vector<ProjectFileProbe> ProjectWorkspace::file_probes() const {
    std::vector<ProjectFileProbe> probes;
    for (const auto &[path, document] : documents_)
        probes.push_back({document->id, path, document->expected_disk, {}});
    return probes;
}
bool ProjectWorkspace::reload_file(const ProjectFileProbe &probe) {
    auto *document = find(probe.id);
    if (!document || document->path != probe.path || document->expected_disk != probe.expected ||
        probe.observed.kind != ProjectFileState::Kind::present || probe.observed.text.size() >= document->text.size() ||
        probe.observed.text.find('\0') != std::string::npos)
        return false;
    update(document->path, probe.observed.text, document->cursor, false);
    document->saved_text = probe.observed.text;
    document->expected_disk = probe.observed;
    document->external_change.reset();
    for (auto *history : {&undo_, &redo_})
        std::erase_if(*history, [&](const auto &edit) { return edit.path == document->path; });
    return true;
}
bool ProjectWorkspace::observe_file(const ProjectFileProbe &probe) {
    auto *document = find(probe.id);
    if (!document || document->path != probe.path || document->expected_disk != probe.expected) return false;
    if (probe.observed == document->expected_disk) {
        document->external_change.reset();
        return false;
    }
    if ((!document->dirty() || probe.observed.text == document->text.data()) &&
        probe.observed.kind == ProjectFileState::Kind::present)
        return reload_file(probe);
    document->external_change = probe.observed;
    return false;
}
bool ProjectWorkspace::allow_overwrite(const ProjectFileProbe &probe) {
    auto *document = find(probe.id);
    if (!document || document->path != probe.path || document->expected_disk != probe.expected ||
        probe.observed.kind == ProjectFileState::Kind::unreadable) return false;
    document->expected_disk = probe.observed;
    document->external_change.reset();
    return true;
}

bool ProjectWorkspace::dirty() const {
    return std::any_of(documents_.begin(), documents_.end(),
                       [](const auto &entry) { return entry.second->dirty(); });
}

ProjectDocument *ProjectWorkspace::rename(const std::filesystem::path &from,
                                          const std::filesystem::path &to) {
    const auto old_key = key(from), new_key = key(to);
    auto node = documents_.extract(old_key);
    if (node.empty()) return nullptr;
    node.key() = new_key;
    node.mapped()->path = new_key;
    auto *result = node.mapped().get();
    documents_.insert(std::move(node));
    for (auto *history : {&undo_, &redo_})
        for (auto &edit : *history)
            if (edit.path == old_key) edit.path = new_key;
    return result;
}

void ProjectWorkspace::relocate(const std::filesystem::path &from_root,
                                const std::filesystem::path &to_root) {
    const auto old_root = key(from_root), new_root = key(to_root);
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> moves;
    for (const auto &[path, document] : documents_) {
        (void)document;
        const auto relative = path.lexically_relative(old_root);
        if (!relative.empty() && *relative.begin() != "..")
            moves.emplace_back(path, (new_root / relative).lexically_normal());
    }
    for (const auto &[from, to] : moves)
        rename(from, to);
}

void ProjectWorkspace::forget(const std::filesystem::path &path) {
    const auto normalized = key(path);
    documents_.erase(normalized);
    auto remove_edits = [&](auto &history) {
        std::erase_if(history, [&](const auto &edit) { return edit.path == normalized; });
    };
    remove_edits(undo_);
    remove_edits(redo_);
}

void ProjectWorkspace::forget_under(const std::filesystem::path &directory) {
    const auto root = key(directory);
    std::vector<std::filesystem::path> paths;
    for (const auto &[path, document] : documents_) {
        (void)document;
        const auto relative = path.lexically_relative(root);
        if (!relative.empty() && *relative.begin() != "..") paths.push_back(path);
    }
    for (const auto &path : paths) forget(path);
}

void ProjectWorkspace::clear() {
    documents_.clear(); undo_.clear(); redo_.clear();
}
} // namespace srz80::ui
