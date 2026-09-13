#include <tui_debug_ui/console_panel.hpp>

#include <tui_debug_ui/dap_ui_theme.hpp>
#include <tui_debug_ui/debug_ui_model.hpp>

#include <tuinator/core/event.hpp>

#include <algorithm>
#include <chrono>
#include <sstream>
#include <variant>

namespace tui_debug_ui {

namespace {

std::string default_console_sgr() {
    // Matches DapUiTheme::console_stdout (180, 220, 180).
    return "\033[38;2;180;220;180m\033[?25h";
}

void normalize_terminal_input(std::string& bytes) {
    std::string normalized;
    normalized.reserve(bytes.size());
    for (char ch : bytes) {
        normalized.push_back(ch == '\r' ? '\n' : ch);
    }
    bytes.swap(normalized);
}

/// Terminal display needs CR+LF; LF alone only advances the row, not the column.
std::string newlines_for_display(std::string text) {
    if (text.empty()) {
        return text;
    }

    std::string out;
    out.reserve(text.size() + 16);
    for (size_t i = 0; i < text.size(); ++i) {
        const char ch = text[i];
        if (ch == '\n' && (i == 0 || text[i - 1] != '\r')) {
            out.append("\r\n");
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

void refresh_cursor_from_buffer(const tuinator::AnsiTerminalBuffer& buffer, int& row, int& col, bool& visible) {
    buffer.cursor_position(row, col, visible);
}

bool is_backward_erase_bytes(const tuinator::KeyPress& key, const std::string& bytes) {
    if (key.key == tuinator::Key::Backspace || key.key == tuinator::Key::Delete) {
        return true;
    }
    return bytes == "\x7f" || bytes == "\x08";
}

std::string display_bytes_for_terminal_input(const tuinator::KeyPress& key, const std::string& bytes) {
    if (is_backward_erase_bytes(key, bytes)) {
        return "\x08 \x08";
    }
    return newlines_for_display(bytes);
}

}  // namespace

ConsolePanel::ConsolePanel(const DapUiTheme& theme)
    : panel_background_(theme.panel_background), terminal_init_sequence_(default_console_sgr()) {}

void ConsolePanel::ensure_buffer_size(const tuinator::Size& size) {
    const tuinator::Size target{std::max(1, size.width), std::max(1, size.height)};
    if (buffer_size_.width == target.width && buffer_size_.height == target.height) {
        return;
    }

    buffer_.resize(target);
    buffer_size_ = target;

    if (!terminal_initialized_) {
        buffer_.feed(terminal_init_sequence_);
        terminal_initialized_ = true;
    }
}

void ConsolePanel::prepare_program_output(std::string& text) {
    if (text.empty()) {
        return;
    }

    bool needs_prefix = pending_output_line_break_;
    if (!needs_prefix) {
        int row = 0;
        int col = 0;
        bool cursor_visible = false;
        buffer_.cursor_position(row, col, cursor_visible);
        if (col > 0 && text.front() != '\n' && text.front() != '\r') {
            needs_prefix = true;
        }
    }

    if (needs_prefix && text.rfind("\r\n", 0) != 0 && text.front() != '\n' && text.front() != '\r') {
        text.insert(0, "\r\n");
    }
    pending_output_line_break_ = false;
}

bool ConsolePanel::flush_pending_output() {
    if (pending_output_.empty()) {
        return false;
    }

    prepare_program_output(pending_output_);
    buffer_.feed(newlines_for_display(pending_output_));
    pending_output_.clear();
    bool cursor_visible = false;
    refresh_cursor_from_buffer(buffer_, last_cursor_row_, last_cursor_col_, cursor_visible);
    return true;
}

void ConsolePanel::feed_output(std::string text) {
    if (text.empty()) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (bounds_.width > 0 && bounds_.height > 0) {
            ensure_buffer_size({bounds_.width, bounds_.height});
            prepare_program_output(text);
            buffer_.feed(newlines_for_display(std::move(text)));
            flush_pending_output();
            bool cursor_visible = false;
            refresh_cursor_from_buffer(buffer_, last_cursor_row_, last_cursor_col_, cursor_visible);
        } else {
            pending_output_ += text;
        }
    }

    output_pending_.store(true, std::memory_order_release);
    mark_dirty();
}

void ConsolePanel::append_lines(std::vector<std::string> lines) {
    if (lines.empty()) {
        return;
    }
    std::string combined;
    for (const std::string& line : lines) {
        combined += line;
        if (!line.empty() && line.back() != '\n') {
            combined += '\n';
        }
    }
    feed_output(std::move(combined));
}

tuinator::Size ConsolePanel::preferred_size() const { return {80, 12}; }

void ConsolePanel::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }

