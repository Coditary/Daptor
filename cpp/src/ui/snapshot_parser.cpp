#include "tui_debug_ui/snapshot_parser.hpp"

#include <cctype>
#include <string_view>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define TUI_DEBUG_UI_HAS_NLOHMANN_JSON 1
#endif

namespace tui_debug_ui {
namespace {

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
        }
    }

    if (snapshot.contains("capabilities") && snapshot.at("capabilities").is_object()) {
        const Json& capabilities = snapshot.at("capabilities");
        model.supports_step_back = capabilities.value("supports_step_back", false);
        model.supports_step_in_targets = capabilities.value("supports_step_in_targets", false);
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

    if (!model.stack_frames.empty()) {
        const StackFrameInfo& frame = model.stack_frames.front();
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
        if (!model.stack_frames.empty()) {
            const StackFrameInfo& top = model.stack_frames.front();
            if (!top.path.empty()) {
                model.execution_path = top.path;
            } else if (top.source_reference > 0) {
                model.execution_path = "dap:source:" + std::to_string(top.source_reference);
            }
            model.execution_source_reference = top.source_reference;
            if (top.line > 0) {
                model.execution_line = static_cast<std::uint32_t>(top.line);
            }
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
    }

    model.connection_state = ConnectionState::Connected;
    return true;
}

}  // namespace

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

        model.scope_variables.clear();
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
