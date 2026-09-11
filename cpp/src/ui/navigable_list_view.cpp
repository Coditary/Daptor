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

int action_icon_display_width(const char* icon) {
    return std::max(2, tuinator::text_display_width(icon));
}

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

bool scope_variable_value_is_empty(std::string_view value) {
    return std::find_if(value.begin(), value.end(),
                        [](unsigned char ch) { return !std::isspace(ch); }) == value.end();
}

std::optional<ScopeVariableRowParts> parse_scope_variable_row_impl(std::string_view line) {
    if (line.empty() || line.back() == ':') {
        return std::nullopt;
    }

    std::size_t pos = 0;
    while (pos < line.size() && line[pos] == ' ') {
        ++pos;
    }
    if (pos < 2) {
        return std::nullopt;
    }

    ScopeVariableRowParts parts{};
    parts.depth = static_cast<int>(pos / 2);

    if (line.size() >= pos + 3 && line.compare(pos, 3, kScopeExpandExpanded) == 0) {
        parts.expandable = true;
        parts.expanded = true;
        pos += 3;
    } else if (line.size() >= pos + 3 && line.compare(pos, 3, kScopeExpandCollapsed) == 0) {
        parts.expandable = true;
        parts.expanded = false;
        pos += 3;
    }

    while (pos < line.size() && line[pos] == ' ') {
        ++pos;
    }

    if (pos >= line.size()) {
        return std::nullopt;
    }

    const std::size_t equals = line.find(" = ", pos);
    if (equals == std::string_view::npos) {
        parts.name = line.substr(pos);
        parts.value = {};
        return parts;
    }

    parts.name = line.substr(pos, equals - pos);
    parts.value = line.substr(equals + 3);
    if (parts.expandable) {
        parts.value = {};
    }
    return parts;
}

std::optional<std::pair<std::string_view, std::string_view>> parse_variable_row(std::string_view line) {
    if (const auto parsed = parse_scope_variable_row_impl(line)) {
        return std::pair{parsed->name, parsed->value};
    }
    return std::nullopt;
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
    if (scroll_parent_ != nullptr) {
        scroll_parent_->refresh_content();
    }
}

