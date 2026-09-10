#include "tui_debug_ui/navigable_list_view.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"

#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace tui_debug_ui {

namespace {

constexpr auto kDoubleClickInterval = std::chrono::milliseconds(500);
constexpr int kActionContentGap = 1;
constexpr int kActionIconGap = 1;
// Nerd Font: nf-md-plus, nf-md-trash_can_outline, nf-md-pencil
constexpr const char* kAddIcon = "\uF44D";
constexpr const char* kRemoveIcon = "\U000F01B4";
constexpr const char* kEditIcon = "\uF448";

int remove_icon_display_width() { return tuinator::text_display_width(kRemoveIcon); }

tuinator::Style action_add_style(const DapUiTheme& theme) { return theme.breakpoint_line_number; }

tuinator::Style action_edit_style(const DapUiTheme& theme) { return theme.variable_name; }

tuinator::Style action_remove_style(const DapUiTheme& theme) { return theme.control_stop; }

bool is_scope_header_row(std::string_view line) {
    return !line.empty() && line.back() == ':' && (line.size() < 2 || line[0] != ' ');
}

tuinator::Style scope_header_style(std::string_view line, const DapUiTheme& theme) {
    if (line.rfind("MainThread", 0) == 0) {
        return theme.thread_header;
    }
    if (line.rfind("Locals", 0) == 0) {
        return theme.scope_locals_header;
    }
    if (line.rfind("Globals", 0) == 0) {
        return theme.scope_globals_header;
    }
    return theme.scope_header;
}

int parse_stack_frame_index(std::string_view line) {
    const std::size_t hash = line.find('#');
    if (hash == std::string_view::npos || hash + 1 >= line.size()) {
        return -1;
    }

    int value = 0;
    std::size_t index = hash + 1;
    while (index < line.size() && line[index] >= '0' && line[index] <= '9') {
        value = value * 10 + (line[index] - '0');
        ++index;
    }
    return index > hash + 1 ? value : -1;
}

struct StackRowParts {
    bool current = false;
    std::string_view name;
    std::string_view location;
};

std::optional<StackRowParts> parse_stack_display_row(std::string_view line) {
    constexpr std::string_view current_marker = "\u25b6 ";
    constexpr std::string_view frame_marker = "  ";

    bool current = false;
    if (line.rfind(current_marker, 0) == 0) {
        current = true;
        line.remove_prefix(current_marker.size());
    } else if (line.rfind(frame_marker, 0) == 0) {
        line.remove_prefix(frame_marker.size());
    } else {
        return std::nullopt;
    }

    const std::size_t separator = line.rfind(' ');
    if (separator == std::string_view::npos || separator + 1 >= line.size()) {
        return std::nullopt;
    }

    return StackRowParts{current, line.substr(0, separator), line.substr(separator + 1)};
}

std::optional<std::pair<std::string_view, std::string_view>> parse_variable_row(std::string_view line) {
    if (line.size() < 5 || line[0] != ' ' || line[1] != ' ') {
        return std::nullopt;
    }
    const std::size_t equals = line.find(" = ", 2);
    if (equals == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{line.substr(2, equals - 2), line.substr(equals + 3)};
}

std::optional<std::pair<std::string_view, std::string_view>> parse_watch_row(std::string_view line) {
    const std::size_t equals = line.find(" = ");
    if (equals == std::string_view::npos || equals == 0) {
        return std::nullopt;
    }
    return std::pair{line.substr(0, equals), line.substr(equals + 3)};
}

void draw_truncated(tuinator::Canvas& canvas, tuinator::Point position, std::string_view text,
                    tuinator::Style style, int max_width) {
    if (max_width <= 0 || text.empty()) {
        return;
    }
    const std::size_t bytes = tuinator::text_byte_length_for_width(text, max_width);
    if (bytes > 0) {
        canvas.draw_text(position, std::string(text.substr(0, bytes)), style);
    }
}

void draw_segment(tuinator::Canvas& canvas, int& column, int row, std::string_view text, tuinator::Style style,
                  int max_width) {
    if (text.empty() || max_width <= 0) {
        return;
    }
    const int remaining = max_width - column;
    if (remaining <= 0) {
        return;
    }
    draw_truncated(canvas, {column, row}, text, style, remaining);
    column += std::min(tuinator::text_display_width(text), remaining);
}

}  // namespace

NavigableListView::NavigableListView(tuinator::Style item_style, tuinator::Style selected_style,
                                     tuinator::Style row_background, bool interactive)
    : tuinator::ListView(item_style, selected_style),
      row_background_(std::move(row_background)),
      item_style_(std::move(item_style)),
      selected_style_(std::move(selected_style)),
      interactive_(interactive) {}

void NavigableListView::set_scroll_parent(tuinator::ScrollView* scroll_parent) {
    scroll_parent_ = scroll_parent;
}

void NavigableListView::set_paint_mode(ListPaintMode mode, const DapUiTheme* theme) {
    paint_mode_ = mode;
    theme_ = theme;
    mark_dirty();
}

void NavigableListView::set_stopped_thread_headers(std::unordered_set<std::string> headers) {
    stopped_thread_headers_ = std::move(headers);
    mark_dirty();
}

void NavigableListView::set_on_activate(ActivateCallback callback) { on_activate_ = std::move(callback); }

void NavigableListView::set_on_row_click(RowClickCallback callback) { on_row_click_ = std::move(callback); }

void NavigableListView::set_on_row_context(RowContextCallback callback) {
    on_row_context_ = std::move(callback);
}

void NavigableListView::set_row_action_layout(ListRowActionLayout layout) {
    row_action_layout_ = layout;
    mark_dirty();
}

void NavigableListView::set_on_row_action(RowActionCallback callback) {
    on_row_action_ = std::move(callback);
}

void NavigableListView::set_row_action_edit_state(RowActionEditCallback callback) {
    row_action_edit_state_ = std::move(callback);
    mark_dirty();
}

void NavigableListView::assign_items(std::vector<std::string> items) {
    set_items(std::move(items));
    scroll_offset_ = 0;
    if (interactive_) {
        set_selected_index(0);
    }
    if (scroll_parent_ != nullptr) {
        scroll_parent_->refresh_content();
    }
    mark_dirty();
}

void NavigableListView::layout(tuinator::Rect bounds) {
    tuinator::ListView::layout(bounds);
    if (interactive_) {
        clamp_scroll_offset();
    }
}

void NavigableListView::clamp_scroll_offset() {
    if (bounds().height <= 0) {
        scroll_offset_ = 0;
        return;
    }

    const int max_scroll = std::max(0, static_cast<int>(items().size()) - bounds().height);
    scroll_offset_ = std::clamp(scroll_offset_, 0, max_scroll);
}

void NavigableListView::paint(tuinator::PaintContext& ctx) const {
    const tuinator::Size size = bounds().size();
    if (size.width > 0 && size.height > 0) {
        ctx.canvas.fill_rect({{0, 0}, size}, ' ', row_background_);
    }

    if (!interactive_) {
        if (paint_mode_ != ListPaintMode::Plain && theme_ != nullptr) {
            paint_themed(ctx);
        } else {
            paint_read_only(ctx);
        }
        return;
    }

    if (paint_mode_ != ListPaintMode::Plain && theme_ != nullptr) {
        paint_themed(ctx);
        return;
    }

    paint_plain_interactive(ctx);
}

void NavigableListView::paint_read_only(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds().width <= 0 || bounds().height <= 0) {
        return;
    }

