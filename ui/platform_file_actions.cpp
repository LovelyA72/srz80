#include "platform_file_actions.hpp"

#include <SDL3/SDL.h>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace srz80::ui::platform {
namespace {

#ifndef _WIN32
std::string percent_encode(const std::string &value) {
    std::ostringstream encoded;
    encoded << std::uppercase << std::hex;
    for (const unsigned char character : value) {
        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') || character == '/' ||
            character == '-' || character == '_' || character == '.' || character == '~') {
            encoded << static_cast<char>(character);
        } else {
            encoded << '%' << std::setw(2) << std::setfill('0') << static_cast<unsigned>(character);
        }
    }
    return encoded.str();
}

bool ensure_private_directory(const std::filesystem::path &path, std::string &error) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(path, filesystem_error);
    if (filesystem_error) {
        error = "Cannot create trash directory: " + filesystem_error.message();
        return false;
    }
    if (::chmod(path.c_str(), S_IRWXU) != 0) {
        error = "Cannot secure trash directory";
        return false;
    }
    return true;
}

std::filesystem::path mount_root(const std::filesystem::path &path, dev_t device) {
    auto current = std::filesystem::is_directory(path) ? path : path.parent_path();
    while (current.has_parent_path() && current.parent_path() != current) {
        struct stat parent_status {};
        if (::stat(current.parent_path().c_str(), &parent_status) != 0 || parent_status.st_dev != device)
            break;
        current = current.parent_path();
    }
    return current;
}

std::filesystem::path home_trash_root(std::string &error) {
    if (const char *xdg_data = std::getenv("XDG_DATA_HOME"); xdg_data && *xdg_data)
        return std::filesystem::path(xdg_data) / "Trash";
    if (const char *home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".local/share/Trash";
    error = "Cannot locate the user trash directory";
    return {};
}

std::filesystem::path trash_root_for(const std::filesystem::path &path, bool &path_is_relative,
                                     std::filesystem::path &top_directory, std::string &error) {
    struct stat target_status {};
    if (::lstat(path.c_str(), &target_status) != 0) {
        error = "Cannot inspect item before moving it to trash";
        return {};
    }

    auto home_trash = home_trash_root(error);
    if (home_trash.empty())
        return {};
    if (!ensure_private_directory(home_trash, error) ||
        !ensure_private_directory(home_trash / "files", error) ||
        !ensure_private_directory(home_trash / "info", error))
        return {};

    struct stat home_status {};
    if (::stat(home_trash.c_str(), &home_status) == 0 && home_status.st_dev == target_status.st_dev) {
        path_is_relative = false;
        return home_trash;
    }

    top_directory = mount_root(path, target_status.st_dev);
    const auto shared = top_directory / ".Trash";
    struct stat shared_status {};
    std::filesystem::path root;
    if (::stat(shared.c_str(), &shared_status) == 0 && S_ISDIR(shared_status.st_mode) &&
        (shared_status.st_mode & S_ISVTX)) {
        root = shared / std::to_string(static_cast<unsigned long long>(::geteuid()));
    } else {
        root = top_directory / (".Trash-" +
                                std::to_string(static_cast<unsigned long long>(::geteuid())));
    }
    if (!ensure_private_directory(root, error) ||
        !ensure_private_directory(root / "files", error) ||
        !ensure_private_directory(root / "info", error))
        return {};
    path_is_relative = true;
    return root;
}
#endif

} // namespace

bool reveal_in_file_manager(const std::filesystem::path &path, std::string &error) {
    error.clear();
#ifdef _WIN32
    PIDLIST_ABSOLUTE item = nullptr;
    const HRESULT initialized = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const HRESULT parsed = ::SHParseDisplayName(path.wstring().c_str(), nullptr, &item, 0, nullptr);
    HRESULT revealed = parsed;
    if (SUCCEEDED(parsed)) {
        PIDLIST_ABSOLUTE parent = ::ILCloneFull(item);
        if (parent && ::ILRemoveLastID(parent)) {
            PCUITEMID_CHILD child = ::ILFindLastID(item);
            revealed = ::SHOpenFolderAndSelectItems(parent, 1, &child, 0);
        } else {
            revealed = ::SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        }
        ::CoTaskMemFree(parent);
        ::CoTaskMemFree(item);
    }
    if (SUCCEEDED(initialized))
        ::CoUninitialize();
    if (FAILED(revealed)) {
        error = "Cannot reveal item in File Explorer";
        return false;
    }
    return true;
#else
    std::error_code filesystem_error;
    const auto directory = std::filesystem::is_directory(path, filesystem_error) ? path : path.parent_path();
    const auto url = "file://" + percent_encode(std::filesystem::absolute(directory).string());
    if (!SDL_OpenURL(url.c_str())) {
        error = std::string("Cannot reveal item in file manager: ") + SDL_GetError();
        return false;
    }
    return true;
#endif
}

bool move_to_trash(const std::filesystem::path &path, std::string &error) {
    error.clear();
#ifdef _WIN32
    std::wstring source = path.wstring();
    source.push_back(L'\0');
    SHFILEOPSTRUCTW operation{};
    operation.wFunc = FO_DELETE;
    operation.pFrom = source.c_str();
    operation.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    const int result = ::SHFileOperationW(&operation);
    if (result != 0 || operation.fAnyOperationsAborted) {
        error = operation.fAnyOperationsAborted ? "Moving item to Recycle Bin was cancelled"
                                                : "Cannot move item to Recycle Bin";
        return false;
    }
    return true;
#else
    const auto absolute = std::filesystem::absolute(path).lexically_normal();
    bool relative_path = false;
    std::filesystem::path top_directory;
    const auto trash_root = trash_root_for(absolute, relative_path, top_directory, error);
    if (trash_root.empty())
        return false;

    auto trash_name = absolute.filename().string();
    for (unsigned suffix = 1;
         std::filesystem::exists(trash_root / "files" / trash_name) ||
         std::filesystem::exists(trash_root / "info" / (trash_name + ".trashinfo")); ++suffix)
        trash_name = absolute.filename().string() + "." + std::to_string(suffix);

    const auto info_path = trash_root / "info" / (trash_name + ".trashinfo");
    std::ofstream info(info_path, std::ios::binary | std::ios::trunc);
    if (!info) {
        error = "Cannot create trash metadata";
        return false;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local_time{};
    localtime_r(&now, &local_time);
    const auto recorded_path = relative_path ? absolute.lexically_relative(top_directory) : absolute;
    info << "[Trash Info]\nPath=" << percent_encode(recorded_path.string())
         << "\nDeletionDate=" << std::put_time(&local_time, "%Y-%m-%dT%H:%M:%S") << '\n';
    info.close();
    if (!info) {
        std::filesystem::remove(info_path);
        error = "Cannot write trash metadata";
        return false;
    }

    std::error_code filesystem_error;
    std::filesystem::rename(absolute, trash_root / "files" / trash_name, filesystem_error);
    if (filesystem_error) {
        std::filesystem::remove(info_path);
        error = "Cannot move item to trash: " + filesystem_error.message();
        return false;
    }
    return true;
#endif
}

} // namespace srz80::ui::platform
