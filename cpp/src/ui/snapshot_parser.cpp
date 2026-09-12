#include "tui_debug_ui/snapshot_parser.hpp"

#include <cctype>
#include <string_view>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define TUI_DEBUG_UI_HAS_NLOHMANN_JSON 1
#endif

namespace tui_debug_ui {
namespace {

bool is_runtime_library_source_path(const std::string& path) {
    if (path.empty()) {
        return true;
    }
    if (path.rfind("dap:source:", 0) == 0) {
        return false;
    }
    return path.find(".so") != std::string::npos || path.find("/lib/") != std::string::npos ||
           path.find("/usr/lib") != std::string::npos || path.find("/lib64/") != std::string::npos;
}

const StackFrameInfo* preferred_user_stack_frame(const std::vector<StackFrameInfo>& frames) {
    for (const StackFrameInfo& frame : frames) {
        if (frame.line <= 0) {
            continue;
        }
        if (!frame.path.empty() && !is_runtime_library_source_path(frame.path)) {
            return &frame;
        }
        if (frame.path.empty() && frame.source_reference > 0) {
            return &frame;
        }
    }
    return frames.empty() ? nullptr : &frames.front();
}

void set_execution_from_frame(DebugUiModel& model, const StackFrameInfo& frame) {
    if (!frame.path.empty()) {
        model.execution_path = frame.path;
    } else if (frame.source_reference > 0) {
        model.execution_path = "dap:source:" + std::to_string(frame.source_reference);
    }
    model.execution_source_reference = frame.source_reference;
    if (frame.line > 0) {
        model.execution_line = static_cast<std::uint32_t>(frame.line);
    }
}

#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
using Json = nlohmann::json;

std::int64_t json_int64_or(const Json& object, const char* key, std::int64_t default_value) {
    if (!object.contains(key)) {
        return default_value;
    }
    const Json& value = object.at(key);
    if (value.is_null() || !value.is_number()) {
        return default_value;
    }
    return value.get<std::int64_t>();
}

std::string json_string_or(const Json& object, const char* key, std::string default_value = {}) {
    if (!object.contains(key)) {
        return default_value;
    }
    const Json& value = object.at(key);
    if (value.is_null() || !value.is_string()) {
        return default_value;
    }
    return value.get<std::string>();
}

bool parse_session_state(const Json& state, std::string& session_state, std::string& stop_reason) {
    if (state.is_string()) {
        const std::string value = state.get<std::string>();
        if (value == "Running") {
            session_state = "running";
            stop_reason.clear();
            return true;
        }
        if (value == "Exited") {
            session_state = "exited";
            stop_reason.clear();
            return true;
        }
        if (value == "Disconnected") {
            session_state = "disconnected";
            stop_reason.clear();
            return true;
        }
        return false;
    }

    if (state.is_object() && state.contains("Stopped") && state.at("Stopped").is_object()) {
        const Json& stopped = state.at("Stopped");
        session_state = "stopped";
        stop_reason = stopped.value("reason", std::string{});
        return true;
    }

    return false;
}

std::string status_message_for_state(const std::string& session_state, const std::string& stop_reason) {
    if (session_state == "stopped") {
        if (stop_reason.empty()) {
            return "Stopped";
        }
        return "Stopped (" + stop_reason + ")";
    }
    if (session_state == "running") {
        return "Running";
    }
    if (session_state == "exited") {
        return "Session ended";
    }
    if (session_state == "disconnected") {
        return "Disconnected";
    }
    return "Connected";
}

void apply_snapshot_object(DebugUiModel& model, const Json& snapshot) {
    if (snapshot.contains("state")) {
        const Json& state = snapshot.at("state");
        parse_session_state(state, model.session_state, model.stop_reason);
        model.status_message = status_message_for_state(model.session_state, model.stop_reason);
        if (state.is_object() && state.contains("Stopped") && state.at("Stopped").is_object()) {
            model.stopped_thread_id = state.at("Stopped").value("thread_id", static_cast<std::int64_t>(0));
        } else if (model.session_state != "stopped") {
            model.stopped_thread_id = 0;
            model.exception_info.reset();
        }
    }

    model.exception_info.reset();
    if (snapshot.contains("exception_info") && snapshot.at("exception_info").is_object()) {
        const Json& info = snapshot.at("exception_info");
        ExceptionInfo parsed{};
        parsed.exception_id = info.value("exceptionId", std::string{});
        parsed.break_mode = info.value("breakMode", std::string{});
        parsed.description = info.value("description", std::string{});
        if (info.contains("details") && info.at("details").is_object()) {
            const Json& details = info.at("details");
            parsed.type_name = details.value("typeName", std::string{});
            parsed.message = details.value("message", std::string{});
            parsed.evaluate_name = details.value("evaluateName", std::string{});
            parsed.stack_trace = details.value("stackTrace", std::string{});
        }
        if (!parsed.exception_id.empty() || !parsed.description.empty() || !parsed.message.empty()) {
            model.exception_info = std::move(parsed);
        }
    }

    if (snapshot.contains("capabilities") && snapshot.at("capabilities").is_object()) {
        const Json& capabilities = snapshot.at("capabilities");
        model.supports_step_back = capabilities.value("supports_step_back", false);
        model.supports_step_in_targets = capabilities.value("supports_step_in_targets", false);
        model.supports_goto_targets = capabilities.value("supports_goto_targets", false);
        model.supports_data_breakpoints = capabilities.value("supports_data_breakpoints", false);
        model.supports_function_breakpoints = capabilities.value("supports_function_breakpoints", false);
        model.supports_completions_request = capabilities.value("supports_completions_request", false);
        model.supports_read_memory_request = capabilities.value("supports_read_memory_request", false);
        model.supports_write_memory_request = capabilities.value("supports_write_memory_request", false);
        model.supports_disassemble_request = capabilities.value("supports_disassemble_request", false);

        model.exception_breakpoint_filters.clear();
        if (capabilities.contains("exception_breakpoint_filters") &&
            capabilities.at("exception_breakpoint_filters").is_array()) {
            for (const Json& filter : capabilities.at("exception_breakpoint_filters")) {
                if (!filter.is_object()) {
                    continue;
                }
                DebugUiModel::ExceptionBreakpointFilterInfo info{};
                info.filter = filter.value("filter", std::string{});
                if (info.filter.empty()) {
                    continue;
                }
                info.label = filter.value("label", info.filter);
                info.description = filter.value("description", std::string{});
                info.default_enabled = filter.value("default", false);
                info.supports_condition = filter.value("supports_condition", false);
                model.exception_breakpoint_filters.push_back(std::move(info));
            }
        }
    }

    model.threads.clear();
    if (snapshot.contains("threads") && snapshot.at("threads").is_array()) {
        for (const Json& thread : snapshot.at("threads")) {
            ThreadInfo info{};
            info.id = thread.value("id", static_cast<std::int64_t>(0));
            info.name = thread.value("name", std::string{});
            model.threads.push_back(std::move(info));
        }
    }

    auto parse_stack_frame = [](const Json& frame) {
        StackFrameInfo info{};
        info.id = frame.value("id", static_cast<std::int64_t>(0));
        info.name = frame.value("name", std::string{});
        info.line = frame.value("line", static_cast<std::int64_t>(0));

        if (frame.contains("source") && frame.at("source").is_object()) {
            const Json& source = frame.at("source");
            info.path = json_string_or(source, "path");
            info.source_reference = json_int64_or(source, "sourceReference", 0);
        }
        info.instruction_pointer_reference = frame.value("instructionPointerReference", std::string{});
        return info;
    };

    model.stack_frames.clear();
    if (snapshot.contains("stack_frames") && snapshot.at("stack_frames").is_array()) {
        for (const Json& frame : snapshot.at("stack_frames")) {
            model.stack_frames.push_back(parse_stack_frame(frame));
        }
    }

    model.thread_stacks.clear();
    if (snapshot.contains("thread_stacks") && snapshot.at("thread_stacks").is_array()) {
        for (const Json& entry : snapshot.at("thread_stacks")) {
            ThreadStackInfo stack{};
            stack.thread_id = entry.value("thread_id", static_cast<std::int64_t>(0));
            if (entry.contains("stack_frames") && entry.at("stack_frames").is_array()) {
                for (const Json& frame : entry.at("stack_frames")) {
                    stack.frames.push_back(parse_stack_frame(frame));
                }
            }
            model.thread_stacks.push_back(std::move(stack));
        }
    }

    model.scopes.clear();
    if (snapshot.contains("scopes") && snapshot.at("scopes").is_array()) {
        for (const Json& scope : snapshot.at("scopes")) {
            ScopeInfo info{};
            info.name = scope.value("name", std::string{});
            info.variables_reference = scope.value("variablesReference", static_cast<std::int64_t>(0));
            model.scopes.push_back(std::move(info));
        }
    }

    model.variables.clear();
    if (snapshot.contains("variables") && snapshot.at("variables").is_array()) {
        for (const Json& variable : snapshot.at("variables")) {
            VariableInfo info{};
            info.name = variable.value("name", std::string{});
            info.value = variable.value("value", std::string{});
            model.variables.push_back(std::move(info));
        }
    }

    if (snapshot.contains("console_lines") && snapshot.at("console_lines").is_array()) {
        model.console_lines.clear();
        for (const Json& line : snapshot.at("console_lines")) {
            ConsoleLine entry{};
            entry.category = line.value("category", std::string{"stdout"});
            entry.text = line.value("text", std::string{});
            model.console_lines.push_back(std::move(entry));
        }
    }

    if (const StackFrameInfo* frame = preferred_user_stack_frame(model.stack_frames); frame != nullptr) {
        set_execution_from_frame(model, *frame);
    }

    model.connection_state = ConnectionState::Connected;
}

bool apply_poll_json_impl(DebugUiModel& model, const Json& root) {
    if (!root.is_object() || !root.contains("snapshot")) {
        return false;
    }

    const std::string type = root.value("type", std::string{});
    if (type != "stopped" && type != "snapshot") {
        return false;
    }

    const Json& snapshot = root.at("snapshot");
    if (!snapshot.is_object()) {
        return false;
    }

    apply_snapshot_object(model, snapshot);
    return true;
}
#endif

std::string_view skip_ws(std::string_view input) {
    while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front())) != 0) {
        input.remove_prefix(1);
    }
    return input;
}

