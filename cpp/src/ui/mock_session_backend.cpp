#include "tui_debug_ui/session_backend.hpp"
#include "tui_debug_ui/step_in_selection.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <vector>

namespace tui_debug_ui {
namespace {

constexpr int kMockLines[] = {1, 3, 4, 8, 9, 12, 13, 17, 22};
constexpr int kMockLineCount = static_cast<int>(sizeof(kMockLines) / sizeof(kMockLines[0]));

int mock_line_index(int line) {
    for (int index = 0; index < kMockLineCount; ++index) {
        if (kMockLines[index] == line) {
            return index;
        }
    }
    return 0;
}

std::string read_file(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

std::string escape_json(const std::string& value) {
    std::string out;
    out.reserve(value.size());
    for (char ch : value) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

std::string mock_variables_json(const char* prefix, int count, int value_offset = 0) {
    std::ostringstream json;
    json << '[';
    for (int index = 0; index < count; ++index) {
        if (index > 0) {
            json << ',';
        }
        const int value = index + value_offset;
        json << R"({"name":")" << prefix << index << R"(","value":")" << value << R"(","variablesReference":0})";
    }
    json << ']';
    return json.str();
}

std::string line_text_at(const std::string& text, int line_number) {
    if (line_number < 1) {
        return {};
    }

    int current = 1;
    for (std::size_t index = 0; index < text.size();) {
        const std::size_t end = text.find('\n', index);
        if (current == line_number) {
            if (end == std::string::npos) {
                return text.substr(index);
            }
            return text.substr(index, end - index);
        }
        if (end == std::string::npos) {
            break;
        }
        index = end + 1;
        ++current;
    }
    return {};
}

bool mock_line_is_jump_target(const std::string& line_text) {
    std::string trimmed = line_text;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front())) != 0) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back())) != 0) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        return false;
    }
    if (trimmed == "{" || trimmed == "}" || trimmed == "};") {
        return false;
    }
    if (trimmed.rfind("//", 0) == 0 || trimmed.front() == '#') {
        return false;
    }
    return true;
}

std::optional<std::int64_t> json_target_id(const std::string& json) {
    const std::size_t key = json.find("\"target_id\"");
    if (key == std::string::npos) {
        return std::nullopt;
    }
    const std::size_t colon = json.find(':', key);
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    std::size_t index = colon + 1;
    while (index < json.size() && std::isspace(static_cast<unsigned char>(json[index])) != 0) {
        ++index;
    }
    if (index >= json.size() || (json[index] != '-' && !std::isdigit(static_cast<unsigned char>(json[index])))) {
        return std::nullopt;
    }
    try {
        return std::stoll(json.substr(index));
    } catch (...) {
        return std::nullopt;
    }
}

}  // namespace

class MockSessionBackend final : public SessionBackend {
  public:
    void launch(const std::string& program_path, const std::vector<std::string>& program_args) override {
        program_path_ = program_path;
        program_args_ = program_args;
        source_text_ = read_file(program_path);
        current_line_ = 1;
        step_index_ = 0;
        if (program_path.find("step_in_demo") != std::string::npos) {
            current_line_ = 17;
            step_index_ = mock_line_index(current_line_);
        }
        breakpoints_.clear();
        console_pending_.clear();
        stopped_ = true;
        session_state_ = "stopped";
        console_pending_.push_back(
            R"json([{"category":"telemetry","text":"ptvsd (mock)"},{"category":"telemetry","text":"debugpy (mock)"}])json");
        launched_ = true;
    }

    void shutdown() override {
        launched_ = false;
        session_state_ = "disconnected";
    }

    bool is_active() const override { return launched_; }

    std::optional<std::string> sync_snapshot_json() override {
        if (!launched_) {
            return std::nullopt;
        }
        return build_snapshot();
    }

    int poll_json(std::string& json_out) override {
        (void)json_out;
        return 1;
    }

    std::optional<std::string> drain_console_json() override {
        if (console_pending_.empty()) {
            return std::nullopt;
        }
        std::string json = console_pending_.front();
        console_pending_.erase(console_pending_.begin());
        return json;
    }

    bool send_command(const std::string& op, std::string& error_out) override {
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }

