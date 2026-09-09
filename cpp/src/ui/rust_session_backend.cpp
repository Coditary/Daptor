#include "tui_debug_ui/session_backend.hpp"

extern "C" {
#include "tui_debug.h"
}

#include <cstring>
#include <vector>

namespace tui_debug_ui {

class RustSessionBackend final : public SessionBackend {
  public:
    ~RustSessionBackend() override { shutdown(); }

    void launch(const std::string& program_path) override {
        shutdown();
        session_ = tui_debug_session_launch(program_path.c_str());
    }

    void shutdown() override {
        if (session_ != nullptr) {
            tui_debug_session_free(session_);
            session_ = nullptr;
        }
    }

    bool is_active() const override { return session_ != nullptr; }

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

    std::optional<std::string> fetch_variables_json(std::int64_t variables_reference) override {
        return read_json_buffer([this, variables_reference](char* out, std::size_t cap) {
            return tui_debug_fetch_variables(session_, variables_reference, out, cap);
        });
    }

    bool send_command(const std::string& op, std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        char cmd[128];
        std::snprintf(cmd, sizeof(cmd), "{\"op\":\"%s\"}", op.c_str());
        if (tui_debug_command(session_, cmd) == 0) {
            return true;
        }

        error_out = last_error();
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

        error_out = last_error();
        return false;
    }

    bool set_breakpoints(const std::string& path, const std::string& lines_json,
                         std::string& error_out) override {
        if (session_ == nullptr) {
            error_out = "no session";
            return false;
        }

        if (tui_debug_set_breakpoints(session_, path.c_str(), lines_json.c_str()) == 0) {
            return true;
        }

        error_out = last_error();
        return false;
    }

    std::optional<std::string> highlight_viewport(const std::string& language, const std::string& source,
                                                  int first_line, int line_count,
                                                  std::string& error_out) override {
        std::vector<char> buffer(65536);
        const int status = tui_debug_highlight_viewport(language.c_str(), source.c_str(), first_line, line_count,
                                                        buffer.data(), buffer.size());
        if (status != 0) {
            error_out = last_error();
            return std::nullopt;
        }
        return std::string(buffer.data());
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

    static std::string last_error() {
        const char* err = tui_debug_last_error();
        if (err != nullptr && err[0] != '\0') {
            return err;
        }
        return "unknown error";
    }

    void* session_ = nullptr;
};

std::unique_ptr<SessionBackend> create_rust_session_backend() {
    return std::make_unique<RustSessionBackend>();
}

}  // namespace tui_debug_ui
