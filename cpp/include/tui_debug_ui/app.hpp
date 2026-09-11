#pragma once

#include "tui_debug_ui/breakpoint_info.hpp"
#include "tui_debug_ui/data_breakpoint_info.hpp"
#include "tui_debug_ui/function_breakpoint_info.hpp"
#include "tui_debug_ui/context_menu.hpp"
#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/debug_ui_model.hpp"
#include "tui_debug_ui/highlight_bridge.hpp"
#include "tui_debug_ui/source_panel.hpp"
#include "tui_debug_ui/session_backend.hpp"
#include "tui_debug_ui/session_io_thread.hpp"
#include "tui_debug_ui/stacks_panel.hpp"
#include "tui_debug_ui/step_in_selection.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/widgets/views/list_view.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tuinator {
class Application;
class ScrollView;
class StatusBar;
class TextInput;
}  // namespace tuinator

namespace tui_debug_ui {

class ControlsBar;
class ResizableSplitPane;
class ScopesPanel;
class ConsolePanel;
class SourcePanel;
class StacksPanel;
class BreakpointsPanel;
class WatchesPanel;
class TitledScrollPane;

/// Tuinator application wrapper for the tui-debug shell.
class DebugApp {
  public:
    explicit DebugApp(const std::string& program_path, SessionMode mode = SessionMode::Rust,
                      DebugAdapter adapter = DebugAdapter::Debugpy,
                      std::vector<std::string> program_args = {});
    ~DebugApp();

    DebugApp(const DebugApp&) = delete;
    DebugApp& operator=(const DebugApp&) = delete;

    int run();

    /// Global debugger / navigation keys routed from DebugRoot.
    bool handle_global_key(const tuinator::KeyPress& key);

    /// Re-layout after terminal resize (called from DebugRoot).
    void on_terminal_resize();

    /// Sync model focus label after Tuinator mouse focus changes.
    void sync_focus_from_ui();

    /// Periodic session/event pump (called from the root widget idle hook).
    void poll_session();
    void maybe_refresh_source_highlight_for_scroll();
    [[nodiscard]] bool context_menu_open() const;
    [[nodiscard]] bool breakpoint_prompt_active() const;
    [[nodiscard]] bool overlay_intercepts_events() const;
    bool handle_overlay_event(const tuinator::Event& event);
    void paint_overlay(tuinator::PaintContext& ctx) const;
    void finalize_text_cursor(tuinator::PaintContext& ctx) const;

    [[nodiscard]] bool is_watch_input_focused() const;
    [[nodiscard]] bool is_breakpoint_input_focused() const;
    [[nodiscard]] bool is_scope_input_focused() const;
    [[nodiscard]] bool console_input_active() const;
    [[nodiscard]] bool should_block_app_quit_key(const tuinator::KeyPress& key) const;
    void blur_watch_input();
    void blur_breakpoint_input(bool cancelled = true);
    void blur_scope_input();
    bool handle_breakpoint_input_key(const tuinator::Event& event);
    bool handle_scope_input_key(const tuinator::Event& event);

