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

std::string breakpoint_kind_label(BreakpointRowKind kind) {
    switch (kind) {
    case BreakpointRowKind::Source:
        return "Source";
    case BreakpointRowKind::Data:
        return "Data";
    case BreakpointRowKind::Function:
        return "Function";
    case BreakpointRowKind::Exception:
        return "Exception";
    }
    return "Breakpoint";
}

std::string thread_filter_label(ThreadPanelFilter filter) {
    switch (filter) {
    case ThreadPanelFilter::Stopped:
        return "Stopped";
    case ThreadPanelFilter::Running:
        return "Running";
    }
    return "Thread";
}

std::string make_panel_tab_label(const PanelSlotConfig& config, const std::vector<PanelSlotConfig>& existing) {
    std::string base = panel_type_label(config.type);
    if (config.scope_filter.has_value() && !config.scope_filter->empty()) {
        base += " · " + *config.scope_filter;
    }
    if (config.breakpoint_filter.has_value()) {
        base += " · " + breakpoint_kind_label(*config.breakpoint_filter);
    }
    if (config.thread_id_filter.has_value()) {
        if (config.thread_name_filter.has_value() && !config.thread_name_filter->empty()) {
            base += " · " + *config.thread_name_filter;
        } else {
            base += " · Thread " + std::to_string(*config.thread_id_filter);
        }
    } else if (config.thread_filter.has_value()) {
        base += " · " + thread_filter_label(*config.thread_filter);
    }

    int same = 0;
    for (const PanelSlotConfig& slot : existing) {
        if (slot.type == config.type && slot.scope_filter == config.scope_filter &&
            slot.breakpoint_filter == config.breakpoint_filter && slot.thread_filter == config.thread_filter &&
            slot.thread_id_filter == config.thread_id_filter) {
            ++same;
        }
    }
    if (same > 0) {
        base += " (" + std::to_string(same + 1) + ")";
    }
    return base;
}

}  // namespace tui_debug_ui
