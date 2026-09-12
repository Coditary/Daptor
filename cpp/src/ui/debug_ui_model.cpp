#include "tui_debug_ui/debug_ui_model.hpp"
#include "tui_debug_ui/snapshot_parser.hpp"

namespace tui_debug_ui {

void DebugUiModel::apply_snapshot_json(const std::string& json) {
    apply_poll_json(*this, json);
}

std::string DebugUiModel::connection_label() const {
    switch (connection_state) {
    case ConnectionState::Connecting:
        return "connecting";
    case ConnectionState::Connected:
        return "connected";
    case ConnectionState::Failed:
        return "failed";
    }
    return "unknown";
}

void LayoutConfig::widen_sidebar() { sidebar_pct = std::min<std::uint16_t>(45, sidebar_pct + 2); }
void LayoutConfig::narrow_sidebar() { sidebar_pct = std::max<std::uint16_t>(15, sidebar_pct - 2); }
void LayoutConfig::grow_bottom() { bottom_pct = std::min<std::uint16_t>(50, bottom_pct + 2); }
void LayoutConfig::shrink_bottom() { bottom_pct = std::max<std::uint16_t>(15, bottom_pct - 2); }
void LayoutConfig::widen_watches() { watches_pct = std::min<std::uint16_t>(50, watches_pct + 2); }
void LayoutConfig::narrow_watches() { watches_pct = std::max<std::uint16_t>(15, watches_pct - 2); }
void LayoutConfig::widen_repl() { repl_pct = std::min<std::uint16_t>(60, repl_pct + 5); }
void LayoutConfig::narrow_repl() { repl_pct = std::max<std::uint16_t>(20, repl_pct - 5); }
void LayoutConfig::grow_scopes() { scopes_pct = std::min<std::uint16_t>(80, scopes_pct + 3); }
void LayoutConfig::shrink_scopes() { scopes_pct = std::max<std::uint16_t>(30, scopes_pct - 3); }

std::string DebugUiModel::focus_label() const {
    switch (focus) {
    case Focus::Source:
        return "source";
    case Focus::Scopes:
        return "variables";
    case Focus::Breakpoints:
        return "breakpoints";
    case Focus::Stacks:
        return "threads";
    case Focus::Watches:
        return "watches";
    case Focus::Memory:
        return "memory";
    case Focus::Disassembly:
        return "disassembly";
    case Focus::RuntimeSource:
        return "runtime source";
    case Focus::Repl:
        return "repl";
    case Focus::Console:
        return "console";
    }
    return "unknown";
}

}  // namespace tui_debug_ui
