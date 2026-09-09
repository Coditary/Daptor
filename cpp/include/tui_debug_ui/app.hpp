#pragma once

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/debug_ui_model.hpp"
#include "tui_debug_ui/session_backend.hpp"
#include "tui_debug_ui/session_io_thread.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/widgets/views/list_view.hpp>

#include <chrono>
#include <memory>
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
class TitledScrollPane;

/// Tuinator application wrapper for the tui-debug shell.
class DebugApp {
  public:
    explicit DebugApp(const std::string& program_path, SessionMode mode = SessionMode::Rust);
    ~DebugApp();

    DebugApp(const DebugApp&) = delete;
    DebugApp& operator=(const DebugApp&) = delete;

    int run();

    /// Global debugger / navigation keys routed from DebugRoot.
    bool handle_global_key(const tuinator::KeyPress& key);

    /// Evaluate a REPL expression against the current stack frame.
    void submit_repl(const std::string& expression);

    /// Re-layout after terminal resize (called from DebugRoot).
    void on_terminal_resize();

    /// Sync model focus label after Tuinator mouse focus changes.
    void sync_focus_from_ui();

    /// Periodic session/event pump (called from the root widget idle hook).
    void poll_session();

  private:
    void ensure_ui_built();
    void build_ui();
    void handle_session_event(const SessionIoEvent& event);
    void send_command(const char* op);
    void maybe_request_scope_variables();
    void maybe_request_source_highlight();
    void apply_instant_source_viewport(int first_line, int line_count);
    void sync_status_bar();
    void apply_scope_variables_payload(const std::string& signature, const std::string& json);
    void maybe_start_launch();
    void handle_launch_complete();
    bool update_connecting_spinner();
    void sync_ui_from_model();
    void invalidate_scope_variables();
    std::string build_scope_variables_signature() const;
    void apply_console_json_payload(const std::string& json);
    void apply_snapshot_json_payload(const std::string& json);
    void cycle_focus_next();
    void apply_focus();
    void toggle_breakpoint();
    void toggle_breakpoint_at_line(int line);
    std::string effective_source_path() const;
    void sync_breakpoints_to_panel();
    void push_breakpoints_to_session(const std::string& path);
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
    void sync_controls_bar();
    bool is_session_stopped() const;
    void mark_all_panels_dirty();

    SessionMode mode_;
    std::string program_path_;
    std::unique_ptr<SessionIoThread> session_io_;
    bool launch_complete_handled_ = false;
    bool launch_posted_ = false;
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
    std::unique_ptr<TitledScrollPane> source_section_;
    SourcePanel* source_panel_ = nullptr;
    tuinator::TextInput* repl_input_ = nullptr;
    tuinator::ListView* repl_history_ = nullptr;
    std::vector<std::string> repl_history_lines_;
    ConsolePanel* console_panel_ = nullptr;
    tuinator::ScrollView* console_scroll_view_ = nullptr;
    ResizableSplitPane* sidebar_split_ = nullptr;
    ResizableSplitPane* main_row_split_ = nullptr;
    ResizableSplitPane* bottom_tray_split_ = nullptr;
    ResizableSplitPane* content_split_ = nullptr;
    std::string cached_source_path_;
    std::string cached_source_text_;
    std::string cached_source_title_;
    std::string cached_status_bar_text_;
    std::vector<std::string> cached_scope_rows_;
    std::vector<std::string> cached_stack_lines_;
    int cached_highlight_first_line_ = -1;
    int cached_highlight_line_count_ = -1;
    int highlight_request_first_line_ = -1;
    int highlight_request_line_count_ = -1;
    std::uint64_t snapshot_generation_ = 0;
    std::uint64_t cached_follow_generation_ = 0;
    std::uint32_t cached_follow_line_ = 0;
    std::string scope_variables_signature_;
    std::string scope_variables_fetch_signature_;
    bool scope_variables_fetch_pending_ = false;
    bool restart_pending_ = false;
    std::unordered_map<std::string, std::unordered_set<int>> breakpoints_by_path_;
    std::chrono::steady_clock::time_point last_spinner_update_{};
};

}  // namespace tui_debug_ui
