#pragma once

#include <filesystem>

namespace tui_debug_ui {

/// Returns the tui-debug config directory, e.g. `~/.config/tui-debug`.
[[nodiscard]] std::filesystem::path config_directory();

/// Expands a leading `~/` in `path` using `$HOME`.
[[nodiscard]] std::filesystem::path expand_user_path(std::filesystem::path path);

/// Resolves `path` relative to `base` when not absolute.
[[nodiscard]] std::filesystem::path resolve_config_path(const std::filesystem::path& base,
                                                        const std::filesystem::path& path);

}  // namespace tui_debug_ui