tuinator::Size NavigableListView::preferred_size() const {
    if (row_action_layout_ != ListRowActionLayout::None) {
        const int height = std::max(1, static_cast<int>(items().size()));
        return {0, height};
    }
    return tuinator::ListView::preferred_size();
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

std::optional<ScopeVariableRowParts> NavigableListView::parse_scope_variable_row(std::string_view line) {
    return parse_scope_variable_row_impl(line);
}

bool NavigableListView::is_scope_loading_row(std::string_view line) {
    if (line.empty()) {
        return false;
    }
    std::size_t pos = 0;
    while (pos < line.size() && line[pos] == ' ') {
        ++pos;
    }
    return pos >= 2 && line.substr(pos) == "…";
}

void NavigableListView::set_on_row_click(RowClickCallback callback) { on_row_click_ = std::move(callback); }

void NavigableListView::set_on_row_context(RowContextCallback callback) {
    on_row_context_ = std::move(callback);
}

void NavigableListView::set_row_action_layout(ListRowActionLayout layout) {
    row_action_layout_ = layout;
    if (scroll_parent_ != nullptr) {
        scroll_parent_->refresh_content();
    }
    mark_dirty();
}

void NavigableListView::set_on_row_action(RowActionCallback callback) {
    on_row_action_ = std::move(callback);
}

void NavigableListView::set_inline_row_edit(int row, std::string prefix, std::string value) {
    inline_row_edit_.row = row;
    inline_row_edit_.prefix = std::move(prefix);
    inline_row_edit_.label.clear();
    inline_row_edit_.value = std::move(value);
    inline_row_edit_.cursor = inline_row_edit_.value.size();
    mark_dirty();
}

void NavigableListView::set_inline_variable_row_edit(int row, std::string name, std::string value) {
    inline_row_edit_.row = row;
    inline_row_edit_.prefix.clear();
    inline_row_edit_.label = std::move(name);
    inline_row_edit_.value = std::move(value);
    inline_row_edit_.cursor = inline_row_edit_.value.size();
    mark_dirty();
}

void NavigableListView::clear_inline_row_edit() {
    inline_row_edit_ = {};
    mark_dirty();
}

bool NavigableListView::has_inline_row_edit() const { return inline_row_edit_.row >= 0; }

const std::string& NavigableListView::inline_row_edit_value() const { return inline_row_edit_.value; }

bool NavigableListView::inline_row_edit_active_row(int row) const {
    return inline_row_edit_.row >= 0 && inline_row_edit_.row == row;
}

bool NavigableListView::is_inline_when_edit_row(const std::string& item) { return item == kInlineWhenEditRow; }

bool NavigableListView::is_inline_hit_edit_row(const std::string& item) { return item == kInlineHitEditRow; }

bool NavigableListView::is_inline_variable_edit_row(const std::string& item) {
    return item == kInlineVariableEditRow;
}

void NavigableListView::set_on_inline_edit_change(InlineEditChangeCallback callback) {
    on_inline_edit_change_ = std::move(callback);
}

void NavigableListView::set_on_inline_edit_submit(InlineEditSubmitCallback callback) {
    on_inline_edit_submit_ = std::move(callback);
}

void NavigableListView::set_on_inline_edit_cancel(InlineEditCancelCallback callback) {
    on_inline_edit_cancel_ = std::move(callback);
}

void NavigableListView::paint_inline_row_edit(tuinator::Canvas& canvas, int row, const std::string& prefix,
                                              int max_width) const {
    if (theme_ == nullptr || max_width <= 0) {
        return;
    }

    tuinator::Style field_style = theme_->label;
    field_style.reverse = true;
    const tuinator::Style& prefix_style =
        prefix == "    hit " ? theme_->breakpoint_hit_condition : theme_->breakpoint_condition;

    int column = 0;
    draw_segment(canvas, column, row, prefix, prefix_style, max_width);
    draw_segment(canvas, column, row, "[", field_style, max_width);

    const int bracket_width = 2;
    const int value_max_width = std::max(0, max_width - column - bracket_width);
    const std::size_t value_bytes =
        tuinator::text_byte_length_for_width(inline_row_edit_.value, value_max_width);
    const std::string visible_value = inline_row_edit_.value.substr(0, value_bytes);
    draw_segment(canvas, column, row, visible_value, field_style, max_width);
    if (inline_row_edit_.cursor >= inline_row_edit_.value.size() &&
        column + bracket_width <= max_width) {
        draw_segment(canvas, column, row, "_", field_style, max_width);
    }
    draw_segment(canvas, column, row, "]", field_style, max_width);
}

void NavigableListView::paint_inline_variable_row_edit(tuinator::Canvas& canvas, int row, int max_width) const {
    if (theme_ == nullptr || max_width <= 0) {
        return;
    }

    tuinator::Style field_style = theme_->label;
    field_style.reverse = true;

    int column = 0;
    draw_segment(canvas, column, row, "  ", item_style_, max_width);
    draw_segment(canvas, column, row, inline_row_edit_.label, theme_->variable_name, max_width);
    draw_segment(canvas, column, row, " = ", item_style_, max_width);
    draw_segment(canvas, column, row, "[", field_style, max_width);

    const int bracket_width = 2;
    const int value_max_width = std::max(0, max_width - column - bracket_width);
    const std::size_t value_bytes =
        tuinator::text_byte_length_for_width(inline_row_edit_.value, value_max_width);
    const std::string visible_value = inline_row_edit_.value.substr(0, value_bytes);
    draw_segment(canvas, column, row, visible_value, field_style, max_width);
    if (inline_row_edit_.cursor >= inline_row_edit_.value.size() && column + bracket_width <= max_width) {
        draw_segment(canvas, column, row, "_", field_style, max_width);
    }
    draw_segment(canvas, column, row, "]", field_style, max_width);
}

bool NavigableListView::handle_inline_row_edit_key(const tuinator::KeyPress& key) {
    if (inline_row_edit_.row < 0) {
        return false;
    }

    auto notify_change = [this]() {
        if (on_inline_edit_change_ != nullptr) {
            on_inline_edit_change_(inline_row_edit_.value);
        }
        mark_dirty();
    };

    switch (key.key) {
    case tuinator::Key::Enter: {
        const std::string submitted = inline_row_edit_.value;
        inline_row_edit_ = {};
        mark_dirty();
        if (on_inline_edit_submit_ != nullptr) {
            on_inline_edit_submit_(submitted);
        }
        return true;
    }
    case tuinator::Key::Escape:
        if (on_inline_edit_cancel_ != nullptr) {
            on_inline_edit_cancel_();
        }
        return true;
    case tuinator::Key::Backspace:
        if (inline_row_edit_.cursor == 0) {
            return true;
        }
        inline_row_edit_.value.erase(inline_row_edit_.cursor - 1, 1);
        --inline_row_edit_.cursor;
        notify_change();
        return true;
    case tuinator::Key::Delete:
        if (inline_row_edit_.cursor >= inline_row_edit_.value.size()) {
            return true;
        }
        inline_row_edit_.value.erase(inline_row_edit_.cursor, 1);
        notify_change();
        return true;
    case tuinator::Key::Left:
        if (inline_row_edit_.cursor > 0) {
            --inline_row_edit_.cursor;
            mark_dirty();
        }
        return true;
    case tuinator::Key::Right:
        if (inline_row_edit_.cursor < inline_row_edit_.value.size()) {
            ++inline_row_edit_.cursor;
            mark_dirty();
        }
        return true;
    default:
        break;
    }

    if (key.character >= 32 && key.character <= 126) {
        inline_row_edit_.value.insert(inline_row_edit_.cursor, 1, key.character);
        ++inline_row_edit_.cursor;
        notify_change();
        return true;
    }

    return false;
}

void NavigableListView::set_variable_row_show_edit(std::vector<bool> show_edit) {
    variable_row_show_edit_ = std::move(show_edit);
    mark_dirty();
}

bool NavigableListView::variable_row_allows_edit(int index) const {
    if (row_action_layout_ != ListRowActionLayout::VariableRow) {
        return false;
    }
    if (index < 0 || index >= static_cast<int>(variable_row_show_edit_.size())) {
        return false;
    }
    return variable_row_show_edit_[static_cast<std::size_t>(index)];
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
        paint_row_actions(canvas, index, index, item, max_width);
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

bool NavigableListView::is_breakpoint_condition_row(const std::string& item) {
    return item.rfind("    when ", 0) == 0 || item.rfind("    hit ", 0) == 0;
}

bool NavigableListView::is_breakpoint_data_row(const std::string& item) {
    return item.rfind("  \u2295 ", 0) == 0;
}

bool NavigableListView::row_shows_actions(int index, const std::string& item) const {
    if (row_action_layout_ == ListRowActionLayout::None || theme_ == nullptr) {
        return false;
    }

    switch (row_action_layout_) {
    case ListRowActionLayout::BreakpointRow:
        if (is_inline_when_edit_row(item) || is_inline_hit_edit_row(item)) {
            return false;
        }
        if (is_breakpoint_condition_row(item) || is_breakpoint_data_row(item)) {
            return true;
        }
        return item.rfind("  ", 0) == 0 && !is_scope_header_row(item);
    case ListRowActionLayout::WatchRow:
        return parse_watch_row(item).has_value();
    case ListRowActionLayout::VariableRow:
        if (is_inline_variable_edit_row(item)) {
            return false;
        }
        return parse_variable_row(item).has_value() && variable_row_allows_edit(index);
    case ListRowActionLayout::None:
        break;
    }
    return false;
}

int NavigableListView::row_action_reserve_width() const {
    switch (row_action_layout_) {
    case ListRowActionLayout::BreakpointRow:
        return kActionContentGap + tuinator::text_display_width(kAddIcon) + kActionIconGap +
               remove_icon_display_width();
    case ListRowActionLayout::WatchRow:
        return kActionContentGap + tuinator::text_display_width(kEditIcon) + kActionIconGap +
               remove_icon_display_width();
    case ListRowActionLayout::VariableRow:
        return kActionContentGap + action_icon_display_width(kEditIcon);
    case ListRowActionLayout::None:
        break;
    }
    return 0;
}

int NavigableListView::row_action_reserve_width_for_item(int index, const std::string& item) const {
    if (!row_shows_actions(index, item)) {
        return 0;
    }
    if (row_action_layout_ == ListRowActionLayout::VariableRow) {
        return kActionContentGap + action_icon_display_width(kEditIcon);
    }
    return row_action_reserve_width();
}

void NavigableListView::paint_row_actions(tuinator::Canvas& canvas, int index, int row, const std::string& item,
                                          int max_width) const {
    if (theme_ == nullptr || !row_shows_actions(index, item) || max_width <= 0) {
        return;
    }

    if (row_action_layout_ == ListRowActionLayout::VariableRow) {
        const int edit_width = action_icon_display_width(kEditIcon);
        const int edit_x = std::max(0, max_width - edit_width);
        canvas.draw_text({edit_x, row}, kEditIcon, action_edit_style(*theme_));
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

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow && is_breakpoint_condition_row(item)) {
        const int remove_width = remove_icon_display_width();
        const int remove_x = std::max(0, max_width - remove_width);
        canvas.draw_text({remove_x, row}, kRemoveIcon, action_remove_style(*theme_));

        const int edit_width = tuinator::text_display_width(kEditIcon);
        const int edit_x = std::max(0, remove_x - kActionIconGap - edit_width);
        canvas.draw_text({edit_x, row}, kEditIcon, action_edit_style(*theme_));
        return;
    }

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow && is_breakpoint_data_row(item)) {
        const int remove_width = remove_icon_display_width();
        const int remove_x = std::max(0, max_width - remove_width);
        canvas.draw_text({remove_x, row}, kRemoveIcon, action_remove_style(*theme_));
        return;
    }

    const int remove_width = remove_icon_display_width();
    const int remove_x = std::max(0, max_width - remove_width);
    canvas.draw_text({remove_x, row}, kRemoveIcon, action_remove_style(*theme_));

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow) {
        const int add_width = tuinator::text_display_width(kAddIcon);
        const int add_x = std::max(0, remove_x - kActionIconGap - add_width);
        canvas.draw_text({add_x, row}, kAddIcon, action_add_style(*theme_));
    }
}

