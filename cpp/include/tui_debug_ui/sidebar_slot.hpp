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
class MemoryPanel;
class DisassemblyPanel;
class RuntimeSourcePanel;
class FileTreePanel;
class SharedWidgetHost;

struct SidebarSlot {
    PanelSlotConfig config;
    std::unique_ptr<ScopesPanel> scopes;
    std::unique_ptr<WatchesPanel> watches;
    std::unique_ptr<StacksPanel> stacks;
    std::unique_ptr<BreakpointsPanel> breakpoints;
    std::unique_ptr<MemoryPanel> memory;
    std::unique_ptr<DisassemblyPanel> disassembly;
    std::unique_ptr<RuntimeSourcePanel> runtime_source;
    std::unique_ptr<FileTreePanel> file_tree;
    std::unique_ptr<SharedWidgetHost> shared_host;
    std::vector<std::string> cached_scope_rows;
    std::vector<ScopeVariableRowMeta> cached_scope_row_meta;
    std::vector<WatchEntry> watches_data;
    std::vector<std::string> cached_memory_lines;
    std::vector<std::string> cached_disassembly_lines;
    std::vector<std::string> cached_runtime_source_lines;
    std::string cached_memory_reference;
    std::string cached_memory_hex_data;
    std::string cached_memory_response_address;
    std::string memory_view_reference;
    std::string memory_search_query;
    std::vector<std::size_t> memory_search_matches;
    int memory_search_match_index = -1;
    std::int64_t cached_memory_read_offset = 0;
    std::int64_t cached_runtime_source_reference = 0;
    bool tab_label_customized = false;

    SidebarSlot();
    ~SidebarSlot();
    SidebarSlot(SidebarSlot&&) noexcept;
    SidebarSlot& operator=(SidebarSlot&&) noexcept;
    SidebarSlot(const SidebarSlot&) = delete;
    SidebarSlot& operator=(const SidebarSlot&) = delete;
};

}  // namespace tui_debug_ui
