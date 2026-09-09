#include "tui_debug_ui/session_backend.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace tui_debug_ui {
namespace {

constexpr int kMockLines[] = {1, 3, 4, 8, 9, 12, 13, 17, 22};

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

}  // namespace

class MockSessionBackend final : public SessionBackend {
  public:
    void launch(const std::string& program_path) override {
        program_path_ = program_path;
        source_text_ = read_file(program_path);
        current_line_ = 1;
        step_index_ = 0;
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

        if (session_state_ == "exited" || session_state_ == "disconnected") {
            if (op == "restart") {
                launch(program_path_);
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

        if (op == "continue" || op == "play_pause") {
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
        if (op == "step_over" || op == "next") {
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
        if (op == "step_into" || op == "step_in") {
            return send_command("step_over", error_out);
        }
        if (op == "step_out") {
            return send_command("step_over", error_out);
        }
        if (op == "pause") {
            stopped_ = true;
            session_state_ = "stopped";
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
        if (op == "restart") {
            current_line_ = 1;
            step_index_ = 0;
            stopped_ = true;
            session_state_ = "stopped";
            return true;
        }

        error_out = "unknown mock command: " + op;
        return false;
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

    bool set_breakpoints(const std::string& path, const std::string& lines_json,
                         std::string& error_out) override {
        (void)path;
        if (!launched_) {
            error_out = "mock session not launched";
            return false;
        }

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

    std::optional<std::string> fetch_variables_json(std::int64_t variables_reference) override {
        if (!launched_) {
            return std::nullopt;
        }
        if (variables_reference == 3) {
            return R"([{"name":"name","value":"'debugger'","variablesReference":0},{"name":"step","value":")"
                   + std::to_string(step_index_) + R"(","variablesReference":0}])";
        }
        if (variables_reference == 4) {
            return R"([{"name":"__name__","value":"'__main__'","variablesReference":0}])";
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
    std::string build_snapshot() const {
        std::ostringstream json;
        json << R"({"type":"snapshot","snapshot":{)";
        if (session_state_ == "exited") {
            json << R"("state":"Exited","threads":[],"stack_frames":[],"scopes":[],"variables":[]}})";
            return json.str();
        }
        if (session_state_ == "disconnected") {
            json << R"("state":"Disconnected","threads":[],"stack_frames":[],"scopes":[],"variables":[]}})";
            return json.str();
        }
        if (stopped_) {
            json << R"("state":{"Stopped":{"thread_id":1,"reason":"entry"}},"threads":[{"id":1,"name":"MainThread"}],"stack_frames":[{"id":2,"name":"<module>","line":)"
                 << current_line_ << R"(,"source":{"path":")" << escape_json(program_path_) << R"("}}],"scopes":[{"name":"Locals","variablesReference":3,"expensive":false},{"name":"Globals","variablesReference":4,"expensive":false}],"variables":[{"name":"name","value":"'debugger'"},{"name":"step","value":")"
                 << step_index_ << R"("}]}})";
        } else {
            json << R"("state":"Running","threads":[{"id":1,"name":"MainThread"}],"stack_frames":[],"scopes":[],"variables":[]}})";
        }
        return json.str();
    }

    std::string program_path_;
    std::string source_text_;
    bool launched_ = false;
    bool stopped_ = true;
    std::string session_state_ = "disconnected";
    int current_line_ = 1;
    int step_index_ = 0;
    std::unordered_set<int> breakpoints_;
    std::vector<std::string> console_pending_;
};

std::unique_ptr<SessionBackend> create_mock_session_backend() {
    return std::make_unique<MockSessionBackend>();
}

}  // namespace tui_debug_ui
