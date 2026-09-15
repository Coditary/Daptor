#include "tui_debug_ui/layout_config.hpp"

#include "tui_debug_ui/sidebar_slot.hpp"

#include <algorithm>
#include <cctype>

namespace tui_debug_ui {
namespace {

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

std::optional<BreakpointRowKind> breakpoint_kind_from_name(const std::string& name) {
    const std::string normalized = lowercase_copy(name);
    if (normalized == "source") {
        return BreakpointRowKind::Source;
    }
    if (normalized == "function") {
        return BreakpointRowKind::Function;
    }
    if (normalized == "data") {
        return BreakpointRowKind::Data;
    }
    if (normalized == "exception") {
        return BreakpointRowKind::Exception;
    }
    return std::nullopt;
}

std::optional<ThreadPanelFilter> thread_filter_from_name(const std::string& name) {
    const std::string normalized = lowercase_copy(name);
    if (normalized == "stopped") {
        return ThreadPanelFilter::Stopped;
    }
    if (normalized == "running") {
        return ThreadPanelFilter::Running;
    }
    return std::nullopt;
}

LayoutDockSpec parse_dock_spec(const YamlNode& node) {
    LayoutDockSpec spec;
    if (!node.is_sequence()) {
        return spec;
    }
    for (const YamlNode& entry : node.sequence) {
        spec.panels.push_back(parse_layout_panel_entry(entry));
    }
    return spec;
}

}  // namespace

std::optional<SidebarPanelType> panel_type_from_config_name(const std::string& name) {
    const std::string normalized = lowercase_copy(name);
    if (normalized == "variables" || normalized == "scopes" || normalized == "scope") {
        return SidebarPanelType::Variables;
    }
    if (normalized == "watches" || normalized == "watch") {
        return SidebarPanelType::Watches;
    }
    if (normalized == "threads" || normalized == "stacks" || normalized == "stack") {
        return SidebarPanelType::Threads;
    }
    if (normalized == "breakpoints" || normalized == "breakpoint") {
        return SidebarPanelType::Breakpoints;
    }
    if (normalized == "source") {
        return SidebarPanelType::Source;
    }
    if (normalized == "memory") {
        return SidebarPanelType::Memory;
    }
    if (normalized == "disassembly" || normalized == "disassembly_asm" || normalized == "asm") {
        return SidebarPanelType::DisassemblyAsm;
    }
    if (normalized == "disassembly_bytes" || normalized == "bytes") {
        return SidebarPanelType::DisassemblyBytes;
    }
    if (normalized == "runtime_source") {
        return SidebarPanelType::RuntimeSource;
    }
    if (normalized == "file_tree" || normalized == "files") {
        return SidebarPanelType::FileTree;
    }
    if (normalized == "resources") {
        return SidebarPanelType::Resources;
    }
    if (normalized == "network") {
        return SidebarPanelType::Network;
    }
    if (normalized == "repl") {
        return SidebarPanelType::Repl;
    }
    if (normalized == "console") {
        return SidebarPanelType::Console;
    }
    return std::nullopt;
}

LayoutPanelEntry parse_layout_panel_entry(const YamlNode& node) {
    LayoutPanelEntry entry;
    if (node.is_scalar()) {
        if (const std::optional<SidebarPanelType> type = panel_type_from_config_name(node.scalar); type.has_value()) {
            entry.type = *type;
        }
        return entry;
    }
    if (!node.is_mapping()) {
        return entry;
    }

    if (const YamlNode* panel = node.get("panel"); panel != nullptr) {
        if (const std::optional<std::string> panel_name = panel->as_string(); panel_name.has_value()) {
            if (const std::optional<SidebarPanelType> type = panel_type_from_config_name(*panel_name); type.has_value()) {
                entry.type = *type;
            }
        }
    }
    if (const YamlNode* scope = node.get("scope"); scope != nullptr) {
        if (const std::optional<std::string> value = scope->as_string(); value.has_value()) {
            entry.scope_filter = *value;
        }
    }
    if (const YamlNode* breakpoint = node.get("breakpoint"); breakpoint != nullptr) {
        if (const std::optional<std::string> value = breakpoint->as_string(); value.has_value()) {
            entry.breakpoint_filter = breakpoint_kind_from_name(*value);
        }
    }
    if (const YamlNode* thread_filter = node.get("thread_filter"); thread_filter != nullptr) {
        if (const std::optional<std::string> value = thread_filter->as_string(); value.has_value()) {
            entry.thread_filter = thread_filter_from_name(*value);
        }
    }
    if (const YamlNode* thread_id = node.get("thread_id"); thread_id != nullptr) {
        if (const std::optional<int> value = thread_id->as_int(); value.has_value()) {
            entry.thread_id_filter = *value;
        }
    }
    if (const YamlNode* thread_name = node.get("thread_name"); thread_name != nullptr) {
        if (const std::optional<std::string> value = thread_name->as_string(); value.has_value()) {
            entry.thread_name_filter = *value;
        }
    }
    if (const YamlNode* label = node.get("label"); label != nullptr) {
        if (const std::optional<std::string> value = label->as_string(); value.has_value()) {
            entry.tab_label = *value;
        }
    }
    return entry;
}

LayoutSpec parse_layout_spec(const YamlNode& root) {
    LayoutSpec spec;
    const YamlNode* layout = root.get("layout");
    if (layout == nullptr) {
        return spec;
    }

    if (const YamlNode* sidebar_pct = layout->get("sidebar_pct"); sidebar_pct != nullptr) {
        if (const std::optional<int> value = sidebar_pct->as_int(); value.has_value()) {
            spec.sidebar_pct = static_cast<std::uint16_t>(std::clamp(*value, 1, 99));
        }
    }
    if (const YamlNode* bottom_pct = layout->get("bottom_pct"); bottom_pct != nullptr) {
        if (const std::optional<int> value = bottom_pct->as_int(); value.has_value()) {
            spec.bottom_pct = static_cast<std::uint16_t>(std::clamp(*value, 1, 99));
        }
    }

    const YamlNode* docks = layout->get("docks");
    if (docks == nullptr) {
        return spec;
    }

    if (const YamlNode* left = docks->get("left"); left != nullptr) {
        spec.left = parse_dock_spec(*left);
    }
    if (const YamlNode* center = docks->get("center"); center != nullptr) {
        spec.center = parse_dock_spec(*center);
    }
    if (const YamlNode* bottom = docks->get("bottom"); bottom != nullptr) {
        spec.bottom = parse_dock_spec(*bottom);
    }
    return spec;
}

void init_slots_from_layout_spec(std::vector<SidebarSlot>& slots, const LayoutDockSpec& spec,
                                 std::uint64_t& next_slot_id) {
    for (const LayoutPanelEntry& entry : spec.panels) {
        PanelSlotConfig config;
        config.id = next_slot_id++;
        config.type = entry.type;
        config.scope_filter = entry.scope_filter;
        config.breakpoint_filter = entry.breakpoint_filter;
        config.thread_filter = entry.thread_filter;
        config.thread_id_filter = entry.thread_id_filter;
        config.thread_name_filter = entry.thread_name_filter;

        std::vector<PanelSlotConfig> existing;
        for (const SidebarSlot& slot : slots) {
            existing.push_back(slot.config);
        }
        config.tab_label = entry.tab_label.has_value() ? *entry.tab_label
                                                       : make_panel_tab_label(config, existing);

        SidebarSlot slot;
        slot.config = std::move(config);
        slots.push_back(std::move(slot));
    }
}

}  // namespace tui_debug_ui
