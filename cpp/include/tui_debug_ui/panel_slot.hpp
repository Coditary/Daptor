#pragma once

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
};

enum class BottomPanelType {
    Repl,
    Console,
};

enum class SourcePanelType {
    Source,
};

struct PanelSlotConfig {
    std::uint64_t id = 0;
    SidebarPanelType type = SidebarPanelType::Variables;
    /// When set, only this DAP scope name is shown (Variables / Watches context).
    std::optional<std::string> scope_filter;
    std::string tab_label;
};

[[nodiscard]] std::string panel_type_label(SidebarPanelType type);
[[nodiscard]] std::string panel_type_label(BottomPanelType type);
[[nodiscard]] std::string panel_type_label(SourcePanelType type);
[[nodiscard]] std::string make_panel_tab_label(SidebarPanelType type, const std::optional<std::string>& scope_filter,
                                               const std::vector<PanelSlotConfig>& existing);

}  // namespace tui_debug_ui