    const int max_width = std::max(0, bounds().width);
    for (int index = 0; index < static_cast<int>(items().size()); ++index) {
        const std::string& item = items()[static_cast<std::size_t>(index)];
        paint_themed_row(canvas, index, index, item, "", false, max_width);
    }
}

void NavigableListView::paint_plain_interactive(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds().width <= 0 || bounds().height <= 0) {
        return;
    }

    const bool show_selection = is_focused();
    const int max_width = std::max(0, bounds().width);
    const int action_reserve = row_action_reserve_width();
    for (int index = 0; index < static_cast<int>(items().size()); ++index) {
        const bool selected = show_selection && index == selected_index();
        const std::string prefix = selected ? "> " : (show_selection ? "  " : "");
        const tuinator::Style& style = selected ? selected_style_ : item_style_;
        const std::string& item = items()[static_cast<std::size_t>(index)];

        const int prefix_width = static_cast<int>(prefix.size());
        const int content_width = std::max(0, max_width - prefix_width - action_reserve);
        const std::size_t bytes = tuinator::text_byte_length_for_width(item, content_width);
        canvas.draw_text({0, index}, prefix + item.substr(0, bytes), style);
        paint_row_actions(canvas, index, item, max_width);
    }
}

void NavigableListView::paint_themed(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds().width <= 0 || bounds().height <= 0) {
        return;
    }

    if (!interactive_) {
        paint_read_only(ctx);
        return;
    }

    const bool show_selection = is_focused() && paint_mode_ != ListPaintMode::Watches &&
                                paint_mode_ != ListPaintMode::Scopes &&
                                paint_mode_ != ListPaintMode::Breakpoints;
    const int max_width = std::max(0, bounds().width);
    for (int index = 0; index < static_cast<int>(items().size()); ++index) {
        const bool selected = show_selection && index == selected_index();
        const std::string prefix = selected ? "> " : (show_selection ? "  " : "");
        const std::string& item = items()[static_cast<std::size_t>(index)];
        paint_themed_row(canvas, index, index, item, prefix, selected, max_width);
    }
}