        std::string command = op;
        if (!op.empty() && op.front() == '{') {
            if (op.find("\"step_into\"") != std::string::npos ||
                op.find("\"step_in\"") != std::string::npos) {
                command = "step_over";
            } else if (op.find("\"op\"") != std::string::npos) {
                const std::size_t op_pos = op.find("\"op\"");
                const std::size_t colon = op.find(':', op_pos);
                const std::size_t quote = op.find('"', colon + 1);
                const std::size_t end = op.find('"', quote + 1);
                if (quote != std::string::npos && end != std::string::npos) {
                    command = op.substr(quote + 1, end - quote - 1);
                }
            }
        }

        if (session_state_ == "exited" || session_state_ == "disconnected") {
            if (op == "restart") {
                launch(program_path_, program_args_);
                return true;
            }
            if (op == "terminate") {
                session_state_ = "exited";
                stopped_ = true;
                return true;
            }
            if (op == "disconnect") {
                session_state_ = "disconnected";
                stopped_ = true;
                return true;
            }
            error_out = "mock session ended";
            return false;
        }

        if (command == "continue" || command == "play_pause") {
            if (stopped_) {
                step_index_ = static_cast<int>(sizeof(kMockLines) / sizeof(kMockLines[0])) - 1;
                current_line_ = kMockLines[step_index_];
                stopped_ = false;
                session_state_ = "running";
            } else {
                stopped_ = true;
                session_state_ = "stopped";
            }
            return true;
        }
        if (command == "step_over" || command == "next") {
            if (step_index_ + 1 < static_cast<int>(sizeof(kMockLines) / sizeof(kMockLines[0]))) {
                ++step_index_;
                current_line_ = kMockLines[step_index_];
            } else {
                session_state_ = "exited";
                stopped_ = true;
                return true;
            }
            stopped_ = true;
            session_state_ = "stopped";
            return true;
        }
        if (command == "step_into" || command == "step_in") {
            return send_command("step_over", error_out);
        }
        if (command == "step_out") {
            return send_command("step_over", error_out);
        }
        if (command == "reverse_continue") {
            if (step_index_ <= 0) {
                error_out = "already at earliest mock stop";
                return false;
            }
            step_index_ = 0;
            current_line_ = kMockLines[step_index_];
            stopped_ = true;
            session_state_ = "stopped";
            return true;
        }
        if (command == "step_back" || command == "step_back_into") {
            if (step_index_ <= 0) {
                error_out = "already at earliest mock stop";
                return false;
            }
            --step_index_;
            current_line_ = kMockLines[step_index_];
            stopped_ = true;
            session_state_ = "stopped";
            return true;
        }
        if (command == "pause") {
            stopped_ = true;
            session_state_ = "stopped";
            return true;
        }
        if (command == "terminate") {
            session_state_ = "exited";
            stopped_ = true;
            return true;
        }
        if (command == "disconnect") {
            session_state_ = "disconnected";
            stopped_ = true;
            return true;
        }
        if (command == "restart") {
            current_line_ = 1;
            step_index_ = 0;
            if (program_path_.find("step_in_demo") != std::string::npos) {
                current_line_ = 17;
                step_index_ = mock_line_index(current_line_);
            }
            stopped_ = true;
            session_state_ = "stopped";
            return true;
        }
        if (command == "goto") {
            if (!stopped_) {
                error_out = "cannot goto while program is running";
                return false;
            }
            const std::optional<std::int64_t> target_id =
                (!op.empty() && op.front() == '{') ? json_target_id(op) : std::nullopt;
            if (!target_id.has_value() || *target_id <= 0) {
                error_out = "goto requires target_id";
                return false;
            }
            current_line_ = static_cast<int>(*target_id);
            step_index_ = mock_line_index(current_line_);
            stopped_ = true;
            session_state_ = "stopped";
            return true;
        }

