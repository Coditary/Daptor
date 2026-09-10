#include <tui_debug_ui/console_panel.hpp>

#include <tui_debug_ui/dap_ui_theme.hpp>
#include <tui_debug_ui/debug_ui_model.hpp>

#include <tuinator/render/text.hpp>

#include <algorithm>
#include <sstream>
#include <string_view>

namespace tui_debug_ui {

namespace {

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }
    if (lines.empty() && !text.empty()) {
        lines.push_back(text);
    }
    return lines;
}

tuinator::Style console_line_style(const std::string& line, tuinator::Style fallback,
                                   tuinator::Style stderr_style, tuinator::Style stdout_style,
                                   tuinator::Style event_style) {
    if (line.size() < 3 || line[0] != '[') {
        return fallback;
    }

    const std::size_t end = line.find(']');
    if (end == std::string::npos || end < 2) {
        return fallback;
    }

    const std::string category = line.substr(1, end - 1);
    if (category == "stderr" || category == "error") {
        return stderr_style;
    }
    if (category == "stdout" || category == "output") {
        return stdout_style;
    }
    if (category == "event" || category == "dap") {
        return event_style;
    }
    return fallback;
}

}  // namespace

ConsolePanel::ConsolePanel(const DapUiTheme& theme)
    : label_style_(theme.label),
      panel_background_(theme.panel_background),
      console_stderr_(theme.console_stderr),
      console_stdout_(theme.console_stdout),
      console_event_(theme.console_event) {}

void ConsolePanel::set_text(std::string text) {
    lines_ = split_lines(std::move(text));
    mark_dirty();
}

void ConsolePanel::append_lines(std::vector<std::string> lines) {
    if (lines.empty()) {
        return;
    }
    lines_.insert(lines_.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
    mark_dirty();
}

tuinator::Size ConsolePanel::preferred_size() const {
    int max_width = 0;
    for (const std::string& line : lines_) {
        max_width = std::max(max_width, tuinator::text_display_width(line));
    }
    return {max_width, std::max(1, static_cast<int>(lines_.size()))};
}

void ConsolePanel::layout(tuinator::Rect bounds) { bounds_ = bounds; }

void ConsolePanel::paint(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', panel_background_);

    const int max_columns = std::max(0, bounds_.width);
    for (int index = 0; index < static_cast<int>(lines_.size()); ++index) {
        const std::string& line = lines_[static_cast<std::size_t>(index)];
        if (line.empty() || max_columns <= 0) {
            continue;
        }

        const std::size_t bytes = tuinator::text_byte_length_for_width(line, max_columns);
        if (bytes == 0) {
            continue;
        }

        const tuinator::Style style =
            console_line_style(line, label_style_, console_stderr_, console_stdout_, console_event_);
        canvas.draw_text({0, index}, line.substr(0, bytes), style);
    }
}

bool ConsolePanel::handle_event(const tuinator::Event& /*event*/) { return false; }

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
