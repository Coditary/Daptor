#include "tui_debug_ui/xdg_paths.hpp"

#include <cstdlib>
#include <unistd.h>

namespace tui_debug_ui {
namespace {

std::filesystem::path home_directory() {
    const char* home = std::getenv("HOME");
    if (home != nullptr && home[0] != '\0') {
        return std::filesystem::path(home);
    }
    return std::filesystem::current_path();
}

}  // namespace

std::filesystem::path config_directory() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg != nullptr && xdg[0] != '\0') {
        return std::filesystem::path(xdg) / "daptor";
    }
    return home_directory() / ".config" / "daptor";
}

std::filesystem::path expand_user_path(std::filesystem::path path) {
    const std::string raw = path.string();
    if (raw.size() >= 2 && raw[0] == '~' && raw[1] == '/') {
        return home_directory() / raw.substr(2);
    }
    return path;
}

std::filesystem::path resolve_config_path(const std::filesystem::path& base,
                                          const std::filesystem::path& path) {
    if (path.empty()) {
        return path;
    }
    const std::filesystem::path expanded = expand_user_path(path);
    if (expanded.is_absolute()) {
        return expanded;
    }
    return base / expanded;
}

}  // namespace tui_debug_ui
