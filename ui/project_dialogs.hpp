#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace srz80::ui {
// Native callbacks retain only this mailbox and value identities. Neither the
// application/session nor a plugin is reachable through a pending request.
enum class ProjectDialogAction { open, save_as, open_state, save_state_as,
                                 select_project_parent,
                                 select_add_rom, select_info_rom,
                                 select_add_project_file, select_info_project_file,
                                 select_inspector_project_file,
                                 select_video_shader,
                                 select_rack_thumbnail,
                                 import_layout, export_layout };
struct ProjectDialogResult {
    ProjectDialogAction action;
    uint64_t operation, epoch;
    std::string path, error;
    uint32_t image_slot = 0;
    uint64_t card = 0;
    std::string config_key{};
};
struct ProjectDialogState {
    std::mutex mutex;
    std::vector<ProjectDialogResult> results;
    std::atomic_uint32_t pending_dialogs = 0;
};
struct ProjectDialogRequest {
    std::shared_ptr<ProjectDialogState> state;
    ProjectDialogAction action;
    uint64_t operation, epoch;
    uint32_t image_slot = 0;
    uint64_t card = 0;
    std::string config_key{};
};
} // namespace srz80::ui
