#pragma once

#include <string>
#include <unordered_map>

namespace tui_debug_ui {

struct BreakpointInfo {
    int line = 0;
    std::string condition;
};

using BreakpointsByPath = std::unordered_map<std::string, std::unordered_map<int, BreakpointInfo>>;

}  // namespace tui_debug_ui
