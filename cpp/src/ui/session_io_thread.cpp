#include "tui_debug_ui/session_io_thread.hpp"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <utility>

namespace tui_debug_ui {
namespace {

constexpr auto kWorkerTick = std::chrono::milliseconds(50);
constexpr auto kBreakpointDebounce = std::chrono::milliseconds(150);

}  // namespace

SessionIoThread::SessionIoThread(SessionMode mode, DebugAdapter adapter)
    : backend_(create_session_backend(mode, adapter)) {
    thread_ = std::thread([this]() { thread_main(); });
}

SessionIoThread::~SessionIoThread() {
    stop_.store(true, std::memory_order_release);
    cv_.notify_all();
    join_launch_worker();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SessionIoThread::join_launch_worker() {
    if (launch_worker_.joinable()) {
        launch_worker_.join();
    }
}

void SessionIoThread::maybe_begin_launch() {
    if (launch_started_.load(std::memory_order_acquire)) {
        return;
    }

    std::string path;
    {
        std::lock_guard lock(mutex_);
        if (program_path_.empty()) {
            return;
        }
        path = program_path_;
        launch_started_.store(true, std::memory_order_release);
    }

    std::vector<std::string> args;
    {
        std::lock_guard lock(mutex_);
        args = program_args_;
    }

    launch_worker_ = std::thread([this, path = std::move(path), args = std::move(args)]() {
        try {
            backend_->launch(path, args);
            const bool active = backend_->is_active();
            backend_active_.store(active, std::memory_order_release);
            adapter_live_.store(active, std::memory_order_release);
            if (active) {
                sync_initial_state();
            }
            std::string launch_error;
            if (!active) {
                launch_error = backend_->last_error();
                if (launch_error.empty()) {
                    launch_error = "Failed to launch debug session";
                }
            }
            push_event(SessionIoEvent{SessionIoEventKind::LaunchFinished, active, std::move(launch_error)});
            launch_finished_.store(true, std::memory_order_release);
        } catch (const std::exception& ex) {
            push_event(SessionIoEvent{SessionIoEventKind::LaunchFinished, false, ex.what()});
            launch_finished_.store(true, std::memory_order_release);
        } catch (...) {
            push_event(SessionIoEvent{SessionIoEventKind::LaunchFinished, false,
                                      "Failed to launch debug session"});
            launch_finished_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
    });
}

void SessionIoThread::start_launch(const std::string& program_path,
                                   const std::vector<std::string>& program_args) {
    {
        std::lock_guard lock(mutex_);
        program_path_ = program_path;
        program_args_ = program_args;
    }
    cv_.notify_all();
}

bool SessionIoThread::launch_finished() const {
    return launch_finished_.load(std::memory_order_acquire);
}

bool SessionIoThread::is_active() const {
    return backend_active_.load(std::memory_order_acquire);
}

bool SessionIoThread::adapter_live() const {
    return adapter_live_.load(std::memory_order_acquire);
}

void SessionIoThread::post_terminal_input(const std::string& bytes) {
    if (bytes.empty()) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        pending_terminal_input_ += bytes;
    }
    cv_.notify_all();
}

void SessionIoThread::post_command(const std::string& op) {
    {
        std::lock_guard lock(mutex_);
        pending_command_ = op;
        has_pending_command_ = true;
        if (command_preempts_background_work(op)) {
            pending_evaluate_.reset();
            scope_fetch_pending_ = false;
            scope_fetch_signature_.clear();
            scope_fetch_scopes_.clear();
            variable_children_fetch_queue_.clear();
            scope_fetch_aborted_.store(true, std::memory_order_release);
        }
    }
    cv_.notify_all();
}

void SessionIoThread::post_evaluate(const std::string& expression, std::int64_t frame_id,
                                    const std::string& context) {
    {
        std::lock_guard lock(mutex_);
        if (has_pending_command_ && command_preempts_background_work(pending_command_)) {
            return;
        }
        pending_evaluate_ = expression;
        pending_evaluate_frame_ = frame_id;
        pending_evaluate_context_ = context;
    }
    cv_.notify_all();
}

void SessionIoThread::post_set_variable(std::int64_t variables_reference, const std::string& name,
                                        const std::string& value) {
    {
        std::lock_guard lock(mutex_);
        pending_set_variable_ = PendingSetVariable{variables_reference, name, value};
    }
    cv_.notify_all();
}

void SessionIoThread::post_set_breakpoints(const std::string& path, const std::string& lines_json) {
    {
        std::lock_guard lock(mutex_);
        pending_breakpoints_path_ = path;
        pending_breakpoints_json_ = lines_json;
        breakpoints_posted_at_ = std::chrono::steady_clock::now();
    }
    cv_.notify_all();
}

void SessionIoThread::request_data_breakpoint_info(std::int64_t variables_reference, std::int64_t frame_id,
                                                   const std::string& name, const std::string& access_type) {
    if (variables_reference <= 0 || name.empty()) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        data_breakpoint_info_request_ =
            DataBreakpointInfoRequest{variables_reference, frame_id, name, access_type};
    }
    cv_.notify_all();
}

void SessionIoThread::post_set_data_breakpoints(const std::string& breakpoints_json) {
    {
        std::lock_guard lock(mutex_);
        pending_data_breakpoints_json_ = breakpoints_json;
        data_breakpoints_posted_at_ = std::chrono::steady_clock::now();
    }
    cv_.notify_all();
}

void SessionIoThread::post_set_function_breakpoints(const std::string& breakpoints_json) {
    {
        std::lock_guard lock(mutex_);
        pending_function_breakpoints_json_ = breakpoints_json;
        function_breakpoints_posted_at_ = std::chrono::steady_clock::now();
    }
    cv_.notify_all();
}

void SessionIoThread::post_set_exception_breakpoints(const std::string& filters_json) {
    {
        std::lock_guard lock(mutex_);
        pending_exception_breakpoints_json_ = filters_json;
        exception_breakpoints_posted_at_ = std::chrono::steady_clock::now();
    }
    cv_.notify_all();
}

void SessionIoThread::request_scope_variables(const std::string& signature,
                                              const std::vector<std::pair<std::int64_t, std::string>>& scopes) {
    {
        std::lock_guard lock(mutex_);
        scope_fetch_signature_ = signature;
        scope_fetch_scopes_ = scopes;
        scope_fetch_pending_ = true;
    }
    cv_.notify_all();
}

void SessionIoThread::request_variable_children(std::int64_t variables_reference, const std::string& path) {
    if (variables_reference <= 0 || path.empty()) {
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (std::find_if(variable_children_fetch_queue_.begin(), variable_children_fetch_queue_.end(),
                         [&](const VariableChildrenFetchRequest& request) {
                             return request.path == path;
                         }) != variable_children_fetch_queue_.end()) {
            return;
        }
        variable_children_fetch_queue_.push_back(VariableChildrenFetchRequest{variables_reference, path});
    }
    cv_.notify_all();
}

void SessionIoThread::request_highlight(const std::string& language, const std::string& source, int first_line,
                                        int line_count) {
    {
        std::lock_guard lock(mutex_);
        highlight_request_ = HighlightRequest{language, source, first_line, line_count};
    }
    cv_.notify_all();
}

void SessionIoThread::request_source_fetch(std::int64_t source_reference, const std::string& cache_key) {
    {
        std::lock_guard lock(mutex_);
        source_fetch_reference_ = source_reference;
        source_fetch_cache_key_ = cache_key;
    }
    cv_.notify_all();
}

bool SessionIoThread::try_pop_event(SessionIoEvent& out) {
    std::lock_guard lock(events_mutex_);
    if (events_.empty()) {
        return false;
    }
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

void SessionIoThread::push_event(SessionIoEvent event) {
    std::lock_guard lock(events_mutex_);
    events_.push_back(std::move(event));
}

bool SessionIoThread::command_needs_snapshot(const std::string& op) {
    const std::string name = command_op_name(op);
    return name == "step_over" || name == "next" || name == "step_into" || name == "step_in" || name == "step_out" ||
           name == "step_back" || name == "step_back_into" || name == "reverse_continue" || name == "restart" ||
           name == "goto" || name == "disconnect" || name == "terminate";
}

bool SessionIoThread::command_syncs_snapshot(const std::string& op) {
    const std::string name = command_op_name(op);
    return name == "continue" || name == "play_pause" || name == "pause" || name == "goto_line";
}

bool SessionIoThread::command_preempts_background_work(const std::string& op) {
    return command_needs_snapshot(op) || command_syncs_snapshot(op);
}

std::string SessionIoThread::command_op_name(const std::string& op_or_json) {
    if (op_or_json.empty() || op_or_json.front() != '{') {
        return op_or_json;
    }

    const std::string needle = "\"op\"";
    const std::size_t op_pos = op_or_json.find(needle);
    if (op_pos == std::string::npos) {
        return op_or_json;
    }
    const std::size_t colon = op_or_json.find(':', op_pos);
    const std::size_t quote = op_or_json.find('"', colon + 1);
    const std::size_t end = op_or_json.find('"', quote + 1);
    if (quote == std::string::npos || end == std::string::npos) {
        return op_or_json;
    }
    return op_or_json.substr(quote + 1, end - quote - 1);
}

void SessionIoThread::request_step_in_targets(std::int64_t frame_id) {
    {
        std::lock_guard lock(mutex_);
        step_in_targets_frame_ = frame_id;
    }
    cv_.notify_all();
}

void SessionIoThread::request_goto_targets(const std::string& path, int line, int column) {
    {
        std::lock_guard lock(mutex_);
        goto_targets_request_ = GotoTargetsRequest{path, line, column};
    }
    cv_.notify_all();
}

void SessionIoThread::process_step_in_targets_fetch() {
    if (!adapter_live_.load(std::memory_order_acquire)) {
        return;
    }

    if (has_pending_execution_command()) {
        return;
    }

    std::int64_t frame_id = 0;
    {
        std::lock_guard lock(mutex_);
        if (!step_in_targets_frame_.has_value()) {
            return;
        }
        frame_id = *step_in_targets_frame_;
        step_in_targets_frame_.reset();
    }

    std::string json;
    std::string error;
    const bool ok = backend_->fetch_step_in_targets(frame_id, json, error);
    SessionIoEvent event{SessionIoEventKind::StepInTargetsReady, ok, ok ? std::move(json) : std::move(error),
                         std::to_string(frame_id)};
    push_event(std::move(event));
}

void SessionIoThread::process_goto_targets_fetch() {
    if (!adapter_live_.load(std::memory_order_acquire)) {
        return;
    }

    if (has_pending_execution_command()) {
        return;
    }

    GotoTargetsRequest request{};
    {
        std::lock_guard lock(mutex_);
        if (!goto_targets_request_.has_value()) {
            return;
        }
        request = std::move(*goto_targets_request_);
        goto_targets_request_.reset();
    }

    std::string json;
    std::string error;
    const bool ok = backend_->fetch_goto_targets(request.path, request.line, request.column, 0, json, error);
    SessionIoEvent event{SessionIoEventKind::GotoTargetsReady,
                         ok,
                         ok ? std::move(json) : std::move(error),
                         std::to_string(request.line)};
    event.detail = request.path;
    event.scope_ref = request.line;
    push_event(std::move(event));
}

void SessionIoThread::sync_initial_state() {
    if (const auto json = backend_->sync_snapshot_json()) {
        push_event(SessionIoEvent{SessionIoEventKind::SnapshotJson, true, *json});
    }

    if (const auto json = backend_->drain_console_json()) {
        push_event(SessionIoEvent{SessionIoEventKind::ConsoleJson, true, *json});
    }
}

void SessionIoThread::clear_pending_adapter_work() {
    std::lock_guard lock(mutex_);
    pending_evaluate_.reset();
    pending_set_variable_.reset();
    pending_breakpoints_path_.reset();
    pending_breakpoints_json_.reset();
    scope_fetch_pending_ = false;
    scope_fetch_signature_.clear();
    scope_fetch_scopes_.clear();
    variable_children_fetch_queue_.clear();
    highlight_request_.reset();
    source_fetch_reference_.reset();
    source_fetch_cache_key_.clear();
}

bool SessionIoThread::has_pending_command() const {
    std::lock_guard lock(mutex_);
    return has_pending_command_;
}

bool SessionIoThread::has_pending_execution_command() const {
    std::lock_guard lock(mutex_);
    return has_pending_command_ && command_preempts_background_work(pending_command_);
}

void SessionIoThread::process_scope_fetch() {
    if (!adapter_live_.load(std::memory_order_acquire)) {
        return;
    }

    if (has_pending_execution_command()) {
        return;
    }

    std::string signature;
    std::vector<std::pair<std::int64_t, std::string>> scopes;
    {
        std::lock_guard lock(mutex_);
        if (!scope_fetch_pending_) {
            return;
        }
        signature = scope_fetch_signature_;
        scopes = scope_fetch_scopes_;
        scope_fetch_pending_ = false;
    }

    scope_fetch_aborted_.store(false, std::memory_order_release);

    auto requeue_scope_fetch = [&]() {
        std::lock_guard lock(mutex_);
        scope_fetch_pending_ = true;
        scope_fetch_signature_ = signature;
        scope_fetch_scopes_ = scopes;
    };

    std::ostringstream json;
    json << "{\"signature\":\"" << signature << "\",\"variables\":{";
    bool first_scope = true;
    for (const auto& [variables_reference, scope_name] : scopes) {
        (void)scope_name;
        if (has_pending_execution_command() || scope_fetch_aborted_.load(std::memory_order_acquire)) {
            requeue_scope_fetch();
            return;
        }
        if (variables_reference <= 0) {
            continue;
        }
        const auto vars_json = backend_->fetch_variables_json(variables_reference, scope_name);
        if (has_pending_execution_command() || scope_fetch_aborted_.load(std::memory_order_acquire)) {
            requeue_scope_fetch();
            return;
        }
        if (!vars_json.has_value()) {
            continue;
        }
        if (!first_scope) {
            json << ',';
        }
        first_scope = false;
        json << '"' << variables_reference << "\":" << *vars_json;
    }
    json << "}}";

    push_event(SessionIoEvent{SessionIoEventKind::ScopeVariablesReady, true, json.str(), signature});
}

void SessionIoThread::process_variable_children_fetch() {
    if (!adapter_live_.load(std::memory_order_acquire)) {
        return;
    }

    if (has_pending_execution_command()) {
        return;
    }

    VariableChildrenFetchRequest request{};
    {
        std::lock_guard lock(mutex_);
        if (variable_children_fetch_queue_.empty()) {
            return;
        }
        request = variable_children_fetch_queue_.front();
        variable_children_fetch_queue_.pop_front();
    }

    if (has_pending_execution_command() || scope_fetch_aborted_.load(std::memory_order_acquire)) {
        std::lock_guard lock(mutex_);
        variable_children_fetch_queue_.push_front(request);
        return;
    }

    const auto vars_json = backend_->fetch_variables_json(request.variables_reference, {});
    if (has_pending_execution_command() || scope_fetch_aborted_.load(std::memory_order_acquire)) {
        std::lock_guard lock(mutex_);
        variable_children_fetch_queue_.push_front(request);
        return;
    }

    if (!vars_json.has_value()) {
        push_event(SessionIoEvent{SessionIoEventKind::VariableChildrenReady, false, {}, request.path,
                                 request.variables_reference});
        return;
    }

    push_event(SessionIoEvent{SessionIoEventKind::VariableChildrenReady, true, *vars_json, request.path,
                             request.variables_reference});
}

void SessionIoThread::process_source_fetch() {
    if (has_pending_execution_command()) {
        return;
    }

    std::int64_t source_reference = 0;
    std::string cache_key;
    {
        std::lock_guard lock(mutex_);
        if (!source_fetch_reference_.has_value()) {
            return;
        }
        source_reference = *source_fetch_reference_;
        cache_key = source_fetch_cache_key_;
        source_fetch_reference_.reset();
        source_fetch_cache_key_.clear();
    }

    if (!adapter_live_.load(std::memory_order_acquire)) {
        push_event(SessionIoEvent{SessionIoEventKind::SourceReady, false, {}, cache_key, source_reference});
        return;
    }

    const auto source = backend_->fetch_source(source_reference);
    push_event(SessionIoEvent{SessionIoEventKind::SourceReady, source.has_value(),
                              source.value_or(std::string{}), cache_key, source_reference});
}

void SessionIoThread::process_highlight_request() {
    HighlightRequest request;
    {
        std::lock_guard lock(mutex_);
        if (!highlight_request_.has_value()) {
            return;
        }
        request = *highlight_request_;
        highlight_request_.reset();
    }

    std::string error;
    const auto json = backend_->highlight_viewport(request.language, request.source, request.first_line,
                                                   request.line_count, error);
    SessionIoEvent event{
        .kind = SessionIoEventKind::HighlightReady,
        .success = json.has_value(),
        .payload = json.value_or(std::string{}),
        .detail = error,
        .highlight_first_line = request.first_line,
        .highlight_line_count = request.line_count,
    };
    push_event(std::move(event));
}

void SessionIoThread::process_data_breakpoint_info_fetch() {
    if (!adapter_live_.load(std::memory_order_acquire) || has_pending_execution_command()) {
        return;
    }

    std::optional<DataBreakpointInfoRequest> request;
    {
        std::lock_guard lock(mutex_);
        if (!data_breakpoint_info_request_.has_value()) {
            return;
        }
        request = std::move(data_breakpoint_info_request_);
        data_breakpoint_info_request_.reset();
    }

    std::string error;
    std::string json;
    const bool ok = backend_->data_breakpoint_info(request->variables_reference, request->frame_id, request->name,
                                                   json, error);
    push_event(SessionIoEvent{
        .kind = SessionIoEventKind::DataBreakpointInfoReady,
        .success = ok,
        .payload = ok ? std::move(json) : std::string{},
        .detail = ok ? request->access_type : std::move(error),
        .scope_ref = request->variables_reference,
    });
}

void SessionIoThread::process_pending_data_breakpoints() {
    if (!adapter_live_.load(std::memory_order_acquire) || has_pending_execution_command()) {
        return;
    }

    std::optional<std::string> json;
    {
        std::lock_guard lock(mutex_);
        if (!pending_data_breakpoints_json_.has_value()) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - data_breakpoints_posted_at_;
        if (elapsed < kBreakpointDebounce) {
            return;
        }
        json = std::move(pending_data_breakpoints_json_);
        pending_data_breakpoints_json_.reset();
    }

    std::string error;
    std::string results;
    const bool ok = backend_->set_data_breakpoints(*json, error, results);
    push_event(SessionIoEvent{SessionIoEventKind::DataBreakpointsFinished, ok,
                              ok ? std::move(results) : std::string{}, ok ? std::move(*json) : std::move(error)});
}

void SessionIoThread::process_pending_function_breakpoints() {
    if (!adapter_live_.load(std::memory_order_acquire) || has_pending_execution_command()) {
        return;
    }

    std::optional<std::string> json;
    {
        std::lock_guard lock(mutex_);
        if (!pending_function_breakpoints_json_.has_value()) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - function_breakpoints_posted_at_;
        if (elapsed < kBreakpointDebounce) {
            return;
        }
        json = std::move(pending_function_breakpoints_json_);
        pending_function_breakpoints_json_.reset();
    }

    std::string error;
    std::string results;
    const bool ok = backend_->set_function_breakpoints(*json, error, results);
    push_event(SessionIoEvent{SessionIoEventKind::FunctionBreakpointsFinished, ok,
                              ok ? std::move(results) : std::string{}, ok ? std::move(*json) : std::move(error)});
}

void SessionIoThread::process_pending_exception_breakpoints() {
    if (!adapter_live_.load(std::memory_order_acquire) || has_pending_execution_command()) {
        return;
    }

    std::optional<std::string> json;
    {
        std::lock_guard lock(mutex_);
        if (!pending_exception_breakpoints_json_.has_value()) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - exception_breakpoints_posted_at_;
        if (elapsed < kBreakpointDebounce) {
            return;
        }
        json = std::move(pending_exception_breakpoints_json_);
        pending_exception_breakpoints_json_.reset();
    }

    std::string error;
    const bool ok = backend_->set_exception_breakpoints(*json, error);
    push_event(SessionIoEvent{SessionIoEventKind::ExceptionBreakpointsFinished, ok, {},
                              ok ? std::move(*json) : std::move(error)});
}

void SessionIoThread::process_pending_breakpoints() {
    if (!adapter_live_.load(std::memory_order_acquire)) {
        return;
    }

    if (has_pending_execution_command()) {
        return;
    }

    std::optional<std::string> path;
    std::optional<std::string> json;
    {
        std::lock_guard lock(mutex_);
        if (!pending_breakpoints_path_.has_value() || !pending_breakpoints_json_.has_value()) {
            return;
        }
        const auto elapsed = std::chrono::steady_clock::now() - breakpoints_posted_at_;
        if (elapsed < kBreakpointDebounce) {
            return;
        }
        path = std::move(pending_breakpoints_path_);
        json = std::move(pending_breakpoints_json_);
        pending_breakpoints_path_.reset();
        pending_breakpoints_json_.reset();
    }

    std::string error;
    std::string results;
    const bool ok = backend_->set_breakpoints(*path, *json, error, results);
    push_event(SessionIoEvent{SessionIoEventKind::BreakpointsFinished, ok,
                              ok ? std::move(results) : std::string{},
                              ok ? std::move(*path) : std::move(error)});
}

void SessionIoThread::dispatch_command(const std::string& op) {
    const std::string op_name = command_op_name(op);
    std::string error;
    const bool ok = backend_->send_command(op, error);
    if (!ok) {
        push_event(SessionIoEvent{SessionIoEventKind::CommandFinished, false, op_name, error});
        return;
    }

    if (op_name == "disconnect") {
        adapter_live_.store(false, std::memory_order_release);
        clear_pending_adapter_work();
    } else if (op_name == "restart") {
        adapter_live_.store(true, std::memory_order_release);
        clear_pending_adapter_work();
    }

    if (command_needs_snapshot(op) || command_syncs_snapshot(op)) {
        if (const auto json = backend_->sync_snapshot_json()) {
            push_event(SessionIoEvent{SessionIoEventKind::SnapshotJson, true, *json});
        }
        if (const auto json = backend_->drain_console_json()) {
            push_event(SessionIoEvent{SessionIoEventKind::ConsoleJson, true, *json});
        }
    }

    push_event(SessionIoEvent{SessionIoEventKind::CommandFinished, true, op_name});
}

void SessionIoThread::process_preempting_commands() {
    std::string op;
    {
        std::lock_guard lock(mutex_);
        if (!has_pending_command_ || !command_preempts_background_work(pending_command_)) {
            return;
        }
        op = std::move(pending_command_);
        has_pending_command_ = false;
        pending_evaluate_.reset();
        scope_fetch_pending_ = false;
        scope_fetch_signature_.clear();
        scope_fetch_scopes_.clear();
    }

    if (!adapter_live_.load(std::memory_order_acquire)) {
        push_event(SessionIoEvent{SessionIoEventKind::CommandFinished, false, op, "debug session disconnected"});
        return;
    }

    process_pending_set_variable();
    dispatch_command(op);
}

bool SessionIoThread::process_pending_set_variable() {
    std::optional<PendingSetVariable> set_variable_request;
    {
        std::lock_guard lock(mutex_);
        if (!pending_set_variable_.has_value()) {
            return false;
        }
        set_variable_request = std::move(pending_set_variable_);
        pending_set_variable_.reset();
    }

    std::string result;
    std::string error;
    bool ok = false;
    if (!adapter_live_.load(std::memory_order_acquire)) {
        error = "debug session disconnected";
    } else {
        ok = backend_->set_variable(set_variable_request->variables_reference, set_variable_request->name,
                                    set_variable_request->value, result, error);
    }
    push_event(SessionIoEvent{SessionIoEventKind::SetVariableFinished, ok,
                             ok ? std::move(result) : std::move(error), set_variable_request->name});
    return true;
}

void SessionIoThread::process_pending_command() {
    if (has_pending_execution_command()) {
        return;
    }

    if (process_pending_set_variable()) {
        return;
    }

    {
        std::lock_guard lock(mutex_);
        if (scope_fetch_pending_) {
            return;
        }
    }

    std::optional<std::string> evaluate_expr;
    std::int64_t evaluate_frame = 0;
    std::string evaluate_context;

    {
        std::lock_guard lock(mutex_);
        if (pending_evaluate_.has_value()) {
            evaluate_expr = std::move(pending_evaluate_);
            pending_evaluate_.reset();
            evaluate_frame = pending_evaluate_frame_;
            evaluate_context = std::move(pending_evaluate_context_);
        }
    }

    if (!evaluate_expr.has_value()) {
        return;
    }

    {
        std::lock_guard lock(mutex_);
        if (has_pending_command_ && command_preempts_background_work(pending_command_)) {
            pending_evaluate_ = std::move(evaluate_expr);
            pending_evaluate_frame_ = evaluate_frame;
            pending_evaluate_context_ = std::move(evaluate_context);
            return;
        }
    }

    std::string result;
    std::string error;
    bool ok = false;
    if (!adapter_live_.load(std::memory_order_acquire)) {
        error = "debug session disconnected";
    } else {
        ok = backend_->evaluate(*evaluate_expr, evaluate_frame, evaluate_context, result, error);
    }
    SessionIoEvent event{SessionIoEventKind::EvaluateFinished, ok, ok ? std::move(result) : std::move(error),
                         *evaluate_expr};
    push_event(std::move(event));
}

void SessionIoThread::process_terminal_input() {
    std::string bytes;
    {
        std::lock_guard lock(mutex_);
        if (pending_terminal_input_.empty()) {
            return;
        }
        bytes = std::move(pending_terminal_input_);
        pending_terminal_input_.clear();
    }

    if (!adapter_live_.load(std::memory_order_acquire)) {
        return;
    }

    std::string error;
    if (!backend_->terminal_write(bytes, error)) {
        push_event(SessionIoEvent{SessionIoEventKind::CommandFinished, false, "terminal_input", error});
    }
}

void SessionIoThread::run_poll_cycle() {
    if (!adapter_live_.load(std::memory_order_acquire)) {
        return;
    }

    std::string json_out;
    const int rc = backend_->poll_json(json_out);
    if (rc == 0) {
        push_event(SessionIoEvent{SessionIoEventKind::PollJson, true, std::move(json_out)});
    } else if (rc < 0) {
        push_event(SessionIoEvent{SessionIoEventKind::PollJson, false, {}, "Session poll failed"});
    }

    if (const auto json = backend_->drain_console_json()) {
        push_event(SessionIoEvent{SessionIoEventKind::ConsoleJson, true, *json});
    }
}

void SessionIoThread::thread_main() {
    while (!stop_.load(std::memory_order_acquire)) {
        try {
            maybe_begin_launch();
            process_highlight_request();

            if (adapter_live_.load(std::memory_order_acquire)) {
                process_source_fetch();
            }

            if (backend_active_.load(std::memory_order_acquire)) {
                process_terminal_input();
                process_preempting_commands();
                run_poll_cycle();
                process_preempting_commands();
                if (!has_pending_execution_command()) {
                    process_pending_command();
                }
                process_pending_breakpoints();
                process_pending_data_breakpoints();
                process_pending_function_breakpoints();
                process_pending_exception_breakpoints();
                if (!has_pending_execution_command()) {
                    process_data_breakpoint_info_fetch();
                }
                if (!has_pending_execution_command()) {
                    process_scope_fetch();
                }
                if (!has_pending_execution_command()) {
                    process_variable_children_fetch();
                }
                if (!has_pending_execution_command()) {
                    process_step_in_targets_fetch();
                }
                if (!has_pending_execution_command()) {
                    process_goto_targets_fetch();
                }
            }

            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, kWorkerTick, [this]() {
                return stop_.load(std::memory_order_acquire) || has_pending_command_ ||
                       pending_evaluate_.has_value() || pending_set_variable_.has_value() ||
                       (pending_breakpoints_path_.has_value() && pending_breakpoints_json_.has_value()) ||
                       scope_fetch_pending_ || !variable_children_fetch_queue_.empty() ||
                       highlight_request_.has_value() ||
                       source_fetch_reference_.has_value() || step_in_targets_frame_.has_value() ||
                       goto_targets_request_.has_value() ||
                       (!launch_started_.load(std::memory_order_acquire) && !program_path_.empty());
            });
        } catch (const std::exception& ex) {
            push_event(SessionIoEvent{SessionIoEventKind::CommandFinished, false, "worker", ex.what()});
        } catch (...) {
            push_event(SessionIoEvent{SessionIoEventKind::CommandFinished, false, "worker",
                                      "unexpected worker thread error"});
        }
    }

    join_launch_worker();
    backend_->shutdown();
    backend_active_.store(false, std::memory_order_release);
}

}  // namespace tui_debug_ui
