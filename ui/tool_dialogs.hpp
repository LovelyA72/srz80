#pragma once
#include <SDL3/SDL_dialog.h>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace srz80::ui {
struct ToolDialogState {
    std::mutex mutex;
    bool pending = true, cancelled = false, error = false;
    std::string path;
    std::vector<std::pair<std::string, std::string>> filters;
    std::vector<SDL_DialogFileFilter> native_filters;
};
}
