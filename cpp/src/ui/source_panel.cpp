#include <tui_debug_ui/source_panel.hpp>
#include <tui_debug_ui/highlight_bridge.hpp>
#include <tui_debug_ui/ui_icons.hpp>

#include <tuinator/core/event.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>

#include <algorithm>
#include <cstdio>
#include <string>
#include <variant>

using tuinator::Canvas;
using tuinator::Event;
using tuinator::Key;
using tuinator::KeyPress;
using tuinator::PaintContext;
using tuinator::Rect;
using tuinator::Size;
using tuinator::Style;

namespace tui_debug_ui {

namespace {

constexpr const char* kEmptyMessage = "No source loaded \u2014 press p to open a file";

} // namespace

SourcePanel::SourcePanel(SyntaxTheme theme) : theme_(std::move(theme)) {
    theme_.keyword.bold = true;
    theme_.breakpoint_marker.bold = true;
    theme_.breakpoint_conditional_marker.bold = true;
    theme_.execution_row.bold = true;
    theme_.execution_marker.bold = true;
}

void SourcePanel::set_scroll_parent(tuinator::ScrollView* scroll_parent) { scroll_parent_ = scroll_parent; }

int SourcePanel::scroll_offset() const {
    return scroll_parent_ != nullptr ? scroll_parent_->scroll_y() : scroll_offset_;
}

int SourcePanel::viewport_height() const {
    if (scroll_parent_ != nullptr) {
        const int scroll_bounds = scroll_parent_->bounds().height;
        if (scroll_bounds > 0) {
            return scroll_bounds;
        }

        const int content_height = static_cast<int>(lines_.size());
        const int max_scroll = scroll_parent_->max_scroll_y();
        if (content_height > 0 && max_scroll < content_height) {
            return content_height - max_scroll;
        }

        return 24;
    }

    return std::max(1, bounds_.height);
}

void SourcePanel::set_lines(std::vector<HighlightedLine> lines) {
    lines_ = std::move(lines);
    if (scroll_parent_ != nullptr) {
        scroll_parent_->refresh_content();
    } else {
        clamp_scroll();
    }
    mark_dirty();
}

void SourcePanel::reset_plain_spans_for_line_range(int first_line, int line_count,
                                                  const std::string& source_text) {
    if (first_line < 1 || line_count <= 0 || source_text.empty()) {
        return;
    }

    const auto plain_lines = build_plain_viewport_lines(source_text, first_line, line_count);
    for (const HighlightedLine& plain_line : plain_lines) {
        for (HighlightedLine& existing : lines_) {
            if (existing.line_number == plain_line.line_number) {
                existing.spans = plain_line.spans;
                break;
            }
        }
    }
    mark_dirty();
}

void SourcePanel::merge_highlighted_lines(const std::vector<HighlightedLine>& highlighted) {
    if (highlighted.empty()) {
        return;
    }

    for (const HighlightedLine& incoming : highlighted) {
        for (HighlightedLine& existing : lines_) {
            if (existing.line_number == incoming.line_number) {
                existing.spans = incoming.spans;
                break;
            }
        }
    }

    mark_dirty();
}

void SourcePanel::set_scroll_offset(int offset) {
    offset = std::max(0, offset);
    if (scroll_parent_ != nullptr) {
        if (scroll_parent_->scroll_y() == offset) {
            return;
        }
        scroll_parent_->scroll_to(0, offset);
        mark_dirty();
        return;
    }

    if (scroll_offset_ == offset) {
        return;
    }
    scroll_offset_ = offset;
    clamp_scroll();
    mark_dirty();
}

void SourcePanel::set_execution_line(int line) {
    line = std::max(0, line);
    if (execution_line_ == line) {
        return;
    }
    execution_line_ = line;
    mark_dirty();
}

void SourcePanel::set_cursor_line(int line) {
    line = std::max(1, line);
    if (cursor_line_ == line) {
        return;
    }
    cursor_line_ = line;
    mark_dirty();
}

void SourcePanel::set_breakpoints(std::unordered_map<int, bool> breakpoints) {
    breakpoints_ = std::move(breakpoints);
    mark_dirty();
}

void SourcePanel::set_function_breakpoint_lines(std::unordered_set<int> lines) {
    function_breakpoint_lines_ = std::move(lines);
    mark_dirty();
}

void SourcePanel::set_file_line_count(int count) {
    file_line_count_ = std::max(1, count);
    cursor_line_ = std::clamp(cursor_line_, 1, file_line_count_);
    mark_dirty();
}

void SourcePanel::set_on_toggle_breakpoint(BreakpointToggleCallback callback) {
    on_toggle_breakpoint_ = std::move(callback);
}

void SourcePanel::set_on_breakpoint_context(BreakpointContextCallback callback) {
    on_breakpoint_context_ = std::move(callback);
}

int SourcePanel::code_column_from_local_x(int local_x) const {
    const int start = code_start_x();
    return std::max(0, local_x - start);
}

void SourcePanel::set_on_request_viewport(ViewportRequestCallback callback) {
    on_request_viewport_ = std::move(callback);
}

void SourcePanel::set_on_step_in_target_click(StepInTargetClickCallback callback) {
    on_step_in_target_click_ = std::move(callback);
}

void SourcePanel::move_cursor_by(int delta) {
    if (file_line_count_ <= 0) {
        return;
    }
    set_cursor_line(std::clamp(cursor_line_ + delta, 1, file_line_count_));
    ensure_cursor_visible();
}

int SourcePanel::line_number_at_row(int row) const {
    if (row >= 0 && row < static_cast<int>(lines_.size())) {
        return lines_[static_cast<std::size_t>(row)].line_number;
    }

    if (scroll_parent_ != nullptr) {
        return row + 1;
    }

    const int index = scroll_offset_ + row;
    if (index >= 0 && index < static_cast<int>(lines_.size())) {
        return lines_[static_cast<std::size_t>(index)].line_number;
    }
    return scroll_offset_ + row + 1;
}

int SourcePanel::row_for_line_number(int line_number) const {
    for (int row = 0; row < static_cast<int>(lines_.size()); ++row) {
        if (lines_[static_cast<std::size_t>(row)].line_number == line_number) {
            return row;
        }
    }

    if (scroll_parent_ != nullptr) {
        return std::max(0, line_number - 1);
    }

    return std::max(0, line_number - scroll_offset_ - 1);
}

tuinator::Point SourcePanel::context_menu_anchor(int line_number, int code_column) const {
    const int row = row_for_line_number(line_number);
    // Place the menu on the line below the click so it does not cover the target line.
    const int menu_row = row + 1;

    const int local_x = code_start_x() + std::max(0, code_column);
    if (scroll_parent_ == nullptr) {
        return {bounds_.x + local_x, bounds_.y + menu_row};
    }

    const tuinator::Rect scroll_bounds = scroll_parent_->bounds();
    return {scroll_bounds.x + local_x - scroll_parent_->scroll_x(),
            scroll_bounds.y + menu_row - scroll_parent_->scroll_y()};
}

int SourcePanel::code_start_x() const {
    return 2 + 2 + gutter_width() + 3;
}

bool SourcePanel::is_gutter_click(int local_x) const {
    return local_x >= 0 && local_x < code_start_x();
}

void SourcePanel::ensure_cursor_visible() {
    if (lines_.empty()) {
        return;
    }

    int line_index = -1;
    for (int index = 0; index < static_cast<int>(lines_.size()); ++index) {
        if (lines_[static_cast<std::size_t>(index)].line_number == cursor_line_) {
            line_index = index;
            break;
        }
    }

    if (line_index < 0) {
        if (on_request_viewport_) {
            on_request_viewport_(cursor_line_);
        }
        return;
    }

    const int viewport = viewport_height();
    if (scroll_parent_ != nullptr) {
        const int scroll_y = scroll_parent_->scroll_y();
        if (line_index < scroll_y) {
            set_scroll_offset(line_index);
        } else if (line_index >= scroll_y + viewport) {
            set_scroll_offset(line_index - viewport + 1);
        }
        return;
    }

    if (bounds_.height <= 0) {
        return;
    }

    if (line_index < scroll_offset_) {
        set_scroll_offset(line_index);
    } else if (line_index >= scroll_offset_ + bounds_.height) {
        set_scroll_offset(line_index - bounds_.height + 1);
    }
}

void SourcePanel::set_syntax_theme(SyntaxTheme theme) {
    theme_ = std::move(theme);
    theme_.keyword.bold = true;
    theme_.breakpoint_marker.bold = true;
    theme_.breakpoint_conditional_marker.bold = true;
    theme_.execution_row.bold = true;
    theme_.execution_marker.bold = true;
    theme_.step_in_candidate.bold = true;
    theme_.step_in_active.bold = true;
    mark_dirty();
}

void SourcePanel::set_step_in_selection(std::optional<StepInSelectionState> selection) {
    step_in_selection_ = std::move(selection);
    mark_dirty();
}

Style SourcePanel::style_for_code_column(int line_number, int code_column, HighlightKind base_kind) const {
    if (step_in_selection_ && step_in_selection_->line == line_number) {
        for (std::size_t index = 0; index < step_in_selection_->targets.size(); ++index) {
            const StepInTargetSpan& target = step_in_selection_->targets[index];
            if (code_column >= target.start_column && code_column < target.end_column) {
                const HighlightKind kind = static_cast<int>(index) == step_in_selection_->active_index
                                               ? HighlightKind::StepInActive
                                               : HighlightKind::StepInCandidate;
                return theme_.style_for(kind);
            }
        }
    }
    return theme_.style_for(base_kind);
}

bool SourcePanel::is_step_in_column(int line_number, int code_column) const {
    return step_in_target_index_at(line_number, code_column).has_value();
}

std::optional<int> SourcePanel::step_in_target_index_at(int line_number, int code_column) const {
    if (!step_in_selection_ || step_in_selection_->line != line_number) {
        return std::nullopt;
    }

    for (std::size_t index = 0; index < step_in_selection_->targets.size(); ++index) {
        const StepInTargetSpan& target = step_in_selection_->targets[index];
        if (code_column >= target.start_column && code_column < target.end_column) {
            return static_cast<int>(index);
        }
    }
    return std::nullopt;
}

Size SourcePanel::preferred_size() const {
    int width = 40;
    for (const HighlightedLine& line : lines_) {
        int line_width = gutter_width() + 2;
        for (const HighlightSpan& span : line.spans) {
            line_width += tuinator::text_display_width(span.text);
        }
        width = std::max(width, line_width);
    }
    const int height = std::max(1, static_cast<int>(lines_.size()));
    return {width, height};
}

void SourcePanel::layout(Rect bounds) {
    bounds_ = bounds;
    clamp_scroll();
}

void SourcePanel::paint(PaintContext& ctx) const {
    Canvas& canvas = ctx.canvas;
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    canvas.fill_rect({0, 0, bounds_.width, bounds_.height}, ' ', theme_.panel_background);

    if (lines_.empty()) {
        paint_empty(ctx);
        return;
    }

    if (scroll_parent_ != nullptr) {
        for (int index = 0; index < static_cast<int>(lines_.size()); ++index) {
            paint_line(ctx, index, lines_[static_cast<std::size_t>(index)]);
        }
        return;
    }

    const int visible_rows = bounds_.height;
    for (int row = 0; row < visible_rows; ++row) {
        const int index = scroll_offset_ + row;
        if (index < 0 || index >= static_cast<int>(lines_.size())) {
            continue;
        }
        paint_line(ctx, row, lines_[static_cast<std::size_t>(index)]);
    }
}

bool SourcePanel::handle_event(const Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        if (lines_.empty()) {
            return false;
        }
        if (scroll_parent_ != nullptr) {
            switch (mouse->action) {
            case tuinator::MouseAction::WheelUp:
            case tuinator::MouseAction::WheelDown:
            case tuinator::MouseAction::WheelLeft:
            case tuinator::MouseAction::WheelRight:
                return false;
            default:
                break;
            }
        }
        const bool right_click = mouse->button == tuinator::MouseButton::Right;
        const bool pointer_pick = mouse->action == tuinator::MouseAction::Click ||
                                  mouse->action == tuinator::MouseAction::Release;
        if (!pointer_pick) {
            return false;
        }

        const tuinator::Point local{mouse->position.x - bounds_.x, mouse->position.y - bounds_.y};
        if (local.x < 0 || local.y < 0 || local.y >= bounds_.height) {
            return false;
        }

        const int line = line_number_at_row(local.y);
        const bool gutter = is_gutter_click(local.x);
        const int code_col = code_column_from_local_x(local.x);

        if (right_click && on_breakpoint_context_) {
            set_focused(true);
            set_cursor_line(line);
            on_breakpoint_context_(line, code_col, context_menu_anchor(line, code_col));
            return true;
        }

        if (gutter && !right_click && on_toggle_breakpoint_) {
            set_focused(true);
            set_cursor_line(line);
            on_toggle_breakpoint_(line);
            return true;
        }

        if (!right_click && step_in_selection_ && step_in_selection_->active()) {
            if (const std::optional<int> target_index =
                    step_in_target_index_at(line, code_column_from_local_x(local.x))) {
                if (on_step_in_target_click_) {
                    set_focused(true);
                    set_cursor_line(line);
                    on_step_in_target_click_(*target_index);
                    return true;
                }
            }
        }

        if (!is_focused()) {
            set_focused(true);
        }

        set_cursor_line(line);
        return true;
    }