bool extract_json_string(std::string_view input, std::size_t& pos, std::string& out) {
    if (pos >= input.size() || input[pos] != '"') {
        return false;
    }
    ++pos;

    out.clear();
    while (pos < input.size()) {
        const char ch = input[pos++];
        if (ch == '"') {
            return true;
        }
        if (ch == '\\' && pos < input.size()) {
            out.push_back(input[pos++]);
            continue;
        }
        out.push_back(ch);
    }
    return false;
}

bool find_object_field(std::string_view json, std::string_view field, std::string_view& value_out) {
    const std::string needle = std::string{"\""} + std::string{field} + "\":";
    const std::size_t pos = json.find(needle);
    if (pos == std::string_view::npos) {
        return false;
    }

    std::size_t cursor = pos + needle.size();
    cursor = skip_ws(json.substr(cursor)).empty() ? cursor : cursor;
    while (cursor < json.size() && std::isspace(static_cast<unsigned char>(json[cursor])) != 0) {
        ++cursor;
    }
    if (cursor >= json.size()) {
        return false;
    }

    value_out = json.substr(cursor);
    return true;
}

bool manual_apply_poll_json(DebugUiModel& model, const std::string& json) {
    std::string_view envelope = json;
    std::string type;
    if (!find_object_field(envelope, "type", envelope)) {
        return false;
    }

    std::size_t type_pos = 0;
    if (!extract_json_string(envelope, type_pos, type)) {
        return false;
    }
    if (type != "stopped" && type != "snapshot") {
        return false;
    }

    std::string_view snapshot_json;
    if (!find_object_field(json, "snapshot", snapshot_json)) {
        return false;
    }

    std::string state_token;
    if (find_object_field(snapshot_json, "state", envelope)) {
        envelope = skip_ws(envelope);
        if (!envelope.empty() && envelope.front() == '"') {
            std::size_t state_pos = 0;
            if (extract_json_string(envelope, state_pos, state_token)) {
                if (state_token == "Running") {
                    model.session_state = "running";
                    model.stop_reason.clear();
                } else if (state_token == "Exited") {
                    model.session_state = "exited";
                    model.stop_reason.clear();
                } else if (state_token == "Disconnected") {
                    model.session_state = "disconnected";
                    model.stop_reason.clear();
                }
            }
        } else if (envelope.starts_with("{\"Stopped\"")) {
            model.session_state = "stopped";
            const std::string reason_needle = "\"reason\":\"";
            const std::size_t reason_pos = snapshot_json.find(reason_needle);
            if (reason_pos != std::string_view::npos) {
                std::size_t cursor = reason_pos + reason_needle.size();
                model.stop_reason.clear();
                while (cursor < snapshot_json.size() && snapshot_json[cursor] != '"') {
                    model.stop_reason.push_back(snapshot_json[cursor++]);
                }
            }
        }
    }

    const std::string frames_needle = "\"stack_frames\":[";
    const std::size_t frames_pos = snapshot_json.find(frames_needle);
    if (frames_pos != std::string_view::npos) {
        model.stack_frames.clear();
        StackFrameInfo frame{};

        const std::string line_needle = "\"line\":";
        const std::size_t line_pos = snapshot_json.find(line_needle, frames_pos);
        if (line_pos != std::string_view::npos) {
            std::size_t cursor = line_pos + line_needle.size();
            while (cursor < snapshot_json.size() && std::isspace(static_cast<unsigned char>(snapshot_json[cursor])) != 0) {
                ++cursor;
            }
            frame.line = 0;
            while (cursor < snapshot_json.size() && std::isdigit(static_cast<unsigned char>(snapshot_json[cursor])) != 0) {
                frame.line = frame.line * 10 + (snapshot_json[cursor++] - '0');
            }
        }

        const std::string path_needle = "\"path\":\"";
        const std::size_t path_pos = snapshot_json.find(path_needle, frames_pos);
        if (path_pos != std::string_view::npos) {
            std::size_t cursor = path_pos + path_needle.size();
            while (cursor < snapshot_json.size() && snapshot_json[cursor] != '"') {
                frame.path.push_back(snapshot_json[cursor++]);
            }
        }

        const std::string source_ref_needle = "\"sourceReference\":";
        const std::size_t source_ref_pos = snapshot_json.find(source_ref_needle, frames_pos);
        if (source_ref_pos != std::string_view::npos) {
            std::size_t cursor = source_ref_pos + source_ref_needle.size();
            while (cursor < snapshot_json.size() &&
                   std::isspace(static_cast<unsigned char>(snapshot_json[cursor])) != 0) {
                ++cursor;
            }
            frame.source_reference = 0;
            while (cursor < snapshot_json.size() && std::isdigit(static_cast<unsigned char>(snapshot_json[cursor])) != 0) {
                frame.source_reference =
                    frame.source_reference * 10 + (snapshot_json[cursor++] - '0');
            }
        }

        model.stack_frames.push_back(std::move(frame));
        if (const StackFrameInfo* preferred = preferred_user_stack_frame(model.stack_frames);
            preferred != nullptr) {
            set_execution_from_frame(model, *preferred);
        }
    }

    if (model.session_state == "stopped") {
        model.status_message = model.stop_reason.empty() ? "Stopped" : "Stopped (" + model.stop_reason + ")";
    } else if (model.session_state == "running") {
        model.status_message = "Running";
    } else if (model.session_state == "exited") {
        model.status_message = "Session ended";
    } else {
        model.status_message = "Connected";
    }

    const std::string capabilities_needle = "\"capabilities\":{";
    const std::size_t capabilities_pos = snapshot_json.find(capabilities_needle);
    if (capabilities_pos != std::string_view::npos) {
        if (snapshot_json.find("\"supports_step_back\":true", capabilities_pos) != std::string_view::npos) {
            model.supports_step_back = true;
        } else if (snapshot_json.find("\"supports_step_back\":false", capabilities_pos) != std::string_view::npos) {
            model.supports_step_back = false;
        }
        if (snapshot_json.find("\"supports_step_in_targets\":true", capabilities_pos) != std::string_view::npos) {
            model.supports_step_in_targets = true;
        } else if (snapshot_json.find("\"supports_step_in_targets\":false", capabilities_pos) !=
                   std::string_view::npos) {
            model.supports_step_in_targets = false;
        }
        if (snapshot_json.find("\"supports_goto_targets\":true", capabilities_pos) != std::string_view::npos) {
            model.supports_goto_targets = true;
        } else if (snapshot_json.find("\"supports_goto_targets\":false", capabilities_pos) !=
                   std::string_view::npos) {
            model.supports_goto_targets = false;
        }
    }

    model.connection_state = ConnectionState::Connected;
    return true;
}

}  // namespace