  private:
    void ensure_ui_built();
    void build_ui();
    void handle_session_event(const SessionIoEvent& event);
    void send_command(const char* op);
    void maybe_request_scope_variables();
    void maybe_request_source_highlight();
    void prefetch_program_source_highlight();
    void apply_highlight_payload(int first_line, int line_count, const std::string& json);
    void scroll_source_to_line(int line);
    void ensure_source_plain_lines();
    int highlight_line_count() const;
    int highlight_first_line() const;
    void apply_instant_source_viewport(int first_line, int line_count);
    int source_viewport_height() const;
    bool uses_full_file_source() const;
    void sync_status_bar();
    void apply_scope_variables_payload(const std::string& signature, const std::string& json);
    void apply_variable_children_payload(std::int64_t variables_reference, const std::string& path,
                                         const std::string& json, bool success);
    void refresh_scope_rows();
    void restore_expanded_scope_children();
    void toggle_scope_row_expand(int row_index);
    void maybe_start_launch();
    void handle_launch_complete();
    bool update_connecting_spinner();
    void sync_ui_from_model();
    void invalidate_scope_variables();
    void request_scope_variables_refresh();
    void patch_local_variable_value(const std::string& name, const std::string& value);
    void apply_scope_value_overrides();
    void clear_scope_value_overrides();
    std::string build_scope_variables_signature() const;
    void apply_console_json_payload(const std::string& json);
    void apply_snapshot_json_payload(const std::string& json);
    void cycle_focus_next();
    void apply_focus();
    void toggle_breakpoint();
    void toggle_breakpoint_at_line(int line);
    void toggle_breakpoint_at(const std::string& path, int line);
    void remove_breakpoint_at(const std::string& path, int line);
    void set_breakpoint_condition(const std::string& path, int line, const std::string& condition);
    void set_breakpoint_hit_condition(const std::string& path, int line, const std::string& hit_condition);
    void begin_edit_breakpoint_condition(const std::string& path, int line);
    void begin_edit_breakpoint_hit_condition(const std::string& path, int line);
    void begin_edit_exception_condition(const std::string& filter);
    void set_exception_breakpoint_condition(const std::string& filter, const std::string& condition);
    void open_breakpoint_condition_editor(const std::string& path, int line,
                                          std::optional<tuinator::Point> action_anchor = std::nullopt,
                                          std::optional<int> breakpoints_display_index = std::nullopt);
    void submit_breakpoint_condition(const std::string& condition);
    void capture_breakpoint_input_state();
    void restore_breakpoint_input_state();
    void sync_breakpoint_panel_input();
    void begin_edit_variable(const std::string& variable_name);
    void submit_variable_value(const std::string& value);
    void capture_scope_input_state();
    void restore_scope_input_state();
    void sync_scopes_list_panel();
    void sync_execution_location_ui();
    [[nodiscard]] bool scope_prompt_active() const;
    void begin_watch_expression(const std::string& seed);
    void show_breakpoint_context_menu(const std::string& path, int line, tuinator::Point anchor,
                                      const std::optional<SourceContextIdentifier>& source_identifier);
    void begin_source_context_menu(const std::string& path, int line, int code_column, tuinator::Point anchor,
                                   const std::optional<SourceContextIdentifier>& source_identifier);
    void open_source_context_menu(const std::string& path, int line, tuinator::Point anchor,
                                  const std::optional<SourceContextIdentifier>& source_identifier,
                                  const std::vector<std::pair<std::int64_t, std::string>>& goto_targets,
                                  bool offer_lldb_line_jump = false);
    void handle_goto_targets_payload(const SessionIoEvent& event);
    void send_goto_command(std::int64_t target_id);
    void send_goto_line_command(const std::string& path, int line);
    [[nodiscard]] std::string goto_probe_source_path() const;
    [[nodiscard]] std::optional<SourceContextIdentifier> resolve_source_identifier(
        const std::string& path, const std::string& source_text, int line, int display_column) const;
    [[nodiscard]] std::optional<std::int64_t> scope_container_for_local(const std::string& name) const;
    [[nodiscard]] tuinator::Rect overlay_clip_bounds() const;
    [[nodiscard]] tuinator::Rect breakpoints_panel_clip_bounds() const;
    [[nodiscard]] tuinator::Rect source_context_clip_bounds() const;
    std::string effective_source_path() const;
    void sync_breakpoints_to_panel();
    void push_breakpoints_to_session(const std::string& path);
    void flush_breakpoints_to_session();
    void push_data_breakpoints_to_session();
    void flush_data_breakpoints_to_session();
    void push_function_breakpoints_to_session();
    void flush_function_breakpoints_to_session();
    void push_exception_breakpoints_to_session();
    void flush_exception_breakpoints_to_session();
    void toggle_exception_breakpoint(const std::string& filter);
    void apply_exception_filter_defaults();
    void add_function_breakpoint(const std::string& name, const std::string& path = {}, int line = 0);
    void remove_function_breakpoint(const std::string& name);
    void toggle_function_breakpoint(const std::string& name, const std::string& path = {}, int line = 0);
    [[nodiscard]] bool has_function_breakpoint(const std::string& name) const;
    [[nodiscard]] int find_function_definition_line(const std::string& source_text,
                                                    const std::string& name) const;
    void show_stack_frame_context_menu(const StackFrameRow& frame, tuinator::Point anchor);
    [[nodiscard]] std::string build_function_breakpoints_json() const;
    [[nodiscard]] std::string build_exception_breakpoints_json() const;
    [[nodiscard]] std::optional<std::string> function_name_at_breakpoint_line(const std::string& path,
                                                                               const std::string& source_text,
                                                                               int line) const;
    [[nodiscard]] bool adapter_supports_function_breakpoints() const;
    void request_data_breakpoint(const std::string& variable_name, std::int64_t container_reference,
                                 const std::string& access_type);
    void remove_data_breakpoint(const std::string& data_id);
    void show_scope_variable_context_menu(int row_index, tuinator::Point anchor);
    void handle_data_breakpoint_info_payload(const SessionIoEvent& event);
    [[nodiscard]] std::string build_data_breakpoints_json() const;
    std::string normalize_source_path(const std::string& path) const;
    void normalize_breakpoint_path_keys();
    void open_source_file(const std::string& path, int line, bool pin, std::int64_t source_reference = 0);
    void maybe_follow_execution();
    void navigate_to_user_stop_frame();
    [[nodiscard]] bool both_cxx_exception_filters_enabled() const;
    [[nodiscard]] bool is_catch_phase_exception_stop() const;
    bool try_skip_exception_runtime_to_catch(const char* op);
    void maybe_finish_ephemeral_catch_skip();
    void sync_breakpoints_list_panel();
    void apply_breakpoint_hit_counts(const std::string& path, const std::string& results_json);
    void apply_breakpoint_hits_from_snapshot_json(const std::string& json);
    void apply_breakpoint_hit_entry(const std::string& path, int line, std::uint64_t hit_count);
    int find_breakpoint_line_at_stop(const BreakpointsByPath::mapped_type& breakpoints, int execution_line) const;
    void record_breakpoint_hit();
    void refresh_breakpoint_hit_counts_from_session();
    BreakpointsByPath::iterator find_breakpoints_path(const std::string& path);
    void apply_execution_command_started(const char* op);
    bool has_active_session() const;
    bool handle_layout_resize_key(const tuinator::KeyPress& key);
    std::string format_status_bar_text() const;
    std::string command_status_message(const char* op) const;
    void persist_split_size_as_pct(ResizableSplitPane* split, std::uint16_t& pct_out, bool horizontal,
                                   bool invert = false);
    void bind_split_pane(ResizableSplitPane* split);
    void on_split_drag_ended();
    void request_full_screen_refresh();
    void request_repaint();
    void sync_controls_bar();
    bool is_session_stopped() const;
    void mark_all_panels_dirty();
    void refresh_scroll_views();
    void add_watch(const std::string& expression);
    void submit_watch_expression(const std::string& expression);
    void begin_edit_watch_at(int index);
    void remove_watch_at(std::size_t index);
    void sync_watches_panel();
    void resolve_watches_from_locals();
    void capture_watch_input_state();
    void restore_watch_input_state();
    void finish_watch_input();
    [[nodiscard]] bool step_in_selection_active() const;
    void begin_step_in_selection(std::vector<StepInTargetSpan> targets);
    void cancel_step_in_selection();
    void cycle_step_in_target(int delta);
    void confirm_step_in_selection();
    void sync_step_in_selection_to_panel();
    [[nodiscard]] std::string execution_line_source_text() const;
    [[nodiscard]] std::int64_t current_frame_id() const;
    bool handle_step_in_request();
    void handle_step_in_targets_payload(const SessionIoEvent& event);
    void send_step_into_command(std::optional<std::int64_t> target_id);
    void send_command_direct(const char* op);
    void update_step_in_status_message();
    bool handle_step_in_selection_key(const tuinator::KeyPress& key);
    void maybe_apply_reverse_continue_hint();

