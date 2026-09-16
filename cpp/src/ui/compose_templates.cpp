#include "tui_debug_ui/compose_templates.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define DAPTOR_UI_HAS_NLOHMANN_JSON 1
#endif

namespace tui_debug_ui {
namespace {

#ifdef DAPTOR_UI_HAS_NLOHMANN_JSON
using Json = nlohmann::json;

NetworkComposeTemplate parse_compose_template(const Json& entry) {
    return {
        .name = entry.value("name", std::string{"Request"}),
        .method = entry.value("method", std::string{"GET"}),
        .url = entry.value("url", std::string{}),
        .headers = entry.value("headers", std::string{}),
        .body = entry.value("body", std::string{}),
        .timeout_ms = entry.value("timeout_ms", 30000),
    };
}

Json compose_template_to_json(const NetworkComposeTemplate& item) {
    return Json{
        {"name", item.name},
        {"method", item.method},
        {"url", item.url},
        {"headers", item.headers},
        {"body", item.body},
        {"timeout_ms", item.timeout_ms},
    };
}
#endif

std::filesystem::path path_from_env() {
    const char* env = std::getenv("DAPTOR_COMPOSE_TEMPLATES");
    if (env == nullptr || env[0] == '\0') {
        return {};
    }
    return std::filesystem::path(env);
}

}  // namespace

std::filesystem::path compose_templates_path(const std::filesystem::path& workspace_root) {
    if (const std::filesystem::path env_path = path_from_env(); !env_path.empty()) {
        return env_path;
    }
    return workspace_root / ".daptor" / "compose-templates.json";
}

std::vector<NetworkComposeTemplate> default_compose_templates() {
    return {
        {
            .name = "Custom request",
            .method = "GET",
            .url = "http://localhost:8080/",
            .headers = "Accept: application/json",
            .body = "",
            .timeout_ms = 30000,
        },
    };
}

ComposeTemplatesLoadResult load_compose_templates(const std::filesystem::path& workspace_root) {
    ComposeTemplatesLoadResult result{
        .templates = default_compose_templates(),
        .path = compose_templates_path(workspace_root),
        .loaded_from_file = false,
    };

#ifdef DAPTOR_UI_HAS_NLOHMANN_JSON
    if (result.path.empty()) {
        return result;
    }

    std::ifstream input(result.path);
    if (!input.is_open()) {
        return result;
    }

    try {
        const Json root = Json::parse(input);
        if (!root.is_array()) {
            return result;
        }

        std::vector<NetworkComposeTemplate> templates;
        templates.reserve(root.size());
        for (const Json& entry : root) {
            if (!entry.is_object()) {
                continue;
            }
            templates.push_back(parse_compose_template(entry));
        }
        if (templates.empty()) {
            return result;
        }

        result.templates = std::move(templates);
        result.loaded_from_file = true;
        return result;
    } catch (const Json::exception&) {
        return result;
    }
#else
    (void)workspace_root;
    return result;
#endif
}

bool save_compose_templates(const std::filesystem::path& path,
                            const std::vector<NetworkComposeTemplate>& templates,
                            std::string& error_out) {
#ifdef DAPTOR_UI_HAS_NLOHMANN_JSON
    if (path.empty()) {
        error_out = "compose templates path is empty";
        return false;
    }

    try {
        const std::filesystem::path parent = path.parent_path();
        if (!parent.empty()) {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        Json root = Json::array();
        for (const NetworkComposeTemplate& item : templates) {
            root.push_back(compose_template_to_json(item));
        }

        std::ofstream output(path);
        if (!output.is_open()) {
            error_out = "failed to open " + path.string();
            return false;
        }
        output << root.dump(2) << '\n';
        return true;
    } catch (const Json::exception& ex) {
        error_out = ex.what();
        return false;
    }
#else
    (void)path;
    (void)templates;
    error_out = "JSON support is not available";
    return false;
#endif
}

int parse_compose_timeout_ms(const std::string& text, int default_ms) {
    if (text.empty()) {
        return default_ms;
    }
    try {
        const long value = std::stol(text);
        if (value < 1000) {
            return 1000;
        }
        if (value > 600000) {
            return 600000;
        }
        return static_cast<int>(value);
    } catch (...) {
        return default_ms;
    }
}

}  // namespace tui_debug_ui
