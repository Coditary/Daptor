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
    HighlightReady,
    CommandFinished,
    EvaluateFinished,
    BreakpointsFinished,
};

struct SessionIoEvent {
    SessionIoEventKind kind = SessionIoEventKind::LaunchFinished;
    bool success = true;
    std::string payload;
    std::string detail;
    std::int64_t scope_ref = 0;
    int highlight_first_line = 0;
    int highlight_line_count = 0;
};

/// Owns the SessionBackend on a worker thread so the Tuinator UI never blocks on DAP I/O.
class SessionIoThread {
  public:
    explicit SessionIoThread(SessionMode mode);
    ~SessionIoThread();

    SessionIoThread(const SessionIoThread&) = delete;
    SessionIoThread& operator=(const SessionIoThread&) = delete;

    void start_launch(const std::string& program_path);
    bool launch_finished() const;
    bool is_active() const;
    bool adapter_live() const;

    void post_command(const std::string& op);
    void post_evaluate(const std::string& expression, std::int64_t frame_id, const std::string& context);
    void post_set_breakpoints(const std::string& path, const std::string& lines_json);
    void request_scope_variables(const std::string& signature,
                                 const std::vector<std::pair<std::int64_t, std::string>>& scopes);
    void request_highlight(const std::string& language, const std::string& source, int first_line, int line_count);

    bool try_pop_event(SessionIoEvent& out);

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
    void process_pending_command();
    void process_pending_breakpoints();
    void process_scope_fetch();
    void process_highlight_request();
    void sync_initial_state();
    void clear_pending_adapter_work();
    bool has_pending_command() const;
    static bool command_needs_snapshot(const std::string& op);
    static bool command_preempts_background_work(const std::string& op);

    std::unique_ptr<SessionBackend> backend_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> launch_started_{false};
    std::atomic<bool> launch_finished_{false};
    std::atomic<bool> backend_active_{false};
    std::atomic<bool> adapter_live_{false};

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::string program_path_;
    std::string pending_command_;
    bool has_pending_command_ = false;
    std::optional<std::string> pending_evaluate_;
    std::int64_t pending_evaluate_frame_ = 0;
    std::string pending_evaluate_context_;
    std::optional<std::string> pending_breakpoints_path_;
    std::optional<std::string> pending_breakpoints_json_;
    std::chrono::steady_clock::time_point breakpoints_posted_at_{};
    std::string scope_fetch_signature_;
    std::vector<std::pair<std::int64_t, std::string>> scope_fetch_scopes_;
    bool scope_fetch_pending_ = false;
    std::optional<HighlightRequest> highlight_request_;

    std::mutex events_mutex_;
    std::deque<SessionIoEvent> events_;
};

}  // namespace tui_debug_ui