    bool flushed = false;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        ensure_buffer_size({bounds.width, bounds.height});
        flushed = flush_pending_output();
    }

    if (flushed) {
        output_pending_.store(true, std::memory_order_release);
        mark_dirty();
    }
}

void ConsolePanel::reset_cursor_blink() {
    cursor_blink_visible_ = true;
    last_cursor_blink_ = std::chrono::steady_clock::now();
}

void ConsolePanel::paint_input_cursor(tuinator::PaintContext& ctx) const {
    if (!is_focused() || !input_active_) {
        return;
    }

    int row = 0;
    int col = 0;
    bool cursor_visible = false;
    refresh_cursor_from_buffer(buffer_, row, col, cursor_visible);
    last_cursor_row_ = row;
    last_cursor_col_ = col;

    row = std::clamp(row, 0, std::max(0, bounds_.height - 1));
    col = std::clamp(col, 0, std::max(0, bounds_.width - 1));

    tuinator::Style style;
    std::string glyph;
    if (!buffer_.cell_at(row, col, glyph, style)) {
        style = {};
    }

    tuinator::Style cursor_style = style;
    cursor_style.background = panel_background_.background;
    cursor_style.background_rgb = panel_background_.background_rgb;
    cursor_style.reverse = false;
    if (!cursor_style.foreground_rgb.has_value()) {
        cursor_style.foreground_rgb = tuinator::Rgb{220, 220, 225};
    }

    if (cursor_blink_visible_) {
        ctx.canvas.draw_text({col, row}, "_", cursor_style);
    } else if (!glyph.empty()) {
        ctx.canvas.draw_text({col, row}, glyph, cursor_style);
    }
}

void ConsolePanel::paint(tuinator::PaintContext& ctx) const {
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    ctx.canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', panel_background_);

    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.paint(ctx, {0, 0});
    paint_input_cursor(ctx);
}

void ConsolePanel::paint_text_cursor(tuinator::PaintContext& root_ctx) const {
    // Drawn in paint() via reverse-video block; keep the hardware cursor hidden.
    root_ctx.canvas.set_text_cursor(std::nullopt);
}

bool ConsolePanel::needs_periodic_idle() const {
    return output_pending_.load(std::memory_order_acquire) || (is_focused() && input_active_);
}

void ConsolePanel::on_idle() {
    if (output_pending_.exchange(false, std::memory_order_acq_rel)) {
        mark_dirty();
    }

    if (!is_focused() || !input_active_) {
        cursor_blink_visible_ = true;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_cursor_blink_ == std::chrono::steady_clock::time_point{}) {
        last_cursor_blink_ = now;
        return;
    }

    if (now - last_cursor_blink_ >= std::chrono::milliseconds(530)) {
        cursor_blink_visible_ = !cursor_blink_visible_;
        last_cursor_blink_ = now;
        mark_dirty();
    }
}

bool ConsolePanel::handle_line_buffered_key(const tuinator::KeyPress& key) {
    if (on_input_ == nullptr) {
        return false;
    }

    if (key.key == tuinator::Key::Backspace || key.key == tuinator::Key::Delete) {
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (bounds_.width <= 0 || bounds_.height <= 0 || pending_input_line_.empty()) {
                return false;
            }
            ensure_buffer_size({bounds_.width, bounds_.height});
            pending_input_line_.pop_back();
            buffer_.feed("\x08 \x08");
            bool cursor_visible = false;
            refresh_cursor_from_buffer(buffer_, last_cursor_row_, last_cursor_col_, cursor_visible);
        }
        mark_dirty();
        reset_cursor_blink();
        return true;
    }

    if (key.key == tuinator::Key::Enter) {
        std::string fifo_bytes;
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (bounds_.width <= 0 || bounds_.height <= 0) {
                return false;
            }
            ensure_buffer_size({bounds_.width, bounds_.height});
            pending_output_line_break_ = true;
            fifo_bytes = pending_input_line_;
            fifo_bytes.push_back('\n');
            pending_input_line_.clear();
            buffer_.feed(newlines_for_display("\r\n"));
            bool cursor_visible = false;
            refresh_cursor_from_buffer(buffer_, last_cursor_row_, last_cursor_col_, cursor_visible);
        }
        mark_dirty();
        reset_cursor_blink();
        on_input_(std::move(fifo_bytes));
        return true;
    }

    if (key.ctrl || key.alt || key.character == '\0') {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (bounds_.width <= 0 || bounds_.height <= 0) {
            return false;
        }
        ensure_buffer_size({bounds_.width, bounds_.height});
        pending_input_line_.push_back(key.character);
        std::string display;
        display.push_back(key.character);
        buffer_.feed(newlines_for_display(display));
        bool cursor_visible = false;
        refresh_cursor_from_buffer(buffer_, last_cursor_row_, last_cursor_col_, cursor_visible);
    }
    mark_dirty();
    reset_cursor_blink();
    return true;
}

