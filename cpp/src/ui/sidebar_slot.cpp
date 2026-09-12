#include "tui_debug_ui/sidebar_slot.hpp"

#include "tui_debug_ui/breakpoints_panel.hpp"
#include "tui_debug_ui/disassembly_panel.hpp"
#include "tui_debug_ui/memory_panel.hpp"
#include "tui_debug_ui/runtime_source_panel.hpp"
#include "tui_debug_ui/scopes_panel.hpp"
#include "tui_debug_ui/shared_widget_host.hpp"
#include "tui_debug_ui/stacks_panel.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"
#include "tui_debug_ui/watches_panel.hpp"

namespace tui_debug_ui {

SidebarSlot::SidebarSlot() = default;
SidebarSlot::~SidebarSlot() = default;
SidebarSlot::SidebarSlot(SidebarSlot&&) noexcept = default;
SidebarSlot& SidebarSlot::operator=(SidebarSlot&&) noexcept = default;

}  // namespace tui_debug_ui