bool NavigableListView::row_shows_actions(int index, const std::string& item) const {
    if (row_action_layout_ == ListRowActionLayout::None || theme_ == nullptr) {
        return false;
    }

    switch (row_action_layout_) {
    case ListRowActionLayout::BreakpointRow:
        return item.rfind("  ", 0) == 0 && item.rfind("    when ", 0) != 0 && !is_scope_header_row(item);
    case ListRowActionLayout::WatchRow:
        return parse_watch_row(item).has_value();
    case ListRowActionLayout::VariableRow:
        return parse_variable_row(item).has_value();
    case ListRowActionLayout::None:
        break;
    }
    return false;
}

int NavigableListView::row_add_action_width() const {
    if (row_action_layout_ != ListRowActionLayout::BreakpointRow) {
        return 0;
    }
    return std::max(tuinator::text_display_width(kAddIcon), tuinator::text_display_width(kEditIcon));
}

std::string NavigableListView::row_add_action_glyph(int index) const {
    if (row_action_layout_ != ListRowActionLayout::BreakpointRow) {
        return {};
    }

    if (row_action_edit_state_ && index >= 0 && row_action_edit_state_(index)) {
        return kEditIcon;
    }
    return kAddIcon;
}

int NavigableListView::row_action_reserve_width() const {
    switch (row_action_layout_) {
    case ListRowActionLayout::BreakpointRow:
        return kActionContentGap + row_add_action_width() + kActionIconGap + remove_icon_display_width();
    case ListRowActionLayout::WatchRow:
        return kActionContentGap + tuinator::text_display_width(kEditIcon) + kActionIconGap +
               remove_icon_display_width();
    case ListRowActionLayout::VariableRow:
        return kActionContentGap + tuinator::text_display_width(kAddIcon) + kActionIconGap +
               tuinator::text_display_width(kEditIcon);
    case ListRowActionLayout::None:
        break;
    }
    return 0;
}

