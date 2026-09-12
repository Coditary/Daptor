#pragma once

#include "tui_debug_ui/debug_ui_model.hpp"
#include "tui_debug_ui/panel_slot.hpp"

#include <memory>
#include <string>
#include <vector>

namespace tui_debug_ui {

class ScopesPanel;
class WatchesPanel;
class StacksPanel;
class BreakpointsPanel;
class SharedWidgetHost;

struct SidebarSlot {
    PanelSlotConfig config;
    std::unique_ptr<ScopesPanel> scopes;
    std::unique_ptr<WatchesPanel> watches;
    std::unique_ptr<StacksPanel> stacks;
    std::unique_ptr<BreakpointsPanel> breakpoints;
    std::unique_ptr<SharedWidgetHost> shared_host;
    std::vector<std::string> cached_scope_rows;
    std::vector<ScopeVariableRowMeta> cached_scope_row_meta;
    std::vector<WatchEntry> watches_data;
    bool tab_label_customized = false;

    SidebarSlot();
    ~SidebarSlot();
    SidebarSlot(SidebarSlot&&) noexcept;
    SidebarSlot& operator=(SidebarSlot&&) noexcept;
    SidebarSlot(const SidebarSlot&) = delete;
    SidebarSlot& operator=(const SidebarSlot&) = delete;
};

}  // namespace tui_debug_ui
