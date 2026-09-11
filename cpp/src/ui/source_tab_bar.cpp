#include "tui_debug_ui/source_tab_bar.hpp"

#include "tui_debug_ui/divider_paint.hpp"

#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>

namespace tui_debug_ui {

namespace {

constexpr int kArrowGap = 1;

}  // namespace

SourceTabBar::SourceTabBar(tuinator::Style background, tuinator::Style inactive_label, tuinator::Style active_label,
                           tuinator::Style arrow_style, tuinator::Style close_style, tuinator::Style divider_style)
    : background_(std::move(background)),
      inactive_label_(std::move(inactive_label)),
      active_label_(std::move(active_label)),
      arrow_style_(std::move(arrow_style)),
      close_style_(std::move(close_style)),
      divider_style_(std::move(divider_style)) {}

void SourceTabBar::set_tabs(std::vector<Tab> tabs, int active_index) {
    tabs_ = std::move(tabs);
    if (tabs_.empty()) {
        active_index_ = 0;
        tab_scroll_offset_ = 0;
    } else {
        active_index_ = std::clamp(active_index, 0, static_cast<int>(tabs_.size()) - 1);
        ensure_active_tab_visible();
    }
    mark_dirty();
}

void SourceTabBar::set_on_select(SelectCallback callback) { on_select_ = std::move(callback); }

void SourceTabBar::set_on_close(CloseCallback callback) { on_close_ = std::move(callback); }

int SourceTabBar::close_glyph_width() const { return std::max(1, tuinator::text_display_width(kCloseGlyph)); }

int SourceTabBar::arrow_glyph_width() const { return tuinator::text_display_width(kPrevGlyph); }

int SourceTabBar::tab_separator_width() const {
    return kTabSeparatorPad + tuinator::text_display_width(kTabSeparator) + kTabSeparatorPad;
}

int SourceTabBar::tab_content_width(int index) const {
    if (index < 0 || index >= static_cast<int>(tabs_.size())) {
        return 0;
    }
    return tuinator::text_display_width(tabs_[static_cast<std::size_t>(index)].label) + 1 + close_glyph_width();
}

int SourceTabBar::scroll_left_overhead() const {
    return tuinator::text_display_width(kLeadingPad) + arrow_glyph_width() + kArrowGap;
}

int SourceTabBar::scroll_right_overhead() const { return kArrowGap + arrow_glyph_width(); }

int SourceTabBar::tabs_budget() const {
    return std::max(0, bounds_.width - scroll_left_overhead() - scroll_right_overhead());
}

int SourceTabBar::max_visible_tabs(int start_index) const {
    if (start_index < 0 || start_index >= static_cast<int>(tabs_.size()) || bounds_.width <= 0) {
        return 0;
    }

    int visible = 0;
    int used = 0;
    for (int i = start_index; i < static_cast<int>(tabs_.size()); ++i) {
        const int segment = tab_content_width(i) + tab_separator_width();
        const int total = scroll_left_overhead() + used + segment + scroll_right_overhead();
        if (total > bounds_.width) {
            break;
        }
        used += segment;
        ++visible;
    }
    return std::max(visible, tabs_.empty() ? 0 : 1);
}

void SourceTabBar::ensure_active_tab_visible() {
    if (tabs_.empty() || bounds_.width <= 0) {
        tab_scroll_offset_ = 0;
        return;
    }

    int total_width = tuinator::text_display_width(kLeadingPad);
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        total_width += tab_content_width(i) + tab_separator_width();
    }
    if (total_width + scroll_right_overhead() <= bounds_.width) {
        tab_scroll_offset_ = 0;
        return;
    }

    int max_visible = 0;
    for (int start = 0; start < static_cast<int>(tabs_.size()); ++start) {
        max_visible = std::max(max_visible, max_visible_tabs(start));
    }
    if (max_visible < 1) {
        tab_scroll_offset_ = 0;
        return;
    }

    const int max_start = std::max(0, static_cast<int>(tabs_.size()) - max_visible);
    if (active_index_ < tab_scroll_offset_) {
        tab_scroll_offset_ = active_index_;
    } else if (active_index_ >= tab_scroll_offset_ + max_visible) {
        tab_scroll_offset_ = active_index_ - max_visible + 1;
    }
    tab_scroll_offset_ = std::clamp(tab_scroll_offset_, 0, max_start);
}

tuinator::Size SourceTabBar::preferred_size() const { return {0, kHeight}; }

void SourceTabBar::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    ensure_active_tab_visible();
}