void NavigableListView::paint_row_actions(tuinator::Canvas& canvas, int row, const std::string& item,
                                          int max_width) const {
    if (theme_ == nullptr || !row_shows_actions(row, item) || max_width <= 0) {
        return;
    }

    if (row_action_layout_ == ListRowActionLayout::VariableRow) {
        const int edit_width = tuinator::text_display_width(kEditIcon);
        const int edit_x = std::max(0, max_width - edit_width);
        canvas.draw_text({edit_x, row}, kEditIcon, action_edit_style(*theme_));

        const int add_width = tuinator::text_display_width(kAddIcon);
        const int add_x = std::max(0, edit_x - kActionIconGap - add_width);
        canvas.draw_text({add_x, row}, kAddIcon, action_add_style(*theme_));
        return;
    }

    if (row_action_layout_ == ListRowActionLayout::WatchRow) {
        const int remove_width = remove_icon_display_width();
        const int remove_x = std::max(0, max_width - remove_width);
        canvas.draw_text({remove_x, row}, kRemoveIcon, action_remove_style(*theme_));

        const int edit_width = tuinator::text_display_width(kEditIcon);
        const int edit_x = std::max(0, remove_x - kActionIconGap - edit_width);
        canvas.draw_text({edit_x, row}, kEditIcon, action_edit_style(*theme_));
        return;
    }

    const int remove_width = remove_icon_display_width();
    const int remove_x = std::max(0, max_width - remove_width);
    canvas.draw_text({remove_x, row}, kRemoveIcon, action_remove_style(*theme_));

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow) {
        const std::string add_glyph = row_add_action_glyph(row);
        if (!add_glyph.empty()) {
            const int add_width = tuinator::text_display_width(add_glyph);
            const int add_x = std::max(0, remove_x - kActionIconGap - add_width);
            const tuinator::Style& add_style =
                add_glyph == kEditIcon ? action_edit_style(*theme_) : action_add_style(*theme_);
            canvas.draw_text({add_x, row}, add_glyph, add_style);
        }
    }
}

std::optional<RowActionType> NavigableListView::row_action_at(int index, const std::string& item,
                                                              int local_x) const {
    if (!row_shows_actions(index, item)) {
        return std::nullopt;
    }

    const int max_width = bounds().width;

    if (row_action_layout_ == ListRowActionLayout::VariableRow) {
        const int edit_width = tuinator::text_display_width(kEditIcon);
        const int edit_x = std::max(0, max_width - edit_width);
        if (local_x >= edit_x) {
            return RowActionType::Edit;
        }
        const int add_width = tuinator::text_display_width(kAddIcon);
        const int add_x = std::max(0, edit_x - kActionIconGap - add_width);
        if (local_x >= add_x && local_x < edit_x - kActionIconGap) {
            return RowActionType::Add;
        }
        return std::nullopt;
    }

    if (row_action_layout_ == ListRowActionLayout::WatchRow) {
        const int remove_width = remove_icon_display_width();
        const int remove_x = std::max(0, max_width - remove_width);
        if (local_x >= remove_x) {
            return RowActionType::Remove;
        }
        const int edit_width = tuinator::text_display_width(kEditIcon);
        const int edit_x = std::max(0, remove_x - kActionIconGap - edit_width);
        if (local_x >= edit_x && local_x < remove_x - kActionIconGap) {
            return RowActionType::Edit;
        }
        return std::nullopt;
    }

    const int remove_width = remove_icon_display_width();
    const int remove_x = std::max(0, max_width - remove_width);
    if (local_x >= remove_x) {
        return RowActionType::Remove;
    }

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow) {
        const std::string add_glyph = row_add_action_glyph(index);
        const int add_width = tuinator::text_display_width(add_glyph);
        const int add_x = std::max(0, remove_x - kActionIconGap - add_width);
        if (local_x >= add_x && local_x < remove_x - kActionIconGap) {
            return RowActionType::Add;
        }
    }

    return std::nullopt;
}

