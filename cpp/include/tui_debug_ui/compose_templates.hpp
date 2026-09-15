#pragma once

#include "tui_debug_ui/network_mock_data.hpp"

#include <filesystem>
#include <vector>

namespace tui_debug_ui {

struct ComposeTemplatesLoadResult {
    std::vector<NetworkComposeTemplate> templates;
    std::filesystem::path path;
    bool loaded_from_file = false;
};

[[nodiscard]] std::filesystem::path compose_templates_path(const std::filesystem::path& workspace_root);
[[nodiscard]] std::vector<NetworkComposeTemplate> default_compose_templates();
[[nodiscard]] ComposeTemplatesLoadResult load_compose_templates(const std::filesystem::path& workspace_root);
[[nodiscard]] bool save_compose_templates(const std::filesystem::path& path,
                                          const std::vector<NetworkComposeTemplate>& templates,
                                          std::string& error_out);

}  // namespace tui_debug_ui
