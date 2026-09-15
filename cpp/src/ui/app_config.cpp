#include "tui_debug_ui/app_config.hpp"

#include "tui_debug_ui/xdg_paths.hpp"
#include "tui_debug_ui/yaml_config.hpp"

#include <cstdlib>
#include <fstream>
#include <unordered_map>

namespace tui_debug_ui {
namespace {

std::filesystem::path config_path_from_env() {
    const char* env = std::getenv("TUI_DEBUG_CONFIG");
    if (env == nullptr || env[0] == '\0') {
        return {};
    }
    return expand_user_path(std::filesystem::path(env));
}

std::unordered_map<std::string, std::string> mapping_to_strings(const YamlNode& node) {
    std::unordered_map<std::string, std::string> values;
    if (!node.is_mapping()) {
        return values;
    }
    for (const auto& [key, value] : node.mapping) {
        if (const std::optional<std::string> scalar = value.as_string(); scalar.has_value()) {
            values[key] = *scalar;
        }
    }
    return values;
}

void parse_paths(const YamlNode& root, AppPaths& paths, const std::filesystem::path& config_directory) {
    const YamlNode* section = root.get("paths");
    if (section == nullptr) {
        return;
    }
    if (const YamlNode* tree_sitter = section->get("tree_sitter_dir"); tree_sitter != nullptr) {
        if (const std::optional<std::string> value = tree_sitter->as_string(); value.has_value()) {
            paths.tree_sitter_dir = resolve_config_path(config_directory, std::filesystem::path(*value));
        }
    }
}

void parse_theme(const YamlNode& root, AppConfig& config) {
    const YamlNode* section = root.get("theme");
    if (section == nullptr) {
        config.theme_file = config.config_directory / "theme.json";
        return;
    }
    if (const YamlNode* file = section->get("file"); file != nullptr) {
        if (const std::optional<std::string> value = file->as_string(); value.has_value()) {
            config.theme_file = resolve_config_path(config.config_directory, std::filesystem::path(*value));
            return;
        }
    }
    config.theme_file = config.config_directory / "theme.json";
}

}  // namespace

AppConfig load_app_config() {
    AppConfig config{
        .config_path = config_directory() / "config.yaml",
        .config_directory = config_directory(),
        .theme_file = std::nullopt,
        .paths = {},
        .layout = {},
        .keybindings = default_keybindings(),
        .loaded_from_file = false,
    };

    if (const std::filesystem::path env_path = config_path_from_env(); !env_path.empty()) {
        config.config_path = env_path;
        if (env_path.has_parent_path()) {
            config.config_directory = env_path.parent_path();
        }
    }

    std::ifstream input(config.config_path);
    if (!input.is_open()) {
        config.theme_file = config.config_directory / "theme.json";
        return config;
    }

    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const YamlNode root = parse_yaml(content);
    config.loaded_from_file = true;

    parse_theme(root, config);
    parse_paths(root, config.paths, config.config_directory);
    config.layout = parse_layout_spec(root);

    if (const YamlNode* keybindings = root.get("keybindings"); keybindings != nullptr) {
        config.keybindings = parse_keybindings(mapping_to_strings(*keybindings));
    }

    return config;
}

}  // namespace tui_debug_ui