void NavigableListView::paint_themed_row(tuinator::Canvas& canvas, int row, int index, const std::string& item,
                                         const std::string& prefix, bool selected, int max_width) const {
    if (theme_ == nullptr) {
        return;
    }

    const int action_reserve = row_shows_actions(index, item) ? row_action_reserve_width() : 0;
    const int content_max_width = std::max(0, max_width - action_reserve);

    if (selected) {
        const int prefix_width = static_cast<int>(prefix.size());
        const int content_width = std::max(0, content_max_width - prefix_width);
        const std::size_t bytes = tuinator::text_byte_length_for_width(item, content_width);
        canvas.draw_text({0, row}, prefix + item.substr(0, bytes), selected_style_);
        paint_row_actions(canvas, row, item, max_width);
        return;
    }

    int column = 0;
    if (!prefix.empty()) {
        draw_segment(canvas, column, row, prefix, item_style_, max_width);
    }

    switch (paint_mode_) {
        case ListPaintMode::Scopes:
        case ListPaintMode::DebugSidebar: {
            if (item.empty()) {
                return;
            }
            if (is_scope_header_row(item)) {
                draw_truncated(canvas, {column, row}, item, scope_header_style(item, *theme_),
                               content_max_width - column);
                return;
            }

            if (const auto parsed = parse_variable_row(item)) {
                draw_segment(canvas, column, row, "  ", item_style_, content_max_width);
                draw_segment(canvas, column, row, parsed->first, theme_->variable_name, content_max_width);
                draw_segment(canvas, column, row, " = ", item_style_, content_max_width);
                draw_segment(canvas, column, row, parsed->second, theme_->variable_value, content_max_width);
                paint_row_actions(canvas, row, item, max_width);
                return;
            }

            if (paint_mode_ == ListPaintMode::DebugSidebar) {
                const int frame_index = parse_stack_frame_index(item);
                if (frame_index >= 0) {
                    const tuinator::Style frame_style =
                        frame_index == 0 ? theme_->frame_current : theme_->frame_normal;
                    const std::size_t at = item.find(" @ ");
                    if (at != std::string::npos) {
                        draw_segment(canvas, column, row, item.substr(0, at), frame_style, content_max_width);
                        draw_segment(canvas, column, row, item.substr(at), theme_->frame_normal, content_max_width);
                        paint_row_actions(canvas, row, item, max_width);
                        return;
                    }
                    draw_truncated(canvas, {column, row}, item, frame_style, content_max_width - column);
                    paint_row_actions(canvas, row, item, max_width);
                    return;
                }
            }
            break;
        }
        case ListPaintMode::Stacks: {
            if (is_scope_header_row(item)) {
                const tuinator::Style header_style =
                    stopped_thread_headers_.contains(item) ? theme_->thread_stopped : theme_->thread_header;
                draw_truncated(canvas, {column, row}, item, header_style, content_max_width - column);
                return;
            }

            if (const auto parsed = parse_stack_display_row(item)) {
                const tuinator::Style name_style = parsed->current ? theme_->thread_stopped : theme_->frame_normal;
                const std::string marker = parsed->current ? "\u25b6 " : "  ";
                draw_segment(canvas, column, row, marker, item_style_, content_max_width);
                draw_segment(canvas, column, row, parsed->name, name_style, content_max_width);
                draw_segment(canvas, column, row, " ", item_style_, content_max_width);
                draw_segment(canvas, column, row, parsed->location, theme_->frame_location, content_max_width);
                return;
            }
            break;
        }
        case ListPaintMode::Breakpoints: {
            if (is_scope_header_row(item) && item.find('/') == std::string::npos) {
                draw_truncated(canvas, {column, row}, item, theme_->breakpoint_file, content_max_width - column);
                return;
            }

            if (item.rfind("    when ", 0) == 0) {
                draw_segment(canvas, column, row, "    when ", item_style_, content_max_width);
                draw_segment(canvas, column, row, item.substr(9), theme_->breakpoint_condition, content_max_width);
                return;
            }

            if (item.rfind("  ", 0) == 0) {
                std::string_view rest = item;
                rest.remove_prefix(2);
                const std::size_t space = rest.find(' ');
                const std::string line_number = space == std::string_view::npos
                                                    ? std::string(rest)
                                                    : std::string(rest.substr(0, space));
                if (!line_number.empty()) {
                    draw_segment(canvas, column, row, "  ", item_style_, content_max_width);
                    draw_segment(canvas, column, row, line_number, theme_->breakpoint_line_number, content_max_width);
                    if (space != std::string_view::npos) {
                        draw_segment(canvas, column, row, rest.substr(space), item_style_, content_max_width);
                    }
                    paint_row_actions(canvas, row, item, max_width);
                    return;
                }
            }
            break;
        }
        case ListPaintMode::Watches: {
            if (const auto parsed = parse_watch_row(item)) {
                draw_segment(canvas, column, row, parsed->first, theme_->variable_name, content_max_width);
                draw_segment(canvas, column, row, " = ", item_style_, content_max_width);
                const tuinator::Style& value_style =
                    parsed->second.rfind("<error:", 0) == 0 ? theme_->console_stderr : theme_->variable_value;
                draw_segment(canvas, column, row, parsed->second, value_style, content_max_width);
                paint_row_actions(canvas, row, item, max_width);
                return;
            }
            break;
        }
        case ListPaintMode::Plain:
            break;
    }

    draw_truncated(canvas, {column, row}, item, item_style_, content_max_width - column);
    paint_row_actions(canvas, row, item, max_width);
}