        error_out = "unknown mock command: " + command;
        return false;
    }

    bool fetch_step_in_targets(std::int64_t frame_id, std::string& json_out,
                               std::string& error_out) override {
        (void)frame_id;
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        if (!stopped_) {
            error_out = "cannot resolve step-in targets while running";
            return false;
        }

        const std::string line_text = line_text_at(source_text_, current_line_);
        if (line_text.empty()) {
            json_out = "[]";
            return true;
        }

        const std::vector<StepInTargetSpan> targets = find_step_in_targets_on_line(line_text);
        std::ostringstream json;
        json << '[';
        for (std::size_t index = 0; index < targets.size(); ++index) {
            if (index > 0) {
                json << ',';
            }
            json << R"({"id":)" << index << R"(,"label":")" << escape_json(targets[index].label) << R"("})";
        }
        json << ']';
        json_out = json.str();
        return true;
    }

    bool fetch_goto_targets(const std::string& path, int line, int column, std::int64_t source_reference,
                            std::string& json_out, std::string& error_out) override {
        (void)path;
        (void)column;
        (void)source_reference;
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        if (!stopped_) {
            error_out = "cannot resolve goto targets while running";
            return false;
        }
        if (line <= 0 || line == current_line_) {
            json_out = "[]";
            return true;
        }

        const std::string line_text = line_text_at(source_text_, line);
        if (!mock_line_is_jump_target(line_text)) {
            json_out = "[]";
            return true;
        }

        json_out = R"([{"id":)" + std::to_string(line) + R"(,"label":"line )" + std::to_string(line) +
                     R"("}])";
        return true;
    }

    bool evaluate(const std::string& expression, std::int64_t frame_id, const std::string& context,
                  std::string& result_out, std::string& error_out) override {
        (void)frame_id;
        (void)context;
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        result_out = "mock(" + expression + ")";
        return true;
    }

    bool set_variable(std::int64_t variables_reference, const std::string& name, const std::string& value,
                      std::string& result_out, std::string& error_out) override {
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        if (!stopped_) {
            error_out = "cannot set variable while program is running";
            return false;
        }
        variable_overrides_[mock_variable_key(variables_reference, name)] = value;
        result_out = R"({"name":")" + escape_json(name) + R"(","value":")" + escape_json(value) +
                     R"(","variablesReference":0})";
        return true;
    }

    bool set_breakpoints(const std::string& path, const std::string& lines_json,
                         std::string& error_out, std::string& results_out) override {
        (void)path;
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }

        results_out = "[]";
        breakpoints_.clear();
        std::size_t pos = 0;
        while (pos < lines_json.size()) {
            const std::size_t start = lines_json.find_first_of("0123456789", pos);
            if (start == std::string::npos) {
                break;
            }
            std::size_t end = start;
            while (end < lines_json.size() && lines_json[end] >= '0' && lines_json[end] <= '9') {
                ++end;
            }
            breakpoints_.insert(static_cast<int>(std::stoi(lines_json.substr(start, end - start))));
            pos = end;
        }
        return true;
    }

    bool data_breakpoint_info(std::int64_t variables_reference, std::int64_t frame_id, const std::string& name,
                              std::string& json_out, std::string& error_out) override {
        (void)variables_reference;
        (void)frame_id;
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        if (name.empty()) {
            error_out = "variable name required";
            return false;
        }
        json_out = R"({"dataId":"mock:")" + escape_json(name) +
                   R"(","description":")" + escape_json(name) + R"(","accessTypes":["read","write","readWrite"]})";
        return true;
    }

    bool set_data_breakpoints(const std::string& breakpoints_json, std::string& error_out,
                              std::string& results_out) override {
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        (void)breakpoints_json;
        results_out = "[]";
        return true;
    }

    bool set_function_breakpoints(const std::string& breakpoints_json, std::string& error_out,
                                  std::string& results_out) override {
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        (void)breakpoints_json;
        results_out = "[]";
        return true;
    }

    bool set_exception_breakpoints(const std::string& filters_json, std::string& error_out) override {
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }
        enabled_exception_filters_json_ = filters_json;
        return true;
    }

    std::optional<std::string> fetch_source(std::int64_t source_reference) override {
        if (!launched_ || source_reference <= 0) {
            return std::nullopt;
        }
        return "# mock adapter source\nimport json\n\ndef encode(obj):\n    return json.dumps(obj)\n";
    }

    std::optional<std::string> fetch_variables_json(std::int64_t variables_reference,
                                                    const std::string& /*scope_name*/) override {
        if (!launched_) {
            return std::nullopt;
        }
        if (variables_reference == 3) {
            return apply_variable_overrides(variables_reference, mock_variables_json("local_", 80, 1));
        }
        if (variables_reference == 4) {
            return apply_variable_overrides(variables_reference, mock_variables_json("global_", 40, 100));
        }
        return "[]";
    }

    std::optional<std::string> highlight_viewport(const std::string& language, const std::string& source,
                                                  int first_line, int line_count,
                                                  std::string& error_out) override {
        (void)language;
        (void)error_out;

        std::vector<std::string> lines;
        std::istringstream stream(source);
        std::string line;
        while (std::getline(stream, line)) {
            lines.push_back(line);
        }

        std::ostringstream json;
        json << '[';
        bool first_row = true;
        for (int row = 0; row < line_count; ++row) {
            const int line_number = first_line + row;
            if (line_number < 1 || line_number > static_cast<int>(lines.size())) {
                continue;
            }
            if (!first_row) {
                json << ',';
            }
            first_row = false;
            json << "{\"line_number\":" << line_number << ",\"spans\":[{\"text\":\""
                 << escape_json(lines[static_cast<std::size_t>(line_number - 1)])
                 << "\",\"kind\":\"default\"}]}";
        }
        json << ']';
        return json.str();
    }

  private:
    static std::string mock_variable_key(std::int64_t variables_reference, const std::string& name) {
        return std::to_string(variables_reference) + ':' + name;
    }

    std::string apply_variable_overrides(std::int64_t variables_reference, const std::string& json) const {
        std::string patched = json;
        for (const auto& [key, value] : variable_overrides_) {
            const std::string prefix = std::to_string(variables_reference) + ':';
            if (key.rfind(prefix, 0) != 0) {
                continue;
            }
            const std::string name = key.substr(prefix.size());
            const std::string needle = R"("name":")" + name + R"(")";
            const std::size_t name_pos = patched.find(needle);
            if (name_pos == std::string::npos) {
                continue;
            }
            const std::size_t value_key = patched.find(R"("value":")", name_pos);
            if (value_key == std::string::npos) {
                continue;
            }
            const std::size_t value_start = value_key + 9;
            const std::size_t value_end = patched.find('"', value_start);
            if (value_end == std::string::npos) {
                continue;
            }
            patched.replace(value_start, value_end - value_start, escape_json(value));
        }
        return patched;
    }

    std::string build_snapshot() const {
        std::ostringstream json;
        json << R"({"type":"snapshot","snapshot":{"capabilities":{"supports_step_back":true,"supports_step_in_targets":true,"supports_goto_targets":true,"supports_data_breakpoints":true,"supports_function_breakpoints":true,"exception_breakpoint_filters":[{"filter":"raised","label":"Raised Exceptions","default":false},{"filter":"uncaught","label":"Uncaught Exceptions","default":true},{"filter":"cxx-throw","label":"C++ Throw","default":false,"supports_condition":true}]},)";
        if (session_state_ == "exited") {
            json << R"("state":"Exited","threads":[],"stack_frames":[],"scopes":[],"variables":[]}})";
            return json.str();
        }
        if (session_state_ == "disconnected") {
            json << R"("state":"Disconnected","threads":[],"stack_frames":[],"scopes":[],"variables":[]}})";
            return json.str();
        }
        if (stopped_) {
            json << R"("state":{"Stopped":{"thread_id":1,"reason":"entry"}},"threads":[{"id":1,"name":"MainThread"},{"id":2,"name":"WorkerThread"}],"stack_frames":[{"id":2,"name":"<module>","line":)"
                 << current_line_ << R"(,"source":{"path":")" << escape_json(program_path_) << R"("}}],"thread_stacks":[{"thread_id":1,"stack_frames":[{"id":2,"name":"<module>","line":)"
                 << current_line_ << R"(,"source":{"path":")" << escape_json(program_path_) << R"("}}]},{"thread_id":2,"stack_frames":[{"id":10,"name":"worker_target","line":1,"source":{"path":")"
                 << escape_json(program_path_) << R"("}}]}],"scopes":[{"name":"Locals","variablesReference":3,"expensive":false},{"name":"Globals","variablesReference":4,"expensive":false}],"variables":[{"name":"name","value":"'debugger'"},{"name":"step","value":")"
                 << step_index_ << R"("}]}})";
        } else {
            json << R"("state":"Running","threads":[{"id":1,"name":"MainThread"}],"stack_frames":[],"scopes":[],"variables":[]}})";
        }
        return json.str();
    }

    std::string program_path_;
    std::vector<std::string> program_args_;
    std::string source_text_;
    bool launched_ = false;
    bool stopped_ = true;
    std::string session_state_ = "disconnected";
    int current_line_ = 1;
    int step_index_ = 0;
    std::unordered_set<int> breakpoints_;
    std::unordered_map<std::string, std::string> variable_overrides_;
    std::vector<std::string> console_pending_;
    std::string enabled_exception_filters_json_;
};

std::unique_ptr<SessionBackend> create_mock_session_backend() {
    return std::make_unique<MockSessionBackend>();
}

}  // namespace tui_debug_ui
