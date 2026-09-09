#include <tui_debug_ui/console_panel.hpp>

#include <tui_debug_ui/debug_ui_model.hpp>

#include <tuinator/core/event.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>
#include <sstream>
#include <variant>

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

} // namespace

ConsolePanel::ConsolePanel(tuinator::Style label_style, tuinator::Style panel_background)
    : label_style_(std::move(label_style)), panel_background_(std::move(panel_background)) {}

void ConsolePanel::set_text(std::string text) {
    lines_ = split_lines(std::move(text));
    scroll_to_bottom();
    mark_dirty();
}

void ConsolePanel::append_lines(std::vector<std::string> lines) {
    if (lines.empty()) {
        return;
    }
    lines_.insert(lines_.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
    scroll_to_bottom();
    mark_dirty();
}

int ConsolePanel::max_scroll_y() const {
    if (bounds_.height <= 0) {
        return std::max(0, static_cast<int>(lines_.size()) - 1);
    }
    return std::max(0, static_cast<int>(lines_.size()) - bounds_.height);
}

void ConsolePanel::clamp_scroll() {
    scroll_y_ = std::clamp(scroll_y_, 0, max_scroll_y());
}

void ConsolePanel::scroll_to_bottom() {
    scroll_y_ = max_scroll_y();
    clamp_scroll();
}

tuinator::Size ConsolePanel::preferred_size() const {
    int max_width = 0;
    for (const std::string& line : lines_) {
        max_width = std::max(max_width, tuinator::text_display_width(line));
    }
    return {max_width, std::max(1, static_cast<int>(lines_.size()))};
}

void ConsolePanel::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    clamp_scroll();
}

void ConsolePanel::paint(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', panel_background_);

    const int visible_rows = bounds_.height;
    for (int row = 0; row < visible_rows; ++row) {
        const int index = scroll_y_ + row;
        if (index < 0 || index >= static_cast<int>(lines_.size())) {
            continue;
        }

        const std::string& line = lines_[static_cast<std::size_t>(index)];
        if (line.empty()) {
            continue;
        }

        const int max_columns = std::max(0, bounds_.width);
        if (max_columns <= 0) {
            break;
        }

        const std::size_t bytes = tuinator::text_byte_length_for_width(line, max_columns);
        if (bytes == 0) {
            continue;
        }

        canvas.draw_text({0, row}, line.substr(0, bytes), label_style_);
    }
}

bool ConsolePanel::handle_event(const tuinator::Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        if (!is_focused() || !bounds_.contains(mouse->position)) {
            return false;
        }

        switch (mouse->action) {
        case tuinator::MouseAction::WheelUp:
            scroll_y_ = std::max(0, scroll_y_ - 1);
            clamp_scroll();
            mark_dirty();
            return true;
        case tuinator::MouseAction::WheelDown:
            scroll_y_ = std::min(max_scroll_y(), scroll_y_ + 1);
            clamp_scroll();
            mark_dirty();
            return true;
        default:
            break;
        }
        return false;
    }

    const auto* key = std::get_if<tuinator::KeyPress>(&event);
    if (key == nullptr || !is_focused()) {
        return false;
    }

    if (key->character == 'j' || key->key == tuinator::Key::Down) {
        scroll_y_ = std::min(max_scroll_y(), scroll_y_ + 1);
        clamp_scroll();
        mark_dirty();
        return true;
    }
    if (key->character == 'k' || key->key == tuinator::Key::Up) {
        scroll_y_ = std::max(0, scroll_y_ - 1);
        clamp_scroll();
        mark_dirty();
        return true;
    }
    if (key->key == tuinator::Key::PageDown) {
        scroll_y_ = std::min(max_scroll_y(), scroll_y_ + std::max(1, bounds_.height));
        clamp_scroll();
        mark_dirty();
        return true;
    }
    if (key->key == tuinator::Key::PageUp) {
        scroll_y_ = std::max(0, scroll_y_ - std::max(1, bounds_.height));
        clamp_scroll();
        mark_dirty();
        return true;
    }
    if (key->character == 'g' && !key->ctrl) {
        scroll_y_ = 0;
        clamp_scroll();
        mark_dirty();
        return true;
    }
    if (key->character == 'G' && !key->ctrl) {
        scroll_to_bottom();
        mark_dirty();
        return true;
    }

    return false;
}

void ConsolePanel::collect_focusable(std::vector<tuinator::Widget*>& out) {
    out.push_back(this);
}

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

} // namespace tui_debug_ui