    SessionMode mode_;
    DebugAdapter adapter_;
    std::string program_path_;
    std::vector<std::string> program_args_;
    std::unique_ptr<SessionIoThread> session_io_;
    bool launch_complete_handled_ = false;
    bool launch_posted_ = false;
    bool reverse_continue_hint_shown_ = false;
    bool terminal_ready_for_session_ = false;
    bool ui_built_ = false;
    bool divider_drag_active_ = false;
    std::string launch_error_;
    int spinner_frame_ = 0;
    DebugUiModel model_;
    DapUiTheme dap_theme_;
    std::unique_ptr<tuinator::Application> app_;

    ControlsBar* controls_bar_ = nullptr;
    tuinator::StatusBar* status_bar_ = nullptr;
    std::unique_ptr<ScopesPanel> scopes_panel_;
    std::unique_ptr<StacksPanel> stacks_panel_;
    std::unique_ptr<BreakpointsPanel> breakpoints_panel_;
    std::unique_ptr<WatchesPanel> watches_panel_;
    std::unique_ptr<ContextMenu> context_menu_;
    std::unique_ptr<TitledScrollPane> source_section_;
    SourcePanel* source_panel_ = nullptr;
    tuinator::ScrollView* source_scroll_view_ = nullptr;
    ConsolePanel* console_panel_ = nullptr;
    tuinator::ScrollView* console_scroll_view_ = nullptr;
    std::size_t console_synced_line_count_ = 0;
    ResizableSplitPane* sidebar_split_ = nullptr;
    ResizableSplitPane* main_row_split_ = nullptr;
    ResizableSplitPane* bottom_tray_split_ = nullptr;
    ResizableSplitPane* content_split_ = nullptr;
    std::string watch_input_draft_;
    bool watch_input_focused_ = false;
    int editing_watch_index_ = -1;
    std::string breakpoint_input_draft_;
    bool breakpoint_input_focused_ = false;
    std::string editing_breakpoint_path_;
    int editing_breakpoint_line_ = 0;
    bool editing_breakpoint_hit_ = false;
    std::string editing_exception_filter_;
    std::string scope_input_draft_;
    bool scope_input_focused_ = false;
    std::string editing_variable_name_;
    std::int64_t editing_variables_reference_ = 0;
    std::string pending_variable_value_;
    std::unordered_map<std::string, std::string> scope_value_overrides_;
    bool variable_set_in_flight_ = false;
    std::string cached_source_path_;
    std::int64_t cached_source_reference_ = 0;
    std::string cached_source_text_;
    std::string pending_source_fetch_key_;
    std::string cached_source_title_;
    std::string cached_status_bar_text_;
    std::vector<std::string> cached_scope_rows_;
    std::vector<ScopeVariableRowMeta> cached_scope_row_meta_;
    std::unordered_set<std::string> expanded_scope_paths_;
    std::unordered_set<std::string> pending_scope_paths_;
    std::vector<std::string> cached_stack_lines_;
    std::uint64_t cached_follow_generation_ = 0;
    std::uint32_t cached_follow_line_ = 0;
    int cached_highlight_first_line_ = -1;
    int cached_highlight_line_count_ = -1;
    int highlight_request_first_line_ = -1;
    int highlight_request_line_count_ = -1;
    int cached_highlight_scroll_y_ = -1;
    int highlight_request_scroll_y_ = -1;
    std::uint64_t snapshot_generation_ = 0;
    std::string scope_variables_signature_;
    std::string scope_variables_fetch_signature_;
    bool scope_variables_fetch_pending_ = false;
    bool restart_pending_ = false;
    bool breakpoints_flushed_after_launch_ = false;
    bool follow_execution_ = true;
    std::optional<StepInSelectionState> step_in_selection_;
    bool step_in_targets_pending_ = false;
    struct PendingSourceContextMenu {
        std::string path;
        int line = 0;
        tuinator::Point anchor{};
        std::optional<SourceContextIdentifier> source_identifier;
    };
    std::optional<PendingSourceContextMenu> pending_source_context_menu_;
    bool goto_targets_pending_ = false;
    struct BreakpointStopRecord {
        std::string path;
        std::uint32_t line = 0;
        std::int64_t thread_id = 0;

        bool operator==(const BreakpointStopRecord& other) const {
            return path == other.path && line == other.line && thread_id == other.thread_id;
        }
    };
    std::optional<BreakpointStopRecord> last_counted_breakpoint_stop_;
    std::unordered_map<std::string, std::vector<HighlightedLine>> source_plain_lines_cache_;
    BreakpointsByPath breakpoints_by_path_;
    DataBreakpoints data_breakpoints_;
    FunctionBreakpoints function_breakpoints_;
    struct ExceptionBreakpointEntry {
        bool enabled = false;
        std::string condition;
    };
    std::unordered_map<std::string, ExceptionBreakpointEntry> exception_breakpoints_;
    bool exception_defaults_applied_ = false;
    struct PendingDataBreakpointRequest {
        std::string variable_name;
        std::string access_type;
    };
    std::optional<PendingDataBreakpointRequest> pending_data_breakpoint_;
    struct EphemeralCatchSkipBreakpoint {
        std::string path;
        int line = 0;
        bool added = false;
    };
    std::optional<EphemeralCatchSkipBreakpoint> ephemeral_catch_skip_;
    std::chrono::steady_clock::time_point last_spinner_update_{};
};

}  // namespace tui_debug_ui
