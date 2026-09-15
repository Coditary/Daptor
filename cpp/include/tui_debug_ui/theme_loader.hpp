#pragma once

#include "tui_debug_ui/app_config.hpp"
#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/syntax_theme.hpp"

#include <filesystem>

namespace tui_debug_ui {

struct LoadedTheme {
    DapUiTheme ui;
    SyntaxTheme syntax;
    std::filesystem::path theme_path;
    bool loaded_from_file = false;
};

/// Resolves and loads the theme file referenced by config / env / defaults.
[[nodiscard]] LoadedTheme load_application_theme(const AppConfig& config);

/// Loads a theme JSON file. Missing keys keep built-in defaults.
[[nodiscard]] LoadedTheme load_theme_file(const std::filesystem::path& path);

}  // namespace tui_debug_ui