bool NavigableListView::handle_event(const tuinator::Event& event) {
    if (!interactive_) {
        return false;
    }

    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        const bool pointer_pick = mouse->action == tuinator::MouseAction::Click ||
                                  mouse->action == tuinator::MouseAction::Release;
        if (pointer_pick) {
            if (!contains_point(mouse->position)) {
                return false;
            }

            const tuinator::Point local{
                mouse->position.x - bounds().x,
                mouse->position.y - bounds().y,
            };
            const int row = local.y;
            if (row < 0 || row >= static_cast<int>(items().size())) {
                return false;
            }

            const std::string& item = items()[static_cast<std::size_t>(row)];
            if (const std::optional<RowActionType> action = row_action_at(row, item, local.x)) {
                if (on_row_action_ != nullptr) {
                    set_focused(true);
                    set_selected_index(row);
                    on_row_action_(row, *action);
                    return true;
                }
            }

            if (mouse->button == tuinator::MouseButton::Right) {
                if (on_row_context_) {
                    set_focused(true);
                    set_selected_index(row);
                    on_row_context_(row, item, mouse->position);
                    return true;
                }
                return false;
            }

            if (on_row_click_ && on_row_click_(row, item)) {
                set_focused(true);
                set_selected_index(row);
                return true;
            }

            set_focused(true);
            set_selected_index(row);

            if (on_activate_) {
                const auto now = std::chrono::steady_clock::now();
                const bool double_click = row == last_click_row_ && last_click_time_ != std::chrono::steady_clock::time_point{} &&
                                          now - last_click_time_ <= kDoubleClickInterval;
                last_click_row_ = row;
                last_click_time_ = now;
                if (double_click) {
                    on_activate_(row);
                    last_click_row_ = -1;
                }
            }

            return true;
        }

        return false;
    }

    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (is_focused()) {
            if (key->key == tuinator::Key::Enter) {
                if (on_activate_ && selected_index() >= 0 &&
                    selected_index() < static_cast<int>(items().size())) {
                    on_activate_(selected_index());
                    return true;
                }
                return false;
            }
            if (key->character == 'j') {
                tuinator::KeyPress down{tuinator::Key::Down, '\0'};
                return tuinator::ListView::handle_event(down);
            }
            if (key->character == 'k') {
                tuinator::KeyPress up{tuinator::Key::Up, '\0'};
                return tuinator::ListView::handle_event(up);
            }
        }
    }

    return tuinator::ListView::handle_event(event);
}

}  // namespace tui_debug_ui