std::vector<BreakpointHitUpdate> parse_breakpoint_hits_from_poll_json(const std::string& json) {
#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    try {
        const Json root = Json::parse(json);
        if (!root.is_object() || !root.contains("snapshot") || !root.at("snapshot").is_object()) {
            return {};
        }
        const Json& snapshot = root.at("snapshot");
        if (!snapshot.contains("breakpoint_hits") || !snapshot.at("breakpoint_hits").is_array()) {
            return {};
        }

        std::vector<BreakpointHitUpdate> updates;
        for (const Json& hit : snapshot.at("breakpoint_hits")) {
            if (!hit.is_object()) {
                continue;
            }
            BreakpointHitUpdate update{};
            update.path = hit.value("path", std::string{});
            update.line = static_cast<int>(hit.value("line", static_cast<std::int64_t>(0)));
            update.hit_count = static_cast<std::uint64_t>(hit.value("hit_count", static_cast<std::uint64_t>(0)));
            if (!update.path.empty() && update.line > 0) {
                updates.push_back(std::move(update));
            }
        }
        return updates;
    } catch (const Json::exception&) {
        return {};
    }
#else
    (void)json;
    return {};
#endif
}

bool apply_poll_json(DebugUiModel& model, const std::string& json) {
#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    try {
        const Json root = Json::parse(json);
        return apply_poll_json_impl(model, root);
    } catch (const Json::exception&) {
        return false;
    }
