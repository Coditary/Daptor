#include "tui_debug_ui/launch_plan.hpp"

extern "C" {
#include "tui_debug.h"
}

#include <sstream>

namespace tui_debug_ui {
namespace {

std::string escape_json_string(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            escaped.push_back('\\');
        }
        escaped.push_back(ch);
    }
    return escaped;
}

std::string build_request_json(const LaunchRequest& request) {
    std::ostringstream json;
    json << "{\"target\":\"" << escape_json_string(request.target.string()) << "\"";
    if (!request.args.empty()) {
        json << ",\"args\":[";
        bool first = true;
        for (const std::string& arg : request.args) {
            if (!first) {
                json << ',';
            }
            first = false;
            json << '"' << escape_json_string(arg) << '"';
        }
        json << ']';
    }
    if (request.profile.has_value()) {
        json << ",\"profile\":\"" << escape_json_string(*request.profile) << '"';
    }
    if (request.adapter.has_value()) {
        json << ",\"adapter\":\"" << escape_json_string(*request.adapter) << '"';
    }
    if (request.binary.has_value()) {
        json << ",\"binary\":\"" << escape_json_string(request.binary->string()) << '"';
    }
    if (request.workspace.has_value()) {
        json << ",\"workspace\":\"" << escape_json_string(request.workspace->string()) << '"';
    }
    json << '}';
    return json.str();
}

std::optional<std::string> json_string_field(const std::string& json, const std::string& key) {
    const std::string needle = '"' + key + "\":\"";
    const std::size_t start = json.find(needle);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    std::size_t cursor = start + needle.size();
    std::string value;
    while (cursor < json.size()) {
        const char ch = json[cursor++];
        if (ch == '"') {
            return value;
        }
        if (ch == '\\' && cursor < json.size()) {
            value.push_back(json[cursor++]);
            continue;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

bool json_bool_field(const std::string& json, const std::string& key) {
    const std::string true_needle = '"' + key + "\":true";
    const std::string false_needle = '"' + key + "\":false";
    const std::size_t true_pos = json.find(true_needle);
    const std::size_t false_pos = json.find(false_needle);
    if (true_pos == std::string::npos) {
        return false;
    }
    if (false_pos == std::string::npos) {
        return true;
    }
    return true_pos < false_pos;
}

LaunchUiSettings parse_ui_settings(const std::string& resolved_json) {
    LaunchUiSettings ui{};
    if (const std::optional<std::string> backend = json_string_field(resolved_json, "backend");
        backend.has_value() && *backend == "rr") {
        ui.is_rr_backend = true;
    }
    ui.adapter_label = json_string_field(resolved_json, "adapter_label").value_or("debug adapter");
    ui.profile = json_string_field(resolved_json, "profile").value_or("");
    ui.line_buffered_console = json_bool_field(resolved_json, "line_buffered_console");
    ui.lldb_goto_line_fallback = json_bool_field(resolved_json, "lldb_goto_line_fallback");
    ui.assume_function_breakpoints = json_bool_field(resolved_json, "assume_function_breakpoints");
    ui.show_reverse_continue_hint = json_bool_field(resolved_json, "show_reverse_continue_hint");
    return ui;
}

}  // namespace

std::optional<LaunchPlan> resolve_launch_plan(const std::filesystem::path& config_path,
                                              const LaunchRequest& request,
                                              std::string& error_out) {
    const std::string request_json = build_request_json(request);
    std::vector<char> buffer(16 * 1024);
    const int status = tui_debug_resolve_launch(config_path.c_str(), request_json.c_str(), buffer.data(),
                                                buffer.size());
    if (status != 0) {
        const char* message = tui_debug_last_error();
        error_out = message != nullptr ? message : "failed to resolve launch profile";
        return std::nullopt;
    }

    const std::string resolved_json(buffer.data());
    LaunchPlan plan{};
    plan.resolved_json = resolved_json;
    plan.target_path = json_string_field(resolved_json, "target").value_or(request.target.string());
    plan.program_path = json_string_field(resolved_json, "program").value_or(plan.target_path);
    plan.ui = parse_ui_settings(resolved_json);

    if (const std::optional<std::string> workspace = json_string_field(resolved_json, "workspace");
        workspace.has_value() && !workspace->empty()) {
        plan.workspace = std::filesystem::path(*workspace);
    }
    if (const std::optional<std::string> display_source = json_string_field(resolved_json, "display_source");
        display_source.has_value() && !display_source->empty()) {
        plan.display_source = display_source;
    }

    return plan;
}

}  // namespace tui_debug_ui
