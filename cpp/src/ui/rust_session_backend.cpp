#include "tui_debug_ui/session_backend.hpp"

extern "C" {
#include "tui_debug.h"
}

#include <cstring>
#include <sstream>
#include <vector>

namespace tui_debug_ui {

namespace {

std::string serialize_program_args_json(const std::vector<std::string>& program_args) {
    if (program_args.empty()) {
        return {};
    }
    std::ostringstream json;
    json << '[';
    bool first = true;
    for (const std::string& arg : program_args) {
        if (!first) {
            json << ',';
        }
        first = false;
        json << '"';
        for (char ch : arg) {
            if (ch == '"' || ch == '\\') {
                json << '\\';
            }
            json << ch;
        }
        json << '"';
    }
    json << ']';
    return json.str();
}

}  // namespace

class RustSessionBackend final : public SessionBackend {
  public:
    explicit RustSessionBackend(DebugAdapter adapter) : adapter_(adapter) {}

    ~RustSessionBackend() override { shutdown(); }

    void launch(const std::string& program_path, const std::vector<std::string>& program_args) override {
        shutdown();
        const std::string args_json = serialize_program_args_json(program_args);
        const char* args_ptr = args_json.empty() ? nullptr : args_json.c_str();
        if (adapter_ == DebugAdapter::Lldb) {
            session_ = tui_debug_session_launch_lldb(program_path.c_str(), args_ptr);
        } else if (adapter_ == DebugAdapter::Rr) {
            session_ = tui_debug_session_launch_rr(program_path.c_str(), args_ptr);
        } else {
            session_ = tui_debug_session_launch(program_path.c_str(), args_ptr);
        }
    }

    void shutdown() override {
        if (session_ != nullptr) {
            tui_debug_session_free(session_);
            session_ = nullptr;
        }
    }

    bool is_active() const override { return session_ != nullptr; }

    std::string last_error() const override { return adapter_last_error(false); }

    std::optional<std::string> sync_snapshot_json() override {
        return read_json_buffer([this](char* out, std::size_t cap) {
            return tui_debug_sync_snapshot(session_, out, cap);
        });
    }

    int poll_json(std::string& json_out) override {
        if (session_ == nullptr) {
            return -1;
        }

        char buffer[65536];
        const int rc = tui_debug_poll(session_, buffer, sizeof(buffer));
        if (rc == 0) {
            json_out = buffer;
        }
        return rc;
    }

    std::optional<std::string> drain_console_json() override {
        return read_json_buffer([this](char* out, std::size_t cap) {
            return tui_debug_drain_console(session_, out, cap);
        });
    }

    bool terminal_write(const std::string& bytes, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }
        if (tui_debug_terminal_write(session_, reinterpret_cast<const unsigned char*>(bytes.data()),
                                     bytes.size()) == 0) {
            return true;
        }
        error_out = adapter_last_error();
        return false;
    }

    std::optional<std::string> fetch_variables_json(std::int64_t variables_reference,
                                                    const std::string& scope_name) override {
        return read_json_buffer([this, variables_reference, &scope_name](char* out, std::size_t cap) {
            const char* scope = scope_name.empty() ? nullptr : scope_name.c_str();
            return tui_debug_fetch_variables(session_, variables_reference, scope, out, cap);
        });
    }

    std::optional<std::string> fetch_source(std::int64_t source_reference) override {
        return read_json_buffer([this, source_reference](char* out, std::size_t cap) {
            return tui_debug_fetch_source(session_, source_reference, out, cap);
        });
    }

    std::optional<std::string> read_memory(const std::string& memory_reference, std::int64_t offset,
                                           std::int64_t count) override {
        return read_json_buffer([this, &memory_reference, offset, count](char* out, std::size_t cap) {
            return tui_debug_read_memory(session_, memory_reference.c_str(), offset, count, out, cap);
        });
    }

    std::optional<std::string> write_memory(const std::string& memory_reference, std::int64_t offset,
                                            const std::string& hex_data) override {
        return read_json_buffer([this, &memory_reference, offset, &hex_data](char* out, std::size_t cap) {
            return tui_debug_write_memory(session_, memory_reference.c_str(), offset, hex_data.c_str(), out, cap);
        });
    }

    std::optional<std::string> disassemble(const std::string& memory_reference, std::int64_t instruction_offset,
                                         std::int64_t offset, std::int64_t instruction_count) override {
        return read_json_buffer([this, &memory_reference, instruction_offset, offset, instruction_count](
                                    char* out, std::size_t cap) {
            return tui_debug_disassemble(session_, memory_reference.c_str(), instruction_offset, offset,
                                         instruction_count, out, cap);
        });
    }