    const auto* key = std::get_if<KeyPress>(&event);
    if (key == nullptr || !is_focused()) {
        return false;
    }

    if ((key->character == 'b' || key->character == ' ') && !key->ctrl && !key->alt && on_toggle_breakpoint_) {
        on_toggle_breakpoint_(cursor_line_);
        return true;
    }

    if (key->character == 'j' || key->key == Key::Down) {
        move_cursor_by(1);
        return true;
    }
    if (key->character == 'k' || key->key == Key::Up) {
        move_cursor_by(-1);
        return true;
    }
    const int page_step = std::max(1, viewport_height());
    if (key->key == Key::PageDown) {
        move_cursor_by(page_step);
        return true;
    }
    if (key->key == Key::PageUp) {
        move_cursor_by(-page_step);
        return true;
    }
    if (key->character == 'g' && !key->ctrl) {
        set_cursor_line(1);
        set_scroll_offset(0);
        return true;
    }
    if (key->character == 'G' && !key->ctrl) {
        set_cursor_line(file_line_count_);
        set_scroll_offset(std::max(0, static_cast<int>(lines_.size()) - page_step));
        return true;
    }

    return false;
}

void SourcePanel::clamp_scroll() {
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll());
}

int SourcePanel::max_scroll() const {
    const int viewport = viewport_height();
    if (viewport <= 0) {
        return std::max(0, static_cast<int>(lines_.size()) - 1);
    }
    return std::max(0, static_cast<int>(lines_.size()) - viewport);
}