#else
    return manual_apply_poll_json(model, json);
#endif
}

bool apply_console_json(DebugUiModel& model, const std::string& json) {
#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    try {
        const Json root = Json::parse(json);
        if (!root.is_array()) {
            return false;
        }

        for (const Json& line : root) {
            ConsoleLine entry{};
            entry.category = line.value("category", std::string{"stdout"});
            entry.text = line.value("text", std::string{});
            model.console_lines.push_back(std::move(entry));
        }
        return true;
    } catch (const Json::exception&) {
        return false;
    }
#else
    (void)model;
    (void)json;
    return false;
#endif
}

std::vector<VariableInfo> parse_variables_json(const std::string& json) {
#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    try {
        const Json root = Json::parse(json);
        if (!root.is_array()) {
            return {};
        }

        std::vector<VariableInfo> variables;
        variables.reserve(root.size());
        for (const Json& entry : root) {
            VariableInfo info{};
            info.name = entry.value("name", std::string{});
            info.value = entry.value("value", std::string{});
            info.variables_reference = json_int64_or(entry, "variablesReference", 0);
            variables.push_back(std::move(info));
        }
        return variables;
    } catch (const Json::exception&) {
        return {};
    }
#else
    (void)json;
    return {};
#endif
}

std::optional<std::string> parse_set_variable_result_value(const std::string& json) {
#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    try {
        const Json root = Json::parse(json);
        if (root.contains("value") && root.at("value").is_string()) {
            return root.at("value").get<std::string>();
        }
    } catch (const Json::exception&) {
    }
#else
    (void)json;
#endif
    return std::nullopt;
}

bool apply_scope_variables_batch(DebugUiModel& model, const std::string& signature, const std::string& json) {
#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    try {
        const Json root = Json::parse(json);
        if (root.value("signature", std::string{}) != signature) {
            return false;
        }
        const Json& variables = root.at("variables");
        if (!variables.is_object()) {
            return false;
        }

        for (auto it = variables.begin(); it != variables.end(); ++it) {
            const std::int64_t reference = std::stoll(it.key());
            model.scope_variables[reference] = parse_variables_json(it.value().dump());
        }
        return true;
    } catch (const Json::exception&) {
        return false;
    } catch (const std::exception&) {
        return false;
    }
#else
    (void)model;
    (void)signature;
    (void)json;
    return false;
#endif
}

}  // namespace tui_debug_ui
