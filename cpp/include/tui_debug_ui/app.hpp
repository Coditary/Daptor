#pragma once

#include "tui_debug_ui/breakpoint_info.hpp"
#include "tui_debug_ui/data_breakpoint_info.hpp"
#include "tui_debug_ui/function_breakpoint_info.hpp"
#include "tui_debug_ui/context_menu.hpp"
#include "tui_debug_ui/file_picker.hpp"
#include "tui_debug_ui/app_config.hpp"
#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/syntax_theme.hpp"
#include "tui_debug_ui/debug_ui_model.hpp"
#include "tui_debug_ui/highlight_bridge.hpp"
#include "tui_debug_ui/source_panel.hpp"
#include "tui_debug_ui/launch_plan.hpp"
#include "tui_debug_ui/session_backend.hpp"
#include "tui_debug_ui/repl_panel.hpp"
#include "tui_debug_ui/session_io_thread.hpp"
#include "tui_debug_ui/stacks_panel.hpp"
#include "tui_debug_ui/layout_drag.hpp"
#include "tui_debug_ui/layout_tree.hpp"
#include "tui_debug_ui/panel_slot.hpp"
#include "tui_debug_ui/process_metrics.hpp"
#include "tui_debug_ui/sidebar_slot.hpp"
#include "tui_debug_ui/step_in_selection.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/widgets/views/list_view.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
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
class MemoryPanel;
class DisassemblyPanel;
class RuntimeSourcePanel;
class FileTreePanel;
class ResourcesPanel;
class NetworkPanel;
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

/// Tuinator application wrapper for the daptor shell.
class DebugApp {
  public:
    explicit DebugApp(const std::string& program_path, SessionMode mode = SessionMode::Rust,
                      LaunchUiSettings launch_ui = {}, std::vector<std::string> program_args = {},
                      AppConfig app_config = {},
                      std::optional<std::string> resolved_launch_json = std::nullopt,
                      std::optional<std::filesystem::path> workspace_override = std::nullopt,
                      std::optional<std::string> display_source_override = std::nullopt);
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
    [[nodiscard]] bool file_picker_open() const;
    [[nodiscard]] bool breakpoint_prompt_active() const;
    [[nodiscard]] bool overlay_intercepts_events() const;
    bool handle_overlay_event(const tuinator::Event& event);
    void paint_overlay(tuinator::PaintContext& ctx) const;
    void finalize_text_cursor(tuinator::PaintContext& ctx) const;

    [[nodiscard]] bool is_watch_input_focused() const;
    [[nodiscard]] bool is_breakpoint_input_focused() const;
    [[nodiscard]] bool is_scope_input_focused() const;
    [[nodiscard]] bool is_memory_input_focused() const;
    [[nodiscard]] bool console_input_active() const;
    [[nodiscard]] bool is_repl_input_focused() const;
    [[nodiscard]] bool should_block_app_quit_key(const tuinator::KeyPress& key) const;
    void blur_watch_input();
    void blur_breakpoint_input(bool cancelled = true);
    void blur_scope_input();
    void blur_memory_input();
    void blur_memory_toolbar_inputs();
    void blur_active_memory_input();
    bool handle_breakpoint_input_key(const tuinator::Event& event);
    bool handle_scope_input_key(const tuinator::Event& event);
    bool handle_memory_input_key(const tuinator::Event& event);
    bool handle_memory_toolbar_input_key(const tuinator::Event& event);
    bool handle_memory_list_navigation_key(const tuinator::Event& event);
    bool handle_memory_activate_key(const tuinator::Event& event);
    void sync_memory_address_from_selected_row(std::uint64_t slot_id, int row_index);
    bool handle_watch_input_key(const tuinator::Event& event);
    bool handle_repl_input_key(const tuinator::Event& event);
    void blur_repl_input();
    bool handle_stacked_pane_rename_key(const tuinator::Event& event);
    void handle_pointer_pick(const tuinator::MouseEvent& mouse);
    void sync_controls_hover(tuinator::Point position);
    bool handle_layout_drag_mouse(const tuinator::MouseEvent& mouse);
    [[nodiscard]] bool layout_drag_active() const;

