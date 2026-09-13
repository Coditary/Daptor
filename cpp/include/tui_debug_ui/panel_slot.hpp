#pragma once

#include "tui_debug_ui/breakpoints_panel.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tui_debug_ui {

enum class SidebarPanelType {
    Variables,
    Watches,
    Threads,
    Breakpoints,
    Source,
    Memory,
    DisassemblyAsm,
    DisassemblyBytes,
    RuntimeSource,
    Repl,
    Console,
};

enum class DisassemblyViewMode {
    Asm,
    Bytes,
};

/// Default pane regions in the initial L-layout. User splits add untagged leaves.
enum class PanelDock {
    Left,
    Center,
    Bottom,
};

[[nodiscard]] std::string panel_dock_label(PanelDock dock);

enum class ThreadPanelFilter {
    Stopped,
    Running,
};

struct PanelSlotConfig {
    std::uint64_t id = 0;
    SidebarPanelType type = SidebarPanelType::Variables;
    /// When set, only this DAP scope name is shown (Variables / Watches context).
    std::optional<std::string> scope_filter;
    /// When set, only this breakpoint kind is shown (Breakpoints context).
    std::optional<BreakpointRowKind> breakpoint_filter;
    /// When set, only stopped or running threads are shown (Threads context).
    std::optional<ThreadPanelFilter> thread_filter;
    /// When set, only this thread id is shown (Threads context).
    std::optional<std::int64_t> thread_id_filter;
    /// Display name for `thread_id_filter` tab labels.
    std::optional<std::string> thread_name_filter;
    std::string tab_label;
};

[[nodiscard]] std::string panel_type_label(SidebarPanelType type);
[[nodiscard]] std::string breakpoint_kind_label(BreakpointRowKind kind);
[[nodiscard]] std::string thread_filter_label(ThreadPanelFilter filter);
[[nodiscard]] std::string make_panel_tab_label(const PanelSlotConfig& config,
                                               const std::vector<PanelSlotConfig>& existing,
                                               const std::string& preferred_base = {});

}  // namespace tui_debug_ui
