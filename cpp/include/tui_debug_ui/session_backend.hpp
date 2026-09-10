#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace tui_debug_ui {

enum class SessionMode {
    Rust,
    Mock,
};

/// Abstraction over the Rust C API (or a mock for frontend-only development).
class SessionBackend {
  public:
    virtual ~SessionBackend() = default;

    virtual void launch(const std::string& program_path) = 0;
    virtual void shutdown() = 0;
    virtual bool is_active() const = 0;
    virtual std::string last_error() const { return {}; }

    virtual std::optional<std::string> sync_snapshot_json() = 0;
    /// Returns 0 when an event was written, 1 when idle, -1 on error.
    virtual int poll_json(std::string& json_out) = 0;
    virtual std::optional<std::string> drain_console_json() = 0;

    virtual bool send_command(const std::string& op, std::string& error_out) = 0;
    virtual bool evaluate(const std::string& expression, std::int64_t frame_id, const std::string& context,
                          std::string& result_out, std::string& error_out) = 0;
    virtual bool set_variable(std::int64_t variables_reference, const std::string& name, const std::string& value,
                              std::string& result_out, std::string& error_out) = 0;
    virtual bool set_breakpoints(const std::string& path, const std::string& lines_json,
                                 std::string& error_out) = 0;
    virtual std::optional<std::string> fetch_variables_json(std::int64_t variables_reference) = 0;
    virtual std::optional<std::string> fetch_source(std::int64_t source_reference) = 0;
    virtual std::optional<std::string> highlight_viewport(const std::string& language, const std::string& source,
                                                          int first_line, int line_count,
                                                          std::string& error_out) = 0;
};

std::unique_ptr<SessionBackend> create_session_backend(SessionMode mode);

}  // namespace tui_debug_ui
