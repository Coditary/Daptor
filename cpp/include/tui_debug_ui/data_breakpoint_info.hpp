#pragma once

#include <string>
#include <vector>

namespace tui_debug_ui {

struct DataBreakpointEntry {
    std::string data_id;
    std::string variable_name;
    std::string description;
    std::string access_type;
    std::string condition;
    bool verified = true;
};

using DataBreakpoints = std::vector<DataBreakpointEntry>;

}  // namespace tui_debug_ui