SourceTabBar::Layout SourceTabBar::build_layout() const {
    Layout layout;
    if (bounds_.width <= 0 || tabs_.empty()) {
        return layout;
    }

    int total_width = tuinator::text_display_width(kLeadingPad);
    for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
        total_width += tab_content_width(i) + tab_separator_width();
    }

    const bool needs_scroll = total_width + scroll_right_overhead() > bounds_.width;
    const int pad_width = tuinator::text_display_width(kLeadingPad);
    const int arrow_width = arrow_glyph_width();

    int x = 0;
    if (needs_scroll) {
        layout.prev_arrow = {x + pad_width, arrow_width, true};
        x = pad_width + arrow_width + kArrowGap;
        layout.first_visible_index =
            std::clamp(tab_scroll_offset_, 0, std::max(0, static_cast<int>(tabs_.size()) - 1));
        const int visible_count = max_visible_tabs(layout.first_visible_index);
        for (int i = layout.first_visible_index; i < layout.first_visible_index + visible_count; ++i) {
            const int label_width = tuinator::text_display_width(tabs_[static_cast<std::size_t>(i)].label);
            const int close_width = close_glyph_width();
            const int width = label_width + 1 + close_width;
            layout.tabs.push_back(TabSegment{i, x, width, x + label_width + 1, close_width});
            x += width + kTabSeparatorPad;
            layout.separator_x.push_back(x);
            x += tuinator::text_display_width(kTabSeparator) + kTabSeparatorPad;
        }
        x += kArrowGap;
        layout.next_arrow = {x, arrow_width, true};
    } else {
        x = pad_width;
        for (int i = 0; i < static_cast<int>(tabs_.size()); ++i) {
            const int label_width = tuinator::text_display_width(tabs_[static_cast<std::size_t>(i)].label);
            const int close_width = close_glyph_width();
            const int width = label_width + 1 + close_width;
            layout.tabs.push_back(TabSegment{i, x, width, x + label_width + 1, close_width});
            x += width + kTabSeparatorPad;
            layout.separator_x.push_back(x);
            x += tuinator::text_display_width(kTabSeparator) + kTabSeparatorPad;
        }
    }

    return layout;
}

tuinator::Style SourceTabBar::tab_style(int index) const {
    return index == active_index_ ? active_label_ : inactive_label_;
}

void SourceTabBar::paint(tuinator::PaintContext& ctx) const {
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    tuinator::Canvas& canvas = ctx.canvas;
    canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', background_);

    const int label_y = 0;
    const Layout layout = build_layout();
    canvas.draw_text({0, label_y}, kLeadingPad, inactive_label_);
    if (layout.prev_arrow.visible) {
        canvas.draw_text({layout.prev_arrow.x, label_y}, kPrevGlyph, arrow_style_);
    }
    for (const int separator_x : layout.separator_x) {
        canvas.draw_text({separator_x, label_y}, kTabSeparator, divider_style_);
    }
    for (const TabSegment& tab : layout.tabs) {
        const Tab& entry = tabs_[static_cast<std::size_t>(tab.index)];
        canvas.draw_text({tab.x, label_y}, entry.label, tab_style(tab.index));
        if (tab.close_x >= 0) {
            const tuinator::Style& close = tab.index == active_index_ ? active_label_ : close_style_;
            canvas.draw_text({tab.close_x, label_y}, kCloseGlyph, close);
        }
    }
    if (layout.next_arrow.visible) {
        canvas.draw_text({layout.next_arrow.x, label_y}, kNextGlyph, arrow_style_);
    }

    if (bounds_.height > kLabelRows) {
        draw_thin_hline(canvas, 0, kLabelRows, bounds_.width, divider_style_);
    }
}

bool SourceTabBar::handle_click(tuinator::Point position) {
    if (!bounds_.contains(position)) {
        return false;
    }

    const tuinator::Point local{position.x - bounds_.x, position.y - bounds_.y};
    if (local.y != 0) {
        return true;
    }

    const Layout layout = build_layout();

    if (layout.prev_arrow.visible && local.x >= layout.prev_arrow.x &&
        local.x < layout.prev_arrow.x + layout.prev_arrow.width) {
        tab_scroll_offset_ = std::max(0, tab_scroll_offset_ - 1);
        ensure_active_tab_visible();
        mark_dirty();
        return true;
    }

    if (layout.next_arrow.visible && local.x >= layout.next_arrow.x &&
        local.x < layout.next_arrow.x + layout.next_arrow.width) {
        tab_scroll_offset_ = std::min(tab_scroll_offset_ + 1,
                                      std::max(0, static_cast<int>(tabs_.size()) - max_visible_tabs(tab_scroll_offset_)));
        ensure_active_tab_visible();
        mark_dirty();
        return true;
    }

    for (const TabSegment& tab : layout.tabs) {
        if (local.x < tab.x || local.x >= tab.x + tab.width) {
            continue;
        }
        if (tab.close_x >= 0 && local.x >= tab.close_x && local.x < tab.close_x + tab.close_width) {
            if (on_close_) {
                on_close_(tab.index);
            }
            return true;
        }
        if (on_select_ && tab.index != active_index_) {
            on_select_(tab.index);
        }
        return true;
    }

    return true;
}

bool SourceTabBar::handle_event(const tuinator::Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        if (mouse->action == tuinator::MouseAction::Click || mouse->action == tuinator::MouseAction::Release) {
            return handle_click(mouse->position);
        }
    }
    return false;
}

tuinator::Widget* SourceTabBar::hit_test(tuinator::Point point) {
    return bounds_.contains(point) ? this : nullptr;
}

}  // namespace tui_debug_ui
