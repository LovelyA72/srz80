#include "project_persistence.hpp"
#include <fstream>
#include <atomic>
#include <chrono>
#include <stdexcept>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#endif

namespace srz80::ui {
std::string project_display_name(const nlohmann::json &document, const std::filesystem::path &file) {
    const auto name = document.find("name");
    if (name != document.end() && name->is_string() &&
        name->get_ref<const std::string &>().find_first_not_of(" \t\r\n") != std::string::npos)
        return name->get<std::string>();
    if (file.empty()) return "Untitled project";
    const auto fallback = file.filename() == "project.json" ? file.parent_path().filename() : file.stem();
    return fallback.empty() ? "Untitled project" : fallback.string();
}
std::string read_project_name(const std::filesystem::path &file) {
    try {
        std::ifstream input(file, std::ios::binary);
        if (input) return project_display_name(nlohmann::json::parse(input), file);
    } catch (...) {}
    return project_display_name(nlohmann::json::object(), file);
}

ProjectSaveDestination project_save_destination(const std::filesystem::path &old_file,
                                                const std::filesystem::path &requested) {
    if (requested.empty() || requested.filename().empty())
        throw std::runtime_error("Project folder name is required");
    const auto old = old_file.empty() ? old_file : std::filesystem::absolute(old_file).lexically_normal();
    const auto path = std::filesystem::absolute(requested).lexically_normal();
    ProjectSaveDestination result;
    result.old_root = old.parent_path();
    result.create_workspace = old.empty() || path != old;
    result.root = result.create_workspace ? path : result.old_root;
    result.file = result.create_workspace ? result.root / "project.json" : path;
    if (result.create_workspace) {
        if (std::filesystem::exists(result.root))
            throw std::runtime_error("Project folder already exists: " + result.root.string());
        if (!result.old_root.empty()) {
            const auto relative = result.root.lexically_relative(result.old_root);
            if (!relative.empty() && *relative.begin() != "..")
                throw std::runtime_error("New project folder cannot be inside the original project folder");
        }
    }
    return result;
}
nlohmann::json project_with_absolute_assets(nlohmann::json document,
                                          const std::filesystem::path &project_file) {
    const auto root = project_file.parent_path();
    if (!root.empty() && document.contains("cards") && document["cards"].is_array()) {
        for (auto &card : document["cards"]) {
            if (card.is_object() && card.contains("image") && card["image"].is_string()) {
                const auto image = std::filesystem::path(card["image"].get<std::string>());
                if (!image.empty() && image.is_relative())
                    card["image"] = (root / image).lexically_normal().string();
            }
            if (card.is_object() && card.contains("images") && card["images"].is_array()) {
                for (auto &entry : card["images"]) if (entry.is_string()) {
                    const auto image = std::filesystem::path(entry.get<std::string>());
                    if (!image.empty() && image.is_relative())
                        entry = (root / image).lexically_normal().string();
                }
            }
        }
    }
    return document;
}
namespace {
// A reserved sibling directory avoids truncating another writer's temporary file.
std::filesystem::path reserve_staging(const std::filesystem::path &parent) {
    static std::atomic<uint64_t> sequence{0};
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto name = ".srz80-save-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                          "-" + std::to_string(++sequence);
        const auto path = parent / name;
        if (std::filesystem::create_directory(path)) return path;
    }
    throw std::runtime_error("Cannot reserve save staging directory");
}
struct SourceConflict { ProjectFileProbe probe; };
void check_source(const ProjectSourceSnapshot &source) {
    if (!source.expected_disk) return;
    const auto observed = read_project_source(source.path);
    if (observed != *source.expected_disk &&
        !(observed.kind == ProjectFileState::Kind::present && observed.text == source.text))
        throw SourceConflict{{source.id, source.path, *source.expected_disk, observed}};
}
void replace_text(const std::filesystem::path &file, const std::string &text,
                  const ProjectSourceSnapshot *source = nullptr) {
    const auto staging = reserve_staging(file.parent_path());
    try {
        const auto temporary = staging / "content";
        std::ofstream output(temporary, std::ios::binary);
        output.write(text.data(), static_cast<std::streamsize>(text.size()));
        output.close();
        if (!output) throw std::runtime_error("Cannot write file: " + file.string());
        if (source) check_source(*source);
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot replace file: " + file.string());
#else
        std::filesystem::rename(temporary, file);
#endif
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(staging, ignored);
        throw;
    }
    std::error_code ignored;
    std::filesystem::remove(staging, ignored);
}
}
void write_project_document(const std::filesystem::path &file, const nlohmann::json &document) {
    replace_text(file, document.dump(2) + '\n');
}
ProjectWriteResult write_project_snapshot(ProjectWriteRequest request) {
    ProjectWriteResult result;
    std::filesystem::path staging;
    size_t external_writes = 0;
    try {
        if (request.execution_state) {
            result.destination.file = std::filesystem::absolute(request.requested).lexically_normal();
            replace_text(result.destination.file, request.document.dump(2) + '\n');
            result.success = true;
            return result;
        }
        // Check the entire batch before creating staging files or writing sources.
        for (const auto &source : request.sources) {
            try { check_source(source); }
            catch (const SourceConflict &conflict) { result.conflicts.push_back(conflict.probe); }
        }
        if (!result.conflicts.empty()) {
            result.error = "Source files changed outside SRZ80";
            return result;
        }
        result.destination = project_save_destination(request.old_file, request.requested);
        const auto &plan = result.destination;
        result.name = project_display_name(request.document, request.old_file.empty() ? plan.file : request.old_file);
        request.document["name"] = result.name;
        result.tool_state = request.document.value("tool_state", nlohmann::json::object());
        if (plan.create_workspace && result.tool_state.is_object()) {
            for (const auto &key : request.tool_path_states) {
                auto state = result.tool_state.find(key);
                if (state == result.tool_state.end() || !state->is_string()) continue;
                const auto text = state->get<std::string>();
                if (text.empty()) continue;
                auto path = std::filesystem::path(text);
                if (path.is_relative()) {
                    if (plan.old_root.empty()) continue;
                    path = (plan.old_root / path).lexically_normal();
                }
                const auto relative = path.lexically_relative(plan.old_root);
                if (!plan.old_root.empty() && !relative.empty() && *relative.begin() != "..") path = plan.root / relative;
                const auto relocated = path.lexically_relative(plan.root);
                *state = relocated.empty() ? path.string() : relocated.string();
            }
            if (request.document.contains("tool_state")) request.document["tool_state"] = result.tool_state;
        }
        const auto serialized = request.document.dump(2) + '\n';
        auto write_root = plan.root;
        if (plan.create_workspace) {
            staging = reserve_staging(plan.root.parent_path());
            write_root = staging;
            if (!plan.old_root.empty())
                for (const auto &entry : std::filesystem::directory_iterator(plan.old_root)) {
                    if (entry.path() == std::filesystem::absolute(request.old_file).lexically_normal()) continue;
                    std::filesystem::copy(entry.path(), staging / entry.path().filename(),
                        std::filesystem::copy_options::recursive | std::filesystem::copy_options::copy_symlinks);
                }
        }
        for (const auto &source : request.sources) {
            auto destination = source.path;
            const auto relative = source.path.lexically_relative(plan.old_root);
            const bool internal = !plan.old_root.empty() && !relative.empty() && *relative.begin() != "..";
            if (plan.create_workspace && internal) destination = write_root / relative;
            replace_text(destination, source.text, &source);
            if (!plan.create_workspace || !internal) ++external_writes;
        }
        replace_text(write_root / plan.file.filename(), serialized);
        if (plan.create_workspace) {
            // Never overwrite a destination created since preparation.
            if (std::filesystem::exists(plan.root)) throw std::runtime_error("Project folder already exists: " + plan.root.string());
            std::filesystem::rename(staging, plan.root);
            staging.clear();
        }
        result.sources = std::move(request.sources);
        result.success = true;
    } catch (const SourceConflict &conflict) {
        result.conflicts.push_back(conflict.probe);
        result.error = "A source file changed while saving";
        if (external_writes) result.error += "; some source files were already written";
    } catch (const std::exception &error) {
        result.error = error.what();
        if (external_writes)
            result.error += "; some source files were written; active paths and unsaved baselines were retained";
    }
    if (!staging.empty()) {
        std::error_code error;
        std::filesystem::remove_all(staging, error);
        if (error) result.error += "; staging cleanup failed: " + staging.string();
    }
    return result;
}
} // namespace srz80::ui
