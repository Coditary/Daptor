#pragma once

#include <tuinator/render/style.hpp>
#include <tuinator/terminal/ansi_terminal_buffer.hpp>
#include <tuinator/widgets/widget.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct ConsoleLine;
struct DapUiTheme;

/// Interactive debuggee console — ANSI terminal display with keyboard input forwarding.
class ConsolePanel : public tuinator::Widget {
  public:
    explicit ConsolePanel(const DapUiTheme& theme);

    void set_on_input(std::function<void(std::string)> callback) { on_input_ = std::move(callback); }
    void set_on_activate(std::function<void()> callback) { on_activate_ = std::move(callback); }

    void feed_output(std::string text);
    void append_lines(std::vector<std::string> lines);
    void set_input_active(bool active) { input_active_ = active; }
    void set_line_buffered_input(bool enabled) { line_buffered_input_ = enabled; }
    void paint_text_cursor(tuinator::PaintContext& root_ctx) const;

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    void on_idle() override;
    bool needs_periodic_idle() const override;
    bool is_focusable() const override { return true; }
    bool captures_keyboard() const override { return is_focused() && input_active_; }
    bool is_shell_terminal() const override { return is_focused() && input_active_; }
    void collect_focusable(std::vector<tuinator::Widget*>& out) override;
    tuinator::Widget* hit_test(tuinator::Point point) override;
    tuinator::Widget* hit_test_focusable(tuinator::Point point) override;

  private:
    void ensure_buffer_size(const tuinator::Size& size);
    bool flush_pending_output();
    void prepare_program_output(std::string& text);
    bool handle_line_buffered_key(const tuinator::KeyPress& key);
    void paint_input_cursor(tuinator::PaintContext& ctx) const;
    void reset_cursor_blink();

    tuinator::Style panel_background_;
    std::string terminal_init_sequence_;
    tuinator::AnsiTerminalBuffer buffer_;
    tuinator::Size buffer_size_{0, 0};
    std::string pending_output_;
    bool terminal_initialized_ = false;
    mutable std::mutex buffer_mutex_;
    std::atomic<bool> output_pending_{false};
    std::function<void(std::string)> on_input_;
    std::function<void()> on_activate_;
    mutable int last_cursor_row_ = 0;
    mutable int last_cursor_col_ = 0;
    mutable bool cursor_blink_visible_ = true;
    mutable std::chrono::steady_clock::time_point last_cursor_blink_{};
    bool pending_output_line_break_ = false;
    bool input_active_ = true;
    bool line_buffered_input_ = false;
    std::string pending_input_line_;
};

/// Format stored console entries for display in the panel.
std::vector<std::string> format_console_display_lines(const std::vector<ConsoleLine>& entries);

}  // namespace tui_debug_ui
