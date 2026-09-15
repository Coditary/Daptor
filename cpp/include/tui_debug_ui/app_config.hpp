#pragma once

#include "tui_debug_ui/keybindings.hpp"
#include "tui_debug_ui/layout_config.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace tui_debug_ui {

struct AppPaths {
    std::optional<std::filesystem::path> tree_sitter_dir;
};

struct AppConfig {
    std::filesystem::path config_path;
    std::filesystem::path config_directory;
    std::optional<std::filesystem::path> theme_file;
    AppPaths paths;
    LayoutSpec layout;
    KeyBindings keybindings = default_keybindings();
    bool loaded_from_file = false;
};

/// Loads `config.yaml` from the XDG config directory or `TUI_DEBUG_CONFIG`.
[[nodiscard]] AppConfig load_app_config();

}  // namespace tui_debug_ui