int SourcePanel::gutter_width() const {
    int max_line = 1;
    for (const HighlightedLine& line : lines_) {
        max_line = std::max(max_line, line.line_number);
    }
    max_line = std::max(max_line, execution_line_);
    max_line = std::max(max_line, cursor_line_);

    int digits = 1;
    while (max_line >= 10) {
        max_line /= 10;
        ++digits;
    }
    return std::max(3, digits);
}

Style SourcePanel::row_style_for_line(int line_number) const {
    if (execution_line_ > 0 && line_number == execution_line_) {
        return theme_.execution_row;
    }
    if (line_number == cursor_line_) {
        return theme_.cursor_row;
    }
    return {};
}

void SourcePanel::paint_empty(PaintContext& ctx) const {
    Canvas& canvas = ctx.canvas;
    Style style = theme_.default_text;
    style.dim = true;
    canvas.draw_text({1, 0}, kEmptyMessage, style);
}

void SourcePanel::paint_line(PaintContext& ctx, int row, const HighlightedLine& line) const {
    Canvas& canvas = ctx.canvas;
    const Style row_style = row_style_for_line(line.line_number);
    int x = 0;

    const auto breakpoint_it = breakpoints_.find(line.line_number);
    const bool has_breakpoint = breakpoint_it != breakpoints_.end();
    const bool is_conditional = has_breakpoint && breakpoint_it->second;
    const bool has_function_breakpoint = function_breakpoint_lines_.contains(line.line_number);
    const bool is_execution = execution_line_ > 0 && line.line_number == execution_line_;

    std::string breakpoint_text = "  ";
    tuinator::Style breakpoint_style = theme_.line_number;
    if (has_function_breakpoint) {
        breakpoint_text = "\u0192 ";
        breakpoint_style = theme_.breakpoint_marker;
    } else if (has_breakpoint) {
        breakpoint_text = (is_conditional ? kUiConditionalBreakpointIcon : kUiBreakpointIcon);
        breakpoint_text += " ";
        breakpoint_style = theme_.breakpoint_marker;
    }
    canvas.draw_text({x, row}, breakpoint_text, breakpoint_style);
    x += 2;

    std::string marker = "  ";
    Style marker_style = theme_.line_number;
    if (is_execution) {
        marker = std::string(kUiExecutionLineIcon) + " ";
        marker_style = theme_.execution_marker;
    }
    canvas.draw_text({x, row}, marker, marker_style);
    x += 2;

    char number_buffer[32];
    const int number_width = gutter_width();
    std::snprintf(number_buffer, sizeof(number_buffer), "%*d \u2502 ", number_width, line.line_number);
    canvas.draw_text({x, row}, number_buffer, theme_.line_number);
    x += number_width + 3;

    if (line.spans.empty()) {
        return;
    }

    int code_column = 0;
    for (const HighlightSpan& span : line.spans) {
        if (span.text.empty()) {
            continue;
        }

        std::size_t byte_offset = 0;
        while (byte_offset < span.text.size()) {
            const int remaining = bounds_.width - x;
            if (remaining <= 0) {
                return;
            }

            const std::size_t chunk_bytes =
                tuinator::text_byte_length_for_width(span.text.substr(byte_offset), remaining);
            if (chunk_bytes == 0) {
                return;
            }

            const std::string chunk = span.text.substr(byte_offset, chunk_bytes);
            const Style base = style_for_code_column(line.line_number, code_column, span.kind);
            const Style span_style =
                is_step_in_column(line.line_number, code_column) ? base : theme_.merge_row_background(base, row_style);
            canvas.draw_text({x, row}, chunk, span_style);
            const int chunk_width = tuinator::text_display_width(chunk);
            x += chunk_width;
            code_column += chunk_width;
            byte_offset += chunk_bytes;
        }
    }
}

} // namespace tui_debug_ui