  private:
    void ensure_ui_built();
    void build_ui();
    void handle_session_event(const SessionIoEvent& event);
    void send_command(const char* op);
    void maybe_request_scope_variables();
    void maybe_request_dap_panel_data();
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
    void apply_network_json_payload(const std::string& json);
    void refresh_network_panel();
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
    void open_file_picker();
    void close_file_picker();
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
    bool handle_tab_navigation_key(const tuinator::KeyPress& key);
    void cycle_sidebar_stack(int delta);
    void cycle_bottom_stack(int delta);
    void cycle_active_leaf_tabs(int delta);
    void cycle_focused_layout_leaf(int delta);
    [[nodiscard]] LayoutNodeId find_leaf_id_for_focus() const;
    void sync_focused_layout_leaf();
    void sync_stack_panes_to_focus();
    void ensure_layout_tree_initialized();
    void ensure_default_leaf_slots(LayoutNodeId leaf_id);
    std::unique_ptr<tuinator::Widget> build_layout_widget(LayoutNodeId node_id);
    std::unique_ptr<StackedPane> build_stacked_pane_for_leaf(LayoutNodeId leaf_id);
    void rebuild_layout_ui();
    void reset_layout_slot_widgets();
    std::unique_ptr<tuinator::Widget> build_layout_content_widget();
    void show_pane_layout_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_move_tab_menu(LayoutNodeId from_leaf, tuinator::Point anchor);
    void show_swap_pane_menu(LayoutNodeId from_leaf, tuinator::Point anchor);
    void show_move_pane_menu(LayoutNodeId from_leaf, tuinator::Point anchor);
    void show_move_pane_direction_menu(LayoutNodeId from_leaf, LayoutNodeId target_leaf, tuinator::Point anchor);
    void open_layout_context_menu(tuinator::Point anchor, std::vector<ContextMenu::Item> items,
                                  const std::function<void(int index)>& on_hover);
    void clear_layout_menu_preview();
    void set_layout_menu_pane_preview(LayoutNodeId source_leaf, LayoutNodeId target_leaf);
    void set_layout_menu_placement_preview(LayoutNodeId source_leaf, const LayoutPlacementOption& option);
    [[nodiscard]] std::vector<LayoutPlacementOption> collect_move_placement_options(LayoutNodeId from_leaf,
                                                                                    LayoutNodeId reference_leaf) const;
    [[nodiscard]] LayoutDropTarget make_placement_drop_target(LayoutNodeId anchor, LayoutNodeId reference_leaf,
                                                              LayoutDropZone zone, bool spans_siblings) const;
    [[nodiscard]] std::string layout_group_label(LayoutNodeId node_id) const;
    [[nodiscard]] std::vector<LayoutNodeId> leaves_in_subtree(LayoutNodeId node_id) const;
    void show_pane_add_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void move_active_panel_to_leaf(LayoutNodeId from_leaf, LayoutNodeId to_leaf);
    void move_tab_to_leaf(LayoutNodeId from_leaf, int tab_index, LayoutNodeId to_leaf);
    void swap_layout_panes(LayoutNodeId a, LayoutNodeId b);
    void normalize_split_after_swap(LayoutNodeId a, LayoutNodeId b);
    void move_layout_pane_adjacent(LayoutNodeId from_leaf, LayoutNodeId anchor, LayoutDropZone zone);
    void move_tab_to_new_pane_adjacent(LayoutNodeId from_leaf, int tab_index, LayoutNodeId to_leaf,
                                       LayoutDropZone zone);
    void on_layout_drag_press(LayoutNodeId leaf_id, LayoutDragSourceKind kind, int tab_index, tuinator::Point position);
    void begin_layout_drag();
    void update_layout_drag_hover(tuinator::Point position);
    void cancel_layout_drag();
    void commit_layout_drag(tuinator::Point position);
    [[nodiscard]] std::optional<LayoutNodeId> layout_leaf_at_point(tuinator::Point position) const;
    [[nodiscard]] tuinator::Rect layout_node_bounds(LayoutNodeId node_id) const;
    [[nodiscard]] tuinator::Rect layout_content_bounds() const;
    [[nodiscard]] LayoutNodeId compute_insert_anchor(LayoutNodeId from_leaf, LayoutNodeId anchor,
                                                     LayoutDropZone zone) const;
    [[nodiscard]] std::optional<LayoutDropTarget> resolve_layout_drop_target(tuinator::Point position,
                                                                             LayoutDragSourceKind kind,
                                                                             LayoutNodeId source_leaf) const;
    void paint_layout_drag_overlay(tuinator::PaintContext& ctx) const;
    void paint_layout_menu_preview(tuinator::PaintContext& ctx) const;
    void paint_layout_drop_highlight(tuinator::PaintContext& ctx, const LayoutDropTarget& target,
                                     tuinator::Style style) const;
    [[nodiscard]] std::optional<LayoutDropTarget> layout_menu_preview_target() const;
    void collapse_empty_leaf_if_needed(LayoutNodeId leaf_id);
    [[nodiscard]] std::string layout_leaf_label(LayoutNodeId leaf_id) const;
    [[nodiscard]] std::string layout_leaf_move_label(LayoutNodeId from_leaf, LayoutNodeId to_leaf) const;
    void split_layout_pane(LayoutNodeId leaf_id, PaneSplitDirection direction);
    void delete_layout_pane(LayoutNodeId leaf_id);
    [[nodiscard]] LayoutNodeId dock_leaf_id(PanelDock dock) const;
    [[nodiscard]] std::vector<SidebarSlot>& leaf_slots(LayoutNodeId leaf_id);
    [[nodiscard]] StackedPane* layout_stack(LayoutNodeId leaf_id);
    [[nodiscard]] const StackedPane* layout_stack(LayoutNodeId leaf_id) const;
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
    void for_each_sidebar_slot(const std::function<void(SidebarSlot&)>& visitor);
    [[nodiscard]] SidebarSlot* slot_by_id(std::uint64_t slot_id);
    [[nodiscard]] SidebarSlot* sidebar_slot_at(int index);
    [[nodiscard]] SidebarSlot* active_sidebar_slot();
    [[nodiscard]] SidebarSlot* sidebar_slot_by_id(std::uint64_t slot_id);
    [[nodiscard]] static bool focus_matches_panel_type(Focus focus, SidebarPanelType type);
    [[nodiscard]] static Focus focus_for_panel_type(SidebarPanelType type);
    [[nodiscard]] int dock_index_for_focus(PanelDock dock, Focus focus) const;
    [[nodiscard]] bool is_panel_type_active(SidebarPanelType type) const;
    void sync_dock_stack_to_focus(PanelDock dock);
    [[nodiscard]] SidebarSlot* active_slot_for_focus();
    [[nodiscard]] SidebarSlot* find_memory_slot();
    [[nodiscard]] SidebarSlot* memory_slot_for_focus(std::uint64_t preferred_slot_id = 0);
    void activate_memory_toolbar(std::uint64_t slot_id);
    [[nodiscard]] std::optional<std::pair<LayoutNodeId, int>> find_source_panel_slot() const;
    [[nodiscard]] bool has_source_panel() const;
    void move_source_panel_to_leaf(LayoutNodeId target_leaf);
    void move_source_panel_to_dock(PanelDock dock);
    void refresh_all_scope_slots();
    void sync_scope_slot(SidebarSlot& slot);
    void sync_memory_slot(SidebarSlot& slot);
    void sync_disassembly_slot(SidebarSlot& slot);
    void sync_runtime_source_slot(SidebarSlot& slot);
    void refresh_all_dap_panel_slots();
    void wire_memory_panel(MemoryPanel& panel, SidebarSlot& slot);
    void begin_memory_row_edit(std::uint64_t slot_id, int row_index);
    void submit_memory_write(std::uint64_t slot_id, int row_index, const std::string& hex_input);
    void submit_memory_address(std::uint64_t slot_id, const std::string& input);
    enum class MemorySearchNavigation {
        Confirm,
        Next,
        Previous,
    };
    void submit_memory_search(std::uint64_t slot_id, const std::string& query, MemorySearchNavigation navigation);
    void preview_memory_search(std::uint64_t slot_id, const std::string& query);
    void apply_memory_search_highlight(SidebarSlot& slot);
    void navigate_memory_view(std::uint64_t slot_id, const std::string& reference, std::int64_t offset);
    bool try_reveal_memory_address_in_loaded_view(std::uint64_t slot_id, const std::string& address);
    void request_memory_fetch_for_slot(SidebarSlot& slot, const std::string& reference, std::int64_t offset);
    void wire_disassembly_panel(DisassemblyPanel& panel, SidebarSlot& slot);
    void wire_runtime_source_panel(RuntimeSourcePanel& panel, SidebarSlot& slot);
    void wire_file_tree_panel(FileTreePanel& panel);
    void sync_file_tree_slot(SidebarSlot& slot);
    void sync_file_tree_slots();
    void sync_resources_slot(SidebarSlot& slot);
    void sync_network_slot(SidebarSlot& slot);
    void wire_network_panel(NetworkPanel& panel);
    void tick_process_metrics();
    void process_pending_file_tree_open();
    void sync_breakpoint_slot(SidebarSlot& slot, const std::vector<BreakpointRow>& rows);
    void sync_thread_slot(SidebarSlot& slot, const std::vector<ThreadStackContent>& threads);
    void sync_threads_list_panel();
    [[nodiscard]] std::vector<BreakpointRow> build_breakpoint_rows() const;
    [[nodiscard]] std::vector<ThreadStackContent> build_thread_stack_contents() const;
    [[nodiscard]] std::vector<WatchEntry>& active_watch_list();
    void show_add_panel_menu(tuinator::Point anchor, LayoutNodeId leaf_id);
    void show_add_scope_menu(LayoutNodeId leaf_id, SidebarPanelType type, tuinator::Point anchor);
    [[nodiscard]] std::vector<std::string> available_scope_names() const;
    void remember_scope_names_from_model();
    [[nodiscard]] std::vector<ThreadInfo> available_thread_menu_threads() const;
    void remember_threads_from_model();
    void show_add_breakpoint_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_add_thread_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_add_advanced_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_add_disassembly_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_add_workspace_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
    void show_add_output_menu(LayoutNodeId leaf_id, tuinator::Point anchor);
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
    void sync_overlay_mouse_tracking();
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
    LaunchUiSettings launch_ui_;
    std::string program_path_;
    std::vector<std::string> program_args_;
    std::optional<std::string> resolved_launch_json_;
    std::optional<std::string> display_source_override_;
    std::unique_ptr<SessionIoThread> session_io_;
    bool launch_complete_handled_ = false;
    bool launch_posted_ = false;
    bool reverse_continue_hint_shown_ = false;
    bool terminal_ready_for_session_ = false;
    bool ui_built_ = false;
    bool divider_drag_active_ = false;
    std::string launch_error_;
    int spinner_frame_ = 0;
    ProcessMetricsSampler process_metrics_sampler_;
    std::chrono::steady_clock::time_point last_process_metrics_tick_{};
    DebugUiModel model_;
    AppConfig app_config_;
    DapUiTheme dap_theme_;
    SyntaxTheme syntax_theme_;
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
    struct LayoutDragState {
        bool active = false;
        struct Pending {
            LayoutNodeId source_leaf = 0;
            LayoutDragSourceKind kind = LayoutDragSourceKind::Pane;
            int tab_index = 0;
            tuinator::Point start{};
        };
        std::optional<Pending> pending;
        LayoutNodeId source_leaf = 0;
        LayoutDragSourceKind kind = LayoutDragSourceKind::Pane;
        int tab_index = 0;
        tuinator::Point position{};
        std::optional<LayoutDropTarget> hover;
    };
    LayoutDragState layout_drag_;
    struct LayoutMenuPreview {
        LayoutNodeId source_leaf = 0;
        LayoutDropTarget target{};
        enum class Mode { PaneHighlight, Placement } mode = Mode::PaneHighlight;
        bool swap = false;
    };
    std::optional<LayoutMenuPreview> layout_menu_preview_;
    std::uint64_t next_slot_id_ = 1;
    std::unique_ptr<tuinator::Widget> repl_shell_;
    std::unique_ptr<tuinator::Widget> console_shell_;
    NetworkPanel* network_panel_ = nullptr;
    bool network_select_last_on_refresh_ = false;
    bool compose_send_pending_ = false;
    std::unique_ptr<tuinator::Widget> network_shell_;
    std::unique_ptr<tuinator::Widget> source_content_shell_;
    std::unique_ptr<ContextMenu> context_menu_;
    std::unique_ptr<FilePicker> file_picker_;
    std::filesystem::path workspace_root_;
    std::vector<std::filesystem::path> workspace_files_;
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
    std::unordered_set<std::string> collapsed_scope_sections_;
    std::vector<std::string> known_scope_names_;
    std::vector<ThreadInfo> known_threads_;
    std::vector<ThreadStackContent> cached_thread_stack_contents_;
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
    std::uint64_t memory_write_slot_id_ = 0;
    std::uint64_t memory_focus_slot_id_ = 0;
    std::uint64_t memory_address_eval_slot_id_ = 0;
    std::int64_t memory_write_offset_ = 0;
    int memory_write_row_ = -1;
    bool memory_write_active_ = false;
    bool memory_toolbar_focused_ = false;
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
    std::optional<std::string> pending_file_tree_open_;
    bool overlay_hover_tracking_active_ = false;
    struct EphemeralCatchSkipBreakpoint {
        std::string path;
        int line = 0;
        bool added = false;
    };
    std::optional<EphemeralCatchSkipBreakpoint> ephemeral_catch_skip_;
    std::chrono::steady_clock::time_point last_spinner_update_{};
};

}  // namespace tui_debug_ui