std::optional<RowActionType> NavigableListView::row_action_at(int index, const std::string& item,
                                                              int local_x) const {
    if (!row_shows_actions(index, item)) {
        return std::nullopt;
    }

    const int max_width = bounds().width;

    if (row_action_layout_ == ListRowActionLayout::VariableRow) {
        const int edit_width = action_icon_display_width(kEditIcon);
        const int edit_x = std::max(0, max_width - edit_width);
        if (local_x >= edit_x) {
            return RowActionType::Edit;
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

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow && is_breakpoint_condition_row(item)) {
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

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow && is_breakpoint_data_row(item)) {
        const int remove_width = remove_icon_display_width();
        const int remove_x = std::max(0, max_width - remove_width);
        if (local_x >= remove_x) {
            return RowActionType::Remove;
        }
        return std::nullopt;
    }

    const int remove_width = remove_icon_display_width();
    const int remove_x = std::max(0, max_width - remove_width);
    if (local_x >= remove_x) {
        return RowActionType::Remove;
    }

    if (row_action_layout_ == ListRowActionLayout::BreakpointRow) {
        const int add_width = tuinator::text_display_width(kAddIcon);
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

    const int action_reserve = row_action_reserve_width_for_item(index, item);
    const int content_max_width = std::max(0, max_width - action_reserve);

    if (selected) {
        if (paint_mode_ == ListPaintMode::Scopes && parse_scope_variable_row_impl(item).has_value()) {
            int column = 0;
            if (!prefix.empty()) {
                draw_segment(canvas, column, row, prefix, selected_style_, content_max_width);
            }
            if (const auto parsed = parse_scope_variable_row_impl(item)) {
                draw_segment(canvas, column, row, std::string(static_cast<std::size_t>(parsed->depth) * 2, ' '),
                             selected_style_, content_max_width);
                if (parsed->expandable) {
                    draw_segment(canvas, column, row,
                                 parsed->expanded ? kScopeExpandExpanded : kScopeExpandCollapsed, selected_style_,
                                 content_max_width);
                }
                draw_segment(canvas, column, row, parsed->name, selected_style_, content_max_width);
                if (!parsed->expandable && !scope_variable_value_is_empty(parsed->value)) {
                    draw_segment(canvas, column, row, " = ", selected_style_, content_max_width);
                    const int value_budget = std::max(0, content_max_width - column);
                    if (value_budget > 0) {
                        draw_truncated(canvas, {column, row}, parsed->value, selected_style_, value_budget);
                    }
                }
            }
            paint_row_actions(canvas, index, row, item, max_width);
            return;
        }

        const int prefix_width = static_cast<int>(prefix.size());
        const int content_width = std::max(0, content_max_width - prefix_width);
        const std::size_t bytes = tuinator::text_byte_length_for_width(item, content_width);
        canvas.draw_text({0, row}, prefix + item.substr(0, bytes), selected_style_);
        paint_row_actions(canvas, index, row, item, max_width);
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

            if (paint_mode_ == ListPaintMode::Scopes && is_inline_variable_edit_row(item)) {
                paint_inline_variable_row_edit(canvas, row, max_width);
                return;
            }

            if (is_scope_loading_row(item)) {
                draw_truncated(canvas, {column, row}, item, item_style_, content_max_width - column);
                return;
            }

            if (const auto parsed = parse_scope_variable_row_impl(item)) {
                draw_segment(canvas, column, row, std::string(static_cast<std::size_t>(parsed->depth) * 2, ' '),
                             item_style_, content_max_width);
                if (parsed->expandable) {
                    draw_segment(canvas, column, row,
                                 parsed->expanded ? kScopeExpandExpanded : kScopeExpandCollapsed, item_style_,
                                 content_max_width);
                }
                draw_segment(canvas, column, row, parsed->name, theme_->variable_name, content_max_width);
                if (!parsed->expandable && !scope_variable_value_is_empty(parsed->value)) {
                    draw_segment(canvas, column, row, " = ", item_style_, content_max_width);
                    const int value_budget = std::max(0, content_max_width - column);
                    if (value_budget > 0) {
                        draw_truncated(canvas, {column, row}, parsed->value, theme_->variable_value, value_budget);
                    }
                }
                paint_row_actions(canvas, index, row, item, max_width);
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
                        paint_row_actions(canvas, index, row, item, max_width);
                        return;
                    }
                    draw_truncated(canvas, {column, row}, item, frame_style, content_max_width - column);
                    paint_row_actions(canvas, index, row, item, max_width);
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

            if (is_inline_when_edit_row(item)) {
                paint_inline_row_edit(canvas, row, "    when ", max_width);
                return;
            }

            if (is_inline_hit_edit_row(item)) {
                paint_inline_row_edit(canvas, row, "    hit ", max_width);
                return;
            }

            if (item.rfind("    when ", 0) == 0) {
                draw_segment(canvas, column, row, "    when ", item_style_, content_max_width);
                draw_segment(canvas, column, row, item.substr(9), theme_->breakpoint_condition, content_max_width);
                paint_row_actions(canvas, index, row, item, max_width);
                return;
            }

            if (item.rfind("    hit ", 0) == 0) {
                draw_segment(canvas, column, row, "    hit ", item_style_, content_max_width);
                draw_segment(canvas, column, row, item.substr(8), theme_->breakpoint_hit_condition,
                               content_max_width);
                paint_row_actions(canvas, index, row, item, max_width);
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
                    paint_row_actions(canvas, index, row, item, max_width);
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
                paint_row_actions(canvas, index, row, item, max_width);
                return;
            }
            break;
        }
        case ListPaintMode::Plain:
            break;
    }

    draw_truncated(canvas, {column, row}, item, item_style_, content_max_width - column);
    paint_row_actions(canvas, index, row, item, max_width);
}

tuinator::Point NavigableListView::row_action_anchor(int index, RowActionType action) const {
    if (index < 0 || index >= static_cast<int>(items().size())) {
        return {};
    }

    const std::string& item = items()[static_cast<std::size_t>(index)];
    if (!row_shows_actions(index, item)) {
        if (scroll_parent_ != nullptr) {
            return to_terminal_point({0, index});
        }
        return {bounds().x, bounds().y + index};
    }

    const int max_width = bounds().width;
    if (row_action_layout_ == ListRowActionLayout::VariableRow && action == RowActionType::Edit) {
        const int edit_width = action_icon_display_width(kEditIcon);
        const int edit_x = std::max(0, max_width - edit_width);
        const int local_x = edit_x + edit_width / 2;
        if (scroll_parent_ != nullptr) {
            return to_terminal_point({local_x, index});
        }
        return {bounds().x + local_x, bounds().y + index};
    }

    const int remove_width = remove_icon_display_width();
    const int remove_x = std::max(0, max_width - remove_width);
    int local_x = remove_x;

    if (action == RowActionType::Add) {
        const int add_width = tuinator::text_display_width(kAddIcon);
        local_x = std::max(0, remove_x - kActionIconGap - add_width) + add_width / 2;
    } else if (action == RowActionType::Edit) {
        const int edit_width = tuinator::text_display_width(kEditIcon);
        local_x = std::max(0, remove_x - kActionIconGap - edit_width) + edit_width / 2;
    }

    if (scroll_parent_ != nullptr) {
        return to_terminal_point({local_x, index});
    }

    return {bounds().x + local_x, bounds().y + index};
}

tuinator::Point NavigableListView::to_terminal_point(tuinator::Point event_position) const {
    if (scroll_parent_ != nullptr) {
        const tuinator::Rect scroll_bounds = scroll_parent_->bounds();
        return {
            scroll_bounds.x + event_position.x - scroll_parent_->scroll_x(),
            scroll_bounds.y + event_position.y - scroll_parent_->scroll_y(),
        };
    }

    return event_position;
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
                    on_row_action_(row, *action, to_terminal_point(mouse->position));
                    return true;
                }
            }

            if (mouse->button == tuinator::MouseButton::Right) {
                if (on_row_context_) {
                    set_focused(true);
                    set_selected_index(row);
                    on_row_context_(row, item, to_terminal_point(mouse->position));
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
        if (has_inline_row_edit() && is_focused()) {
            if (handle_inline_row_edit_key(*key)) {
                return true;
            }
        }
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
