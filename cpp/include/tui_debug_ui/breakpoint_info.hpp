#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace tui_debug_ui {

struct BreakpointInfo {
    int line = 0;
    std::string condition;
    std::string hit_condition;
    std::uint64_t hit_count = 0;
};

using BreakpointsByPath = std::unordered_map<std::string, std::unordered_map<int, BreakpointInfo>>;

}  // namespace tui_debug_ui
