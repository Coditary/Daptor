#pragma once

#include "tui_debug_ui/session_backend.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tui_debug_ui {

enum class SessionIoEventKind {
    LaunchFinished,
    SnapshotJson,
    PollJson,
    ConsoleJson,
    ScopeVariablesReady,
    VariableChildrenReady,
    SourceReady,
    HighlightReady,
    CommandFinished,
    EvaluateFinished,
    SetVariableFinished,
    BreakpointsFinished,
    DataBreakpointInfoReady,
    DataBreakpointsFinished,
    FunctionBreakpointsFinished,
    ExceptionBreakpointsFinished,
    StepInTargetsReady,
    GotoTargetsReady,
};

struct PendingSetVariable {
    std::int64_t variables_reference = 0;
    std::string name;
    std::string value;
};

struct SessionIoEvent {
    SessionIoEventKind kind = SessionIoEventKind::LaunchFinished;
    bool success = true;
    std::string payload;
    std::string detail;
    std::int64_t scope_ref = 0;
    std::int64_t source_reference = 0;
    int highlight_first_line = 0;
    int highlight_line_count = 0;
};

/// Owns the SessionBackend on a worker thread so the Tuinator UI never blocks on DAP I/O.
class SessionIoThread {
  public:
    explicit SessionIoThread(SessionMode mode, DebugAdapter adapter = DebugAdapter::Debugpy);
    ~SessionIoThread();

    SessionIoThread(const SessionIoThread&) = delete;
    SessionIoThread& operator=(const SessionIoThread&) = delete;

    void start_launch(const std::string& program_path, const std::vector<std::string>& program_args);
    bool launch_finished() const;
    bool is_active() const;
    bool adapter_live() const;

    void post_command(const std::string& op);
    void post_evaluate(const std::string& expression, std::int64_t frame_id, const std::string& context);
    void post_set_variable(std::int64_t variables_reference, const std::string& name, const std::string& value);
    void post_set_breakpoints(const std::string& path, const std::string& lines_json);
    void request_data_breakpoint_info(std::int64_t variables_reference, std::int64_t frame_id,
                                      const std::string& name, const std::string& access_type);
    void post_set_data_breakpoints(const std::string& breakpoints_json);
    void post_set_function_breakpoints(const std::string& breakpoints_json);
    void post_set_exception_breakpoints(const std::string& filters_json);
    void request_scope_variables(const std::string& signature,
                                 const std::vector<std::pair<std::int64_t, std::string>>& scopes);
    void request_variable_children(std::int64_t variables_reference, const std::string& path);
    void request_highlight(const std::string& language, const std::string& source, int first_line, int line_count);
    void request_source_fetch(std::int64_t source_reference, const std::string& cache_key);
    void request_step_in_targets(std::int64_t frame_id);
    void request_goto_targets(const std::string& path, int line, int column);

    bool try_pop_event(SessionIoEvent& out);
    [[nodiscard]] bool has_pending_execution_command() const;

  private:
    struct HighlightRequest {
        std::string language;
        std::string source;
        int first_line = 0;
        int line_count = 0;
    };

    void thread_main();
    void push_event(SessionIoEvent event);
    void run_poll_cycle();
    void process_preempting_commands();
    void process_pending_command();
    void process_pending_breakpoints();
    void process_data_breakpoint_info_fetch();
    void process_pending_data_breakpoints();
    void process_pending_function_breakpoints();
    void process_pending_exception_breakpoints();
    void dispatch_command(const std::string& op);
    void process_scope_fetch();
    void process_variable_children_fetch();
    void process_highlight_request();
    void process_source_fetch();
    void process_step_in_targets_fetch();
    void process_goto_targets_fetch();
    bool process_pending_set_variable();
    void maybe_begin_launch();
    void join_launch_worker();
    void sync_initial_state();
    void clear_pending_adapter_work();
    bool has_pending_command() const;
    static bool command_needs_snapshot(const std::string& op);
    static bool command_syncs_snapshot(const std::string& op);
    static bool command_preempts_background_work(const std::string& op);
    static std::string command_op_name(const std::string& op_or_json);

    std::unique_ptr<SessionBackend> backend_;
    std::thread thread_;
    std::thread launch_worker_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> launch_started_{false};
    std::atomic<bool> launch_finished_{false};
    std::atomic<bool> backend_active_{false};
    std::atomic<bool> adapter_live_{false};
    std::atomic<bool> scope_fetch_aborted_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string program_path_;
    std::vector<std::string> program_args_;
    std::string pending_command_;
    bool has_pending_command_ = false;
    std::optional<std::string> pending_evaluate_;
    std::int64_t pending_evaluate_frame_ = 0;
    std::string pending_evaluate_context_;
    std::optional<PendingSetVariable> pending_set_variable_;
    std::optional<std::string> pending_breakpoints_path_;
    std::optional<std::string> pending_breakpoints_json_;
    std::chrono::steady_clock::time_point breakpoints_posted_at_{};
    struct DataBreakpointInfoRequest {
        std::int64_t variables_reference = 0;
        std::int64_t frame_id = 0;
        std::string name;
        std::string access_type;
    };
    std::optional<DataBreakpointInfoRequest> data_breakpoint_info_request_;
    std::optional<std::string> pending_data_breakpoints_json_;
    std::chrono::steady_clock::time_point data_breakpoints_posted_at_{};
    std::optional<std::string> pending_function_breakpoints_json_;
    std::chrono::steady_clock::time_point function_breakpoints_posted_at_{};
    std::optional<std::string> pending_exception_breakpoints_json_;
    std::chrono::steady_clock::time_point exception_breakpoints_posted_at_{};
    std::string scope_fetch_signature_;
    std::vector<std::pair<std::int64_t, std::string>> scope_fetch_scopes_;
    bool scope_fetch_pending_ = false;
    struct VariableChildrenFetchRequest {
        std::int64_t variables_reference = 0;
        std::string path;
    };
    std::deque<VariableChildrenFetchRequest> variable_children_fetch_queue_;
    std::optional<HighlightRequest> highlight_request_;
    std::optional<std::int64_t> source_fetch_reference_;
    std::string source_fetch_cache_key_;
    std::optional<std::int64_t> step_in_targets_frame_;
    struct GotoTargetsRequest {
        std::string path;
        int line = 0;
        int column = -1;
    };
    std::optional<GotoTargetsRequest> goto_targets_request_;

    std::mutex events_mutex_;
    std::deque<SessionIoEvent> events_;
};

}  // namespace tui_debug_ui
