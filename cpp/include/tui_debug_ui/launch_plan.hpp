#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct LaunchRequest {
    std::filesystem::path target;
    std::vector<std::string> args;
    std::optional<std::string> profile;
    std::optional<std::string> adapter;
    std::optional<std::filesystem::path> binary;
    std::optional<std::filesystem::path> workspace;
};

struct LaunchUiSettings {
    bool is_rr_backend = false;
    bool line_buffered_console = false;
    bool lldb_goto_line_fallback = false;
    bool assume_function_breakpoints = false;
    bool show_reverse_continue_hint = false;
    std::string adapter_label;
    std::string profile;
};

struct LaunchPlan {
    std::string program_path;
    std::string target_path;
    std::string resolved_json;
    LaunchUiSettings ui;
    std::optional<std::filesystem::path> workspace;
    std::optional<std::string> display_source;
};

/// Resolve a launch profile via the Rust core (`tui_debug_resolve_launch`).
[[nodiscard]] std::optional<LaunchPlan> resolve_launch_plan(const std::filesystem::path& config_path,
                                                            const LaunchRequest& request,
                                                            std::string& error_out);

}  // namespace tui_debug_ui