    bool send_command(const std::string& op, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        const std::string cmd = !op.empty() && op.front() == '{' ? op : std::string("{\"op\":\"") + op + "\"}";
        if (tui_debug_command(session_, cmd.c_str()) == 0) {
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool fetch_step_in_targets(std::int64_t frame_id, std::string& json_out,
                               std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char buffer[16384];
        if (tui_debug_fetch_step_in_targets(session_, frame_id, buffer, sizeof(buffer)) == 0) {
            json_out = buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool fetch_goto_targets(const std::string& path, int line, int column, std::int64_t source_reference,
                            std::string& json_out, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char buffer[16384];
        if (tui_debug_fetch_goto_targets(session_, path.c_str(), line, column, source_reference, buffer,
                                         sizeof(buffer)) == 0) {
            json_out = buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool set_variable(std::int64_t variables_reference, const std::string& name, const std::string& value,
                      std::string& result_out, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char buffer[4096];
        if (tui_debug_set_variable(session_, variables_reference, name.c_str(), value.c_str(), buffer,
                                   sizeof(buffer)) == 0) {
            result_out = buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool evaluate(const std::string& expression, std::int64_t frame_id, const std::string& context,
                std::string& result_out, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char buffer[4096];
        if (tui_debug_evaluate(session_, expression.c_str(), frame_id, context.c_str(), buffer,
                               sizeof(buffer)) == 0) {
            result_out = buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool fetch_completions(const std::string& text, std::int64_t column, std::int64_t frame_id,
                           std::string& json_out, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char buffer[16384];
        if (tui_debug_completions(session_, text.c_str(), column, frame_id, buffer, sizeof(buffer)) == 0) {
            json_out = buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool set_breakpoints(const std::string& path, const std::string& lines_json,
                         std::string& error_out, std::string& results_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char results_buffer[4096];
        if (tui_debug_set_breakpoints(session_, path.c_str(), lines_json.c_str(), results_buffer,
                                      sizeof(results_buffer)) == 0) {
            results_out = results_buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool data_breakpoint_info(std::int64_t variables_reference, std::int64_t frame_id, const std::string& name,
                              std::string& json_out, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char buffer[4096];
        const char* name_ptr = name.empty() ? nullptr : name.c_str();
        if (tui_debug_data_breakpoint_info(session_, variables_reference, frame_id, name_ptr, buffer,
                                           sizeof(buffer)) == 0) {
            json_out = buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool set_data_breakpoints(const std::string& breakpoints_json, std::string& error_out,
                              std::string& results_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char results_buffer[4096];
        if (tui_debug_set_data_breakpoints(session_, breakpoints_json.c_str(), results_buffer,
                                           sizeof(results_buffer)) == 0) {
            results_out = results_buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool set_function_breakpoints(const std::string& breakpoints_json, std::string& error_out,
                                  std::string& results_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char results_buffer[4096];
        if (tui_debug_set_function_breakpoints(session_, breakpoints_json.c_str(), results_buffer,
                                               sizeof(results_buffer)) == 0) {
            results_out = results_buffer;
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    bool set_exception_breakpoints(const std::string& filters_json, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        if (tui_debug_set_exception_breakpoints(session_, filters_json.c_str()) == 0) {
            return true;
        }

        error_out = adapter_last_error();
        return false;
    }

    std::optional<std::string> highlight_viewport(const std::string& language, const std::string& source,
                                                  int first_line, int line_count,
                                                  std::string& error_out) override {
        std::size_t capacity = 256 * 1024;
        for (int attempt = 0; attempt < 4; ++attempt) {
            std::vector<char> buffer(capacity, '\0');
            const int status = tui_debug_highlight_viewport(language.c_str(), source.c_str(), first_line, line_count,
                                                            buffer.data(), buffer.size());
            if (status == 0) {
                return std::string(buffer.data());
            }

            error_out = adapter_last_error();
            if (error_out.find("buffer too small") == std::string::npos) {
                return std::nullopt;
            }
            capacity *= 2;
        }
        return std::nullopt;
    }

  private:
    template <typename ReadFn>
    std::optional<std::string> read_json_buffer(ReadFn read_fn) {
        if (session_ == nullptr) {
            return std::nullopt;
        }

        char buffer[65536];
        if (read_fn(buffer, sizeof(buffer)) != 0) {
            return std::nullopt;
        }
        return std::string(buffer);
    }

    static std::string adapter_last_error(bool unknown_fallback = true) {
        const char* err = tui_debug_last_error();
        if (err != nullptr && err[0] != '\0') {
            return err;
        }
        return unknown_fallback ? "unknown error" : std::string{};
    }

    void* session_ = nullptr;
    DebugAdapter adapter_ = DebugAdapter::Debugpy;
};

std::unique_ptr<SessionBackend> create_rust_session_backend(DebugAdapter adapter) {
    return std::make_unique<RustSessionBackend>(adapter);
}

}  // namespace tui_debug_ui
