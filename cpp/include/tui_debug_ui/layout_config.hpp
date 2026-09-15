#pragma once

#include "tui_debug_ui/panel_slot.hpp"
#include "tui_debug_ui/sidebar_slot.hpp"
#include "tui_debug_ui/yaml_config.hpp"

#include <optional>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct LayoutPanelEntry {
    SidebarPanelType type = SidebarPanelType::Variables;
    std::optional<std::string> scope_filter;
    std::optional<BreakpointRowKind> breakpoint_filter;
    std::optional<ThreadPanelFilter> thread_filter;
    std::optional<std::int64_t> thread_id_filter;
    std::optional<std::string> thread_name_filter;
    std::optional<std::string> tab_label;
};

struct LayoutDockSpec {
    std::vector<LayoutPanelEntry> panels;
};

struct LayoutSpec {
    std::optional<std::uint16_t> sidebar_pct;
    std::optional<std::uint16_t> bottom_pct;
    std::optional<LayoutDockSpec> left;
    std::optional<LayoutDockSpec> center;
    std::optional<LayoutDockSpec> bottom;
};

[[nodiscard]] LayoutSpec parse_layout_spec(const YamlNode& root);
[[nodiscard]] std::optional<SidebarPanelType> panel_type_from_config_name(const std::string& name);
[[nodiscard]] LayoutPanelEntry parse_layout_panel_entry(const YamlNode& node);
void init_slots_from_layout_spec(std::vector<SidebarSlot>& slots, const LayoutDockSpec& spec,
                                 std::uint64_t& next_slot_id);

}  // namespace tui_debug_ui
