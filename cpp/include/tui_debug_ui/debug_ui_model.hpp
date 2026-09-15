#pragma once

#include "tui_debug_ui/network_mock_data.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tui_debug_ui {

enum class ConnectionState {
    Connecting,
    Connected,
    Failed,
};

enum class Focus {
    Source,
    Scopes,
    Breakpoints,
    Stacks,
    Watches,
    Memory,
    Disassembly,
    RuntimeSource,
    FileTree,
    Resources,
    Network,
    Repl,
    Console,
};

struct LayoutConfig {
    std::uint16_t sidebar_pct = 25;
    std::uint16_t right_pct = 25;
    std::uint16_t top_pct = 25;
    std::uint16_t bottom_pct = 35;
    std::uint16_t watches_pct = 25;
    std::uint16_t repl_pct = 30;
    std::uint16_t scopes_pct = 55;

    void widen_sidebar();
    void narrow_sidebar();
    void widen_right();
    void narrow_right();
    void grow_top();
    void shrink_top();
    void grow_bottom();
    void shrink_bottom();
    void widen_watches();
    void narrow_watches();
    void widen_repl();
    void narrow_repl();
    void grow_scopes();
    void shrink_scopes();
};

struct ThreadInfo {
    std::int64_t id = 0;
    std::string name;
};

struct StackFrameInfo {
    std::int64_t id = 0;
    std::string name;
    std::int64_t line = 0;
    std::string path;
    /// DAP `sourceReference` when the adapter serves source without a disk path.
    std::int64_t source_reference = 0;
    /// DAP `instructionPointerReference` for disassembly / memory views.
    std::string instruction_pointer_reference;
};

struct ThreadStackInfo {
    std::int64_t thread_id = 0;
    std::vector<StackFrameInfo> frames;
};

struct ScopeInfo {
    std::string name;
    std::int64_t variables_reference = 0;
};

struct ScopeVariableRowMeta {
    std::int64_t container_reference = 0;
    std::int64_t expand_reference = 0;
    /// Stable expand key: scope name + `\x1F`-separated variable names (survives DAP ref changes).
    std::string expand_path;
    std::string variable_name;
    bool show_edit = false;
    bool is_scope_section = false;
    std::string scope_section_name;
};

struct VariableInfo {
    std::string name;
    std::string value;
    std::int64_t variables_reference = 0;

    [[nodiscard]] bool has_children() const { return variables_reference > 0; }

    friend bool operator==(const VariableInfo& lhs, const VariableInfo& rhs) {
        return lhs.name == rhs.name && lhs.value == rhs.value &&
               lhs.variables_reference == rhs.variables_reference;
    }
};

struct ConsoleLine {
    std::string category;
    std::string text;
};

struct WatchEntry {
    std::uint64_t id = 0;
    std::string expression;
    std::string value;
    std::string error;
};

struct ExceptionInfo {
    std::string exception_id;
    std::string break_mode;
    std::string description;
    std::string type_name;
    std::string message;
    std::string evaluate_name;
    std::string stack_trace;
};

/// View model mirroring debugger state for the Tuinator UI layer.
class DebugUiModel {
  public:
    ConnectionState connection_state = ConnectionState::Connecting;
    Focus focus = Focus::Source;
    LayoutConfig layout{};

    std::string source_path;
    std::int64_t source_reference = 0;
    std::string execution_path;
    std::int64_t execution_source_reference = 0;
    std::uint32_t execution_line = 0;
    std::uint32_t current_line = 0;
    std::string status_message = "Connecting to debugpy…";

    std::string session_state;
    std::string stop_reason;
    std::int64_t stopped_thread_id = 0;
    std::optional<ExceptionInfo> exception_info;
    std::vector<ThreadInfo> threads;
    std::vector<StackFrameInfo> stack_frames;
    std::vector<ThreadStackInfo> thread_stacks;
    std::vector<ScopeInfo> scopes;
    std::vector<VariableInfo> variables;
    /// Variables keyed by DAP `variablesReference` (lazy-loaded per scope).
    std::unordered_map<std::int64_t, std::vector<VariableInfo>> scope_variables;
    std::vector<ConsoleLine> console_lines;
    NetworkMockSession network_session;
    bool network_capture_live = false;
    bool network_show_mock_fallback = false;
    std::vector<std::string> repl_history;
    std::vector<WatchEntry> watches;
    std::uint64_t next_watch_id = 1;

    bool supports_step_back = false;
    bool supports_step_in_targets = false;
    bool supports_goto_targets = false;
    bool supports_data_breakpoints = false;
    bool supports_function_breakpoints = false;
    bool supports_completions_request = false;
    bool supports_read_memory_request = false;
    bool supports_write_memory_request = false;
    bool supports_disassemble_request = false;

    struct ExceptionBreakpointFilterInfo {
        std::string filter;
        std::string label;
        std::string description;
        bool default_enabled = false;
        bool supports_condition = false;
    };
    std::vector<ExceptionBreakpointFilterInfo> exception_breakpoint_filters;
    /// OS PIDs from DAP `process` events (`systemProcessId`), aggregated for resource metrics.
    std::vector<std::uint32_t> debug_process_ids;

    /// Apply a JSON snapshot from the Rust session.
    void apply_snapshot_json(const std::string& json);

    std::string connection_label() const;
    std::string focus_label() const;
};

}  // namespace tui_debug_ui
