#pragma once

#include <string>
#include <vector>

namespace tui_debug_ui {

struct FunctionBreakpointEntry {
    std::string name;
    std::string path;
    int line = 0;
    std::string condition;
    std::string hit_condition;
    bool verified = true;
};

using FunctionBreakpoints = std::vector<FunctionBreakpointEntry>;

}  // namespace tui_debug_ui