bool ConsolePanel::handle_event(const tuinator::Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        if (mouse->action != tuinator::MouseAction::Click && mouse->action != tuinator::MouseAction::Release) {
            return false;
        }
        if (!bounds_.contains(mouse->position)) {
            return false;
        }
        set_focused(true);
        {
            std::lock_guard<std::mutex> lock(buffer_mutex_);
            if (bounds_.width > 0 && bounds_.height > 0) {
                ensure_buffer_size({bounds_.width, bounds_.height});
                buffer_.feed("\033[?25h");
            }
            bool cursor_visible = false;
            refresh_cursor_from_buffer(buffer_, last_cursor_row_, last_cursor_col_, cursor_visible);
        }
        if (on_activate_ != nullptr) {
            on_activate_();
        }
        mark_dirty();
        return true;
    }

    if (!is_focused() || !input_active_) {
        return false;
    }

    const auto* key = std::get_if<tuinator::KeyPress>(&event);
    if (key == nullptr) {
        return false;
    }

    if (on_input_ == nullptr) {
        return false;
    }

    if (line_buffered_input_) {
        return handle_line_buffered_key(*key);
    }

    std::string bytes;
    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        bytes = buffer_.keyboard_bytes(*key);
    }
    if (bytes.empty() && key->key == tuinator::Key::Enter) {
        bytes = "\n";
    }
    if (bytes.empty() && key->key == tuinator::Key::Backspace) {
        bytes = "\x7f";
    }
    if (bytes.empty() && key->key == tuinator::Key::Delete) {
        bytes = "\x1b[3~";
    }
    if (bytes.empty()) {
        return false;
    }
    std::string fifo_bytes = bytes;
    normalize_terminal_input(fifo_bytes);
    if (!fifo_bytes.empty() && fifo_bytes.back() == '\n') {
        pending_output_line_break_ = true;
    }

    {
        std::lock_guard<std::mutex> lock(buffer_mutex_);
        if (bounds_.width > 0 && bounds_.height > 0) {
            ensure_buffer_size({bounds_.width, bounds_.height});
            buffer_.feed(display_bytes_for_terminal_input(*key, fifo_bytes));
            bool cursor_visible = false;
            refresh_cursor_from_buffer(buffer_, last_cursor_row_, last_cursor_col_, cursor_visible);
        }
    }
    mark_dirty();
    reset_cursor_blink();

    on_input_(std::move(fifo_bytes));
    return true;
}

void ConsolePanel::collect_focusable(std::vector<tuinator::Widget*>& out) { out.push_back(this); }

tuinator::Widget* ConsolePanel::hit_test(tuinator::Point point) {
    if (bounds_.contains(point)) {
        return this;
    }
    return nullptr;
}

tuinator::Widget* ConsolePanel::hit_test_focusable(tuinator::Point point) {
    if (bounds_.contains(point)) {
        return this;
    }
    return nullptr;
}

std::vector<std::string> format_console_display_lines(const std::vector<ConsoleLine>& entries) {
    std::vector<std::string> lines;
    for (const ConsoleLine& entry : entries) {
        if (entry.text.empty()) {
            lines.push_back("[" + entry.category + "]");
            continue;
        }

        std::istringstream stream(entry.text);
        std::string line;
        bool first = true;
        while (std::getline(stream, line)) {
            if (first) {
                lines.push_back("[" + entry.category + "] " + line);
                first = false;
            } else {
                lines.push_back(line);
            }
        }
    }
    return lines;
}

}  // namespace tui_debug_ui
