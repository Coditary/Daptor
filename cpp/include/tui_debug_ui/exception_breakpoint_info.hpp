#pragma once

#include <string>
#include <vector>

namespace tui_debug_ui {

struct ExceptionBreakpointFilterInfo {
    std::string filter;
    std::string label;
    std::string description;
    bool default_enabled = false;
    bool supports_condition = false;
};

using ExceptionBreakpointFilters = std::vector<ExceptionBreakpointFilterInfo>;

}  // namespace tui_debug_ui
