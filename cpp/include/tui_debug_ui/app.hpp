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
#include "tui_debug_ui/repl_panel.hpp"
#include "tui_debug_ui/session_io_thread.hpp"
#include "tui_debug_ui/stacks_panel.hpp"
#include "tui_debug_ui/layout_tree.hpp"
#include "tui_debug_ui/panel_slot.hpp"
#include "tui_debug_ui/sidebar_slot.hpp"
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
class ReplPanel;
class SourcePanel;
class StacksPanel;
class BreakpointsPanel;
class WatchesPanel;
class TitledScrollPane;
class StackedPane;
class SharedWidgetHost;
class SourceTabBar;

struct SourceFileTab {
    std::string path;
    std::int64_t source_reference = 0;
    std::string cache_key;
    std::string cached_text;
    int cursor_line = 1;
    int scroll_y = 0;
};

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
    [[nodiscard]] bool is_repl_input_focused() const;
    [[nodiscard]] bool should_block_app_quit_key(const tuinator::KeyPress& key) const;
    void blur_watch_input();
    void blur_breakpoint_input(bool cancelled = true);
    void blur_scope_input();
    bool handle_breakpoint_input_key(const tuinator::Event& event);
    bool handle_scope_input_key(const tuinator::Event& event);
    bool handle_watch_input_key(const tuinator::Event& event);
    bool handle_repl_input_key(const tuinator::Event& event);
    void blur_repl_input();
    bool handle_stacked_pane_rename_key(const tuinator::Event& event);
    void handle_pointer_pick(const tuinator::MouseEvent& mouse);

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
    void toggle_scope_row_expand(std::uint64_t slot_id, int row_index);
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
    void begin_add_watch(const std::string& seed = "");
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
    void save_active_source_file_tab();
    void activate_source_file_tab(int index, int line = 0);
    void switch_source_file_tab(int index);
    void close_source_file_tab(int index);
    void sync_source_file_tab_bar();
    [[nodiscard]] int find_source_file_tab_index(const std::string& cache_key) const;
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
    bool handle_panel_swap_key(const tuinator::KeyPress& key);
    void cycle_sidebar_stack(int delta);
    void cycle_bottom_stack(int delta);
    void sync_stack_panes_to_focus();
    void ensure_layout_tree_initialized();
    void ensure_default_leaf_slots(LayoutNodeId leaf_id);
    std::unique_ptr<tuinator::Widget> build_layout_widget(LayoutNodeId node_id);
    std::unique_ptr<StackedPane> build_stacked_pane_for_leaf(LayoutNodeId leaf_id);
    void rebuild_layout_ui();
    void reset_layout_slot_widgets();
    std::unique_ptr<tuinator::Widget> build_layout_content_widget();
    void show_pane_layout_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_pane_add_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void split_layout_pane(LayoutNodeId leaf_id, PaneSplitDirection direction);
    void delete_layout_pane(LayoutNodeId leaf_id);
    [[nodiscard]] LayoutNodeId dock_leaf_id(PanelDock dock) const;
    [[nodiscard]] std::vector<SidebarSlot>& leaf_slots(LayoutNodeId leaf_id);
    [[nodiscard]] StackedPane* layout_stack(LayoutNodeId leaf_id);
    [[nodiscard]] int& leaf_stack_index(LayoutNodeId leaf_id);
    void init_default_sidebar_slots(std::vector<SidebarSlot>& slots);
    void ensure_sidebar_slot_panels(SidebarSlot& slot, const tuinator::ScrollViewOptions& scroll_options);
    std::unique_ptr<tuinator::Widget> release_sidebar_slot_widget(SidebarSlot& slot);
    void update_active_panel_pointers();
    [[nodiscard]] std::vector<SidebarSlot>& dock_slots(PanelDock dock);
    [[nodiscard]] const std::vector<SidebarSlot>& dock_slots(PanelDock dock) const;
    [[nodiscard]] StackedPane* dock_stack(PanelDock dock);
    [[nodiscard]] int& dock_stack_index(PanelDock dock);
    [[nodiscard]] std::vector<PanelSlotConfig> dock_slot_configs(PanelDock dock) const;
    [[nodiscard]] SidebarSlot* dock_slot_at(PanelDock dock, int index);
    [[nodiscard]] SidebarSlot* active_dock_slot(PanelDock dock);
    [[nodiscard]] SidebarSlot* slot_by_id(std::uint64_t slot_id);
    [[nodiscard]] SidebarSlot* sidebar_slot_at(int index);
    [[nodiscard]] SidebarSlot* active_sidebar_slot();
    [[nodiscard]] SidebarSlot* sidebar_slot_by_id(std::uint64_t slot_id);
    [[nodiscard]] static bool focus_matches_panel_type(Focus focus, SidebarPanelType type);
    [[nodiscard]] static Focus focus_for_panel_type(SidebarPanelType type);
    [[nodiscard]] int dock_index_for_focus(PanelDock dock, Focus focus) const;
    void sync_dock_stack_to_focus(PanelDock dock);
    [[nodiscard]] SidebarSlot* active_slot_for_focus();
    [[nodiscard]] std::optional<std::pair<LayoutNodeId, int>> find_source_panel_slot() const;
    [[nodiscard]] bool has_source_panel() const;
    void move_source_panel_to_leaf(LayoutNodeId target_leaf);
    void move_source_panel_to_dock(PanelDock dock);
    void refresh_all_scope_slots();
    void sync_scope_slot(SidebarSlot& slot);
    void sync_breakpoint_slot(SidebarSlot& slot, const std::vector<BreakpointRow>& rows);
    void sync_thread_slot(SidebarSlot& slot, const std::vector<ThreadStackContent>& threads);
    void sync_threads_list_panel();
    [[nodiscard]] std::vector<BreakpointRow> build_breakpoint_rows() const;
    [[nodiscard]] std::vector<ThreadStackContent> build_thread_stack_contents() const;
    [[nodiscard]] std::vector<WatchEntry>& active_watch_list();
    void show_add_panel_menu(tuinator::Point anchor, LayoutNodeId leaf_id);
    void show_add_scope_menu(LayoutNodeId leaf_id, SidebarPanelType type, tuinator::Point anchor);
    void show_add_breakpoint_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_add_thread_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void add_panel_to_leaf(LayoutNodeId leaf_id, SidebarPanelType type,
                           std::optional<std::string> scope_filter = std::nullopt,
                           std::optional<BreakpointRowKind> breakpoint_filter = std::nullopt,
                           std::optional<ThreadPanelFilter> thread_filter = std::nullopt,
                           std::optional<std::int64_t> thread_id_filter = std::nullopt,
                           std::optional<std::string> thread_name_filter = std::nullopt);
    void add_panel_to_dock(PanelDock dock, SidebarPanelType type,
                           std::optional<std::string> scope_filter = std::nullopt,
                           std::optional<BreakpointRowKind> breakpoint_filter = std::nullopt,
                           std::optional<ThreadPanelFilter> thread_filter = std::nullopt,
                           std::optional<std::int64_t> thread_id_filter = std::nullopt,
                           std::optional<std::string> thread_name_filter = std::nullopt);
    void rename_leaf_panel(LayoutNodeId leaf_id, int index, const std::string& label);
    void rename_dock_panel(PanelDock dock, int index, const std::string& label);
    [[nodiscard]] SidebarSlot* leaf_slot_at(LayoutNodeId leaf_id, int index);
    [[nodiscard]] std::vector<PanelSlotConfig> leaf_slot_configs(LayoutNodeId leaf_id) const;
    void init_default_bottom_slots(std::vector<SidebarSlot>& slots);
    void init_default_source_slots(std::vector<SidebarSlot>& slots);
    void sync_source_stack_title();
    void wire_scopes_panel(ScopesPanel& panel, SidebarSlot& slot);
    void wire_watches_panel(WatchesPanel& panel, SidebarSlot& slot);
    void wire_breakpoints_panel(BreakpointsPanel& panel);
    void wire_stacks_panel(StacksPanel& panel);
    [[nodiscard]] static bool is_sidebar_focus(Focus focus);
    [[nodiscard]] Focus focus_for_sidebar_index(int index);
    [[nodiscard]] int sidebar_index_for_focus(Focus focus);
    std::string format_status_bar_text() const;
    std::string command_status_message(const char* op) const;
    void persist_split_size_as_pct(ResizableSplitPane* split, std::uint16_t& pct_out, bool horizontal,
                                   bool invert = false);
    void bind_split_pane(ResizableSplitPane* split);
    void on_split_drag_ended();
    void request_full_screen_refresh();
    void request_repaint();
    void mark_source_view_dirty();
    void refresh_source_highlight_if_needed();
    void sync_controls_bar();
    bool is_session_stopped() const;
    void mark_all_panels_dirty();
    void refresh_scroll_views(bool include_source = true);
    void add_watch(const std::string& expression);
    void submit_watch_expression(const std::string& expression);
    void begin_edit_watch_at(int index);
    void remove_watch_at(std::size_t index);
    void sync_watches_panel();
    void sync_repl_panel();
    void submit_repl_expression(const std::string& expression);
    void deactivate_repl_input(bool clear_draft);
    void tick_repl_completion();
    void request_repl_completion();
    void handle_repl_completions_event(const SessionIoEvent& event);
    void show_repl_completion_ghost();
    bool cycle_repl_completion(int delta);
    void rebuild_repl_completion_matches();
    void clear_repl_completion_state();
    void resolve_watches_from_locals();
    void append_console_text(const std::string& text, const std::string& category = "exception");
    void remove_dollar_exception_watches();
    void handle_exception_info_from_snapshot();
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
    ScopesPanel* scopes_panel_ = nullptr;
    StacksPanel* stacks_panel_ = nullptr;
    BreakpointsPanel* breakpoints_panel_ = nullptr;
    WatchesPanel* watches_panel_ = nullptr;
    LayoutTree layout_tree_;
    std::unordered_map<LayoutNodeId, StackedPane*> layout_stacks_;
    std::unordered_map<LayoutNodeId, ResizableSplitPane*> layout_splits_;
    LayoutNodeId focused_layout_leaf_ = 0;
    std::uint64_t next_slot_id_ = 1;
    std::unique_ptr<tuinator::Widget> repl_shell_;
    std::unique_ptr<tuinator::Widget> console_shell_;
    std::unique_ptr<tuinator::Widget> source_content_shell_;
    std::unique_ptr<ContextMenu> context_menu_;
    std::unique_ptr<TitledScrollPane> source_section_;
    SourcePanel* source_panel_ = nullptr;
    SourceTabBar* source_tab_bar_ = nullptr;
    tuinator::ScrollView* source_scroll_view_ = nullptr;
    std::vector<SourceFileTab> source_file_tabs_;
    int active_source_file_tab_ = -1;
    ConsolePanel* console_panel_ = nullptr;
    tuinator::ScrollView* console_scroll_view_ = nullptr;
    std::size_t console_synced_line_count_ = 0;
    std::unique_ptr<ReplPanel> repl_panel_;
    ResizableSplitPane* main_row_split_ = nullptr;
    ResizableSplitPane* content_split_ = nullptr;
    std::string watch_input_draft_;
    bool watch_input_focused_ = false;
    std::string repl_input_draft_;
    bool repl_input_focused_ = false;
    std::chrono::steady_clock::time_point repl_last_edit_time_{};
    std::uint64_t repl_completion_request_id_ = 0;
    std::uint64_t repl_completion_pending_id_ = 0;
    std::string repl_completion_request_text_;
    std::size_t repl_completion_request_column_ = 0;
    bool repl_completion_fetch_sent_ = false;
    bool repl_completion_results_ready_ = false;
    std::vector<ReplCompletionCandidate> repl_completion_candidates_;
    std::vector<ReplCompletionCandidate> repl_completion_matches_;
    int repl_completion_selected_index_ = 0;
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
    std::string cached_status_bar_text_;
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
    int cached_source_viewport_height_ = -1;
    bool source_layout_settling_ = false;
    std::uint64_t snapshot_generation_ = 0;
    std::string last_logged_exception_key_;
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
