#include "tui_debug_ui/panel_slot.hpp"

#include <algorithm>

namespace tui_debug_ui {

std::string panel_type_label(SidebarPanelType type) {
    switch (type) {
    case SidebarPanelType::Variables:
        return "Variables";
    case SidebarPanelType::Watches:
        return "Watches";
    case SidebarPanelType::Threads:
        return "Threads";
    case SidebarPanelType::Breakpoints:
        return "Breakpoints";
    }
    return "Panel";
}

std::string panel_type_label(BottomPanelType type) {
    switch (type) {
    case BottomPanelType::Repl:
        return "REPL";
    case BottomPanelType::Console:
        return "Console";
    }
    return "Panel";
}

std::string panel_type_label(SourcePanelType type) {
    switch (type) {
    case SourcePanelType::Source:
        return "Source";
    }
    return "Panel";
}

std::string make_panel_tab_label(SidebarPanelType type, const std::optional<std::string>& scope_filter,
                                 const std::vector<PanelSlotConfig>& existing) {
    std::string base = panel_type_label(type);
    if (scope_filter.has_value() && !scope_filter->empty()) {
        base += " · " + *scope_filter;
    }

    int same = 0;
    for (const PanelSlotConfig& slot : existing) {
        if (slot.type == type && slot.scope_filter == scope_filter) {
            ++same;
        }
    }
    if (same > 0) {
        base += " (" + std::to_string(same + 1) + ")";
    }
    return base;
}

}  // namespace tui_debug_ui
