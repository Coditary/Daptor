#include "tui_debug_ui/stacked_pane.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>
#include <variant>

namespace tui_debug_ui {

namespace {

constexpr const char* kLeadingPad = "  ";
constexpr const char* kPrevGlyph = "◄";
constexpr const char* kNextGlyph = "►";
constexpr int kTabGapWidth = 3;
constexpr int kArrowGap = 1;

}  // namespace

StackedPane::StackedPane(std::vector<Entry> entries, tuinator::Style background, tuinator::Style chrome_label,
                         tuinator::Style chrome_hint)
    : background_(std::move(background)),
      chrome_label_(std::move(chrome_label)),
      chrome_hint_(std::move(chrome_hint)) {
    for (Entry& entry : entries) {
        entries_.push_back(EntryData{std::move(entry.label), std::move(entry.widget)});
    }
    if (entries_.empty()) {
        entries_.push_back(EntryData{"", nullptr});
    }
    active_index_ = std::clamp(active_index_, 0, count() - 1);
}

const std::string& StackedPane::active_label() const {
    return entries_[static_cast<std::size_t>(active_index_)].label;
}

int StackedPane::label_width(int index) const {
    if (index < 0 || index >= count()) {
        return 0;
    }
    return tuinator::text_display_width(entries_[static_cast<std::size_t>(index)].label);
}

int StackedPane::width_for_tabs(int start, int num) const {
    if (num <= 0 || start < 0 || start + num > count()) {
        return 0;
    }

    int width = 0;
    for (int i = 0; i < num; ++i) {
        if (i > 0) {
            width += kTabGapWidth;
        }
        width += label_width(start + i);
    }
    return width;
}

int StackedPane::all_tabs_width() const {
    return width_for_tabs(0, count()) + tuinator::text_display_width(kLeadingPad);
}

int StackedPane::scroll_chrome_overhead() const {
    const int pad_width = tuinator::text_display_width(kLeadingPad);
    const int arrow_width = tuinator::text_display_width(kPrevGlyph);
    return pad_width + arrow_width + kArrowGap + arrow_width + kArrowGap + counter_width();
}

int StackedPane::tabs_budget() const {
    return std::max(0, bounds_.width - scroll_chrome_overhead());
}

int StackedPane::max_tabs_for_budget(int budget) const {
    if (budget <= 0 || count() == 0) {
        return 0;
    }

    int best = 0;
    for (int start = 0; start < count(); ++start) {
        int used = 0;
        int visible = 0;
        for (int i = start; i < count(); ++i) {
            const int segment = label_width(i) + (visible > 0 ? kTabGapWidth : 0);
            if (visible == 0 && segment > budget) {
                break;
            }
            if (visible > 0 && used + segment > budget) {
                break;
            }
            used += segment;
            ++visible;
        }
        best = std::max(best, visible);
    }
    return best;
}

std::string StackedPane::counter_text() const {
    return "(" + std::to_string(active_index_ + 1) + "/" + std::to_string(count()) + ")";
}

int StackedPane::counter_width() const { return tuinator::text_display_width(counter_text()); }

tuinator::Style StackedPane::chrome_button_style() const {
    tuinator::Style style = chrome_hint_;
    style.bold = true;
    if (style.foreground == tuinator::Color::Default) {
        style.foreground = tuinator::Color::Cyan;
    }
    return style;
}

tuinator::Style StackedPane::tab_style(int index) const {
    return index == active_index_ ? chrome_button_style() : chrome_label_;
}

void StackedPane::ensure_active_tab_visible() {
    if (count() <= 0 || bounds_.width <= 0) {
        tab_scroll_offset_ = 0;
        return;
    }

    if (all_tabs_width() <= bounds_.width) {
        tab_scroll_offset_ = 0;
        return;
    }

    const int max_visible = max_tabs_for_budget(tabs_budget());
    if (max_visible < 2) {
        tab_scroll_offset_ = 0;
        return;
    }

    const int max_start = std::max(0, count() - max_visible);
    if (active_index_ < tab_scroll_offset_) {
        tab_scroll_offset_ = active_index_;
    } else if (active_index_ >= tab_scroll_offset_ + max_visible) {
        tab_scroll_offset_ = active_index_ - max_visible + 1;
    }
    tab_scroll_offset_ = std::clamp(tab_scroll_offset_, 0, max_start);
}

void StackedPane::set_active_index(int index) {
    if (entries_.empty()) {
        return;
    }
    const int clamped = std::clamp(index, 0, count() - 1);
    const bool changed = clamped != active_index_;
    active_index_ = clamped;
    ensure_active_tab_visible();
    layout(bounds_);
    mark_dirty();
    if (changed) {
        notify_active_changed();
    }
}

void StackedPane::cycle(int delta) {
    if (entries_.empty() || delta == 0) {
        return;
    }
    int next = active_index_ + delta;
    const int n = count();
    next = ((next % n) + n) % n;
    set_active_index(next);
}

void StackedPane::set_on_active_changed(ActiveChangedCallback callback) {
    on_active_changed_ = std::move(callback);
}

void StackedPane::propagate_on_dirty(std::function<void(tuinator::Rect)> callback) {
    set_on_dirty(std::move(callback));
    for (EntryData& entry : entries_) {
        if (entry.widget != nullptr) {
            entry.widget->set_on_dirty(on_dirty_);
        }
    }
}

void StackedPane::notify_active_changed() {
    if (on_active_changed_) {
        on_active_changed_(active_index_);
    }
}

tuinator::Rect StackedPane::chrome_bounds() const {
    return {bounds_.x, bounds_.y, bounds_.width, std::min(kChromeHeight, bounds_.height)};
}

tuinator::Rect StackedPane::content_bounds() const {
    const int y = bounds_.y + std::min(kChromeHeight, bounds_.height);
    const int height = std::max(0, bounds_.height - std::min(kChromeHeight, bounds_.height));
    return {bounds_.x, y, bounds_.width, height};
}

StackedPane::ChromeLayout StackedPane::chrome_layout() const {
    ChromeLayout layout;
    layout.counter = counter_text();

    if (bounds_.width <= 0 || count() == 0) {
        return layout;
    }

    const int pad_width = tuinator::text_display_width(kLeadingPad);
    const int arrow_width = tuinator::text_display_width(kPrevGlyph);
    const int counter_w = counter_width();

    if (all_tabs_width() <= bounds_.width) {
        layout.mode = ChromeMode::AllTabs;
        int x = pad_width;
        for (int i = 0; i < count(); ++i) {
            if (i > 0) {
                x += kTabGapWidth;
            }
            const int width = label_width(i);
            layout.tabs.push_back(TabSegment{i, x, width});
            x += width;
        }
        return layout;
    }

    const int budget = tabs_budget();
    const int max_visible = max_tabs_for_budget(budget);

    if (max_visible >= 2) {
        layout.mode = ChromeMode::ScrollTabs;
        layout.first_visible_index = std::clamp(tab_scroll_offset_, 0, std::max(0, count() - max_visible));
        const int visible_count = std::min(max_visible, count() - layout.first_visible_index);

        int x = pad_width;
        layout.prev_arrow = {x, arrow_width, true};
        x += arrow_width + kArrowGap;

        for (int i = layout.first_visible_index; i < layout.first_visible_index + visible_count; ++i) {
            if (i > layout.first_visible_index) {
                x += kTabGapWidth;
            }
            const int width = label_width(i);
            layout.tabs.push_back(TabSegment{i, x, width});
            x += width;
        }

        x += kArrowGap;
        layout.next_arrow = {x, arrow_width, true};
        layout.counter_x = std::max(0, bounds_.width - counter_w);
        return layout;
    }

    layout.mode = ChromeMode::Compact;
    layout.prev_arrow = {pad_width, arrow_width, true};
    const int title_x = pad_width + arrow_width + kArrowGap;
    layout.tabs.push_back(TabSegment{active_index_, title_x, label_width(active_index_)});
    layout.next_arrow = {title_x + layout.tabs.front().width + kArrowGap, arrow_width, true};
    layout.counter_x = std::max(0, bounds_.width - counter_w);
    return layout;
}

tuinator::Size StackedPane::preferred_size() const {
    tuinator::Size content{0, 0};
    for (const EntryData& entry : entries_) {
        if (entry.widget == nullptr) {
            continue;
        }
        const tuinator::Size size = entry.widget->preferred_size();
        content.width = std::max(content.width, size.width);
        content.height = std::max(content.height, size.height);
    }
    return {content.width, content.height + kChromeHeight};
}

void StackedPane::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    ensure_active_tab_visible();

    const tuinator::Rect content = content_bounds();

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        tuinator::Widget* widget = entries_[i].widget.get();
        if (widget == nullptr) {
            continue;
        }
        if (static_cast<int>(i) == active_index_) {
            widget->layout(content);
        } else {
            widget->layout({content.x, content.y, 0, 0});
        }
    }
}

void StackedPane::paint_chrome(tuinator::Canvas& canvas) const {
    const ChromeLayout chrome = chrome_layout();
    canvas.draw_text({0, 0}, kLeadingPad, chrome_label_);

    if (chrome.mode == ChromeMode::AllTabs) {
        for (const TabSegment& tab : chrome.tabs) {
            canvas.draw_text({tab.x, 0}, entries_[static_cast<std::size_t>(tab.index)].label, tab_style(tab.index));
        }
        return;
    }

    if (chrome.prev_arrow.visible) {
        canvas.draw_text({chrome.prev_arrow.x, 0}, kPrevGlyph, chrome_button_style());
    }

    for (const TabSegment& tab : chrome.tabs) {
        canvas.draw_text({tab.x, 0}, entries_[static_cast<std::size_t>(tab.index)].label, tab_style(tab.index));
    }

    if (chrome.next_arrow.visible) {
        canvas.draw_text({chrome.next_arrow.x, 0}, kNextGlyph, chrome_button_style());
    }

    if (chrome.counter_x >= 0) {
        canvas.draw_text({chrome.counter_x, 0}, chrome.counter, chrome_label_);
    }
}

void StackedPane::paint(tuinator::PaintContext& ctx) const {
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    tuinator::Canvas& canvas = ctx.canvas;
    canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', background_);

    if (bounds_.height >= kChromeHeight) {
        paint_chrome(canvas);
    }

    const EntryData& active = entries_[static_cast<std::size_t>(active_index_)];
    if (active.widget == nullptr || content_bounds().height <= 0) {
        return;
    }

    const tuinator::Rect local{active.widget->bounds().x - bounds_.x, active.widget->bounds().y - bounds_.y,
                               active.widget->bounds().width, active.widget->bounds().height};
    ctx.with_clip(local, [&](tuinator::PaintContext& child_ctx) { active.widget->paint(child_ctx); });
}

bool StackedPane::handle_chrome_click(tuinator::Point position) {
    if (!chrome_bounds().contains(position)) {
        return false;
    }

    const tuinator::Point local{position.x - bounds_.x, position.y - bounds_.y};
    const ChromeLayout chrome = chrome_layout();

    if (chrome.prev_arrow.visible && local.x >= chrome.prev_arrow.x &&
        local.x < chrome.prev_arrow.x + chrome.prev_arrow.width) {
        cycle(-1);
        return true;
    }
    if (chrome.next_arrow.visible && local.x >= chrome.next_arrow.x &&
        local.x < chrome.next_arrow.x + chrome.next_arrow.width) {
        cycle(1);
        return true;
    }

    if (chrome.mode == ChromeMode::ScrollTabs || chrome.mode == ChromeMode::AllTabs) {
        for (const TabSegment& tab : chrome.tabs) {
            if (local.x >= tab.x && local.x < tab.x + tab.width) {
                set_active_index(tab.index);
                return true;
            }
        }
    }

    return true;
}

bool StackedPane::handle_event(const tuinator::Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        if (mouse->action == tuinator::MouseAction::Click || mouse->action == tuinator::MouseAction::Release) {
            if (handle_chrome_click(mouse->position)) {
                return true;
            }
        }
    }

    tuinator::Widget* active = entries_[static_cast<std::size_t>(active_index_)].widget.get();
    return active != nullptr && active->handle_event(event);
}

tuinator::Widget* StackedPane::hit_test(tuinator::Point point) {
    if (!bounds_.contains(point)) {
        return nullptr;
    }

    if (chrome_bounds().contains(point)) {
        return this;
    }

    tuinator::Widget* active = entries_[static_cast<std::size_t>(active_index_)].widget.get();
    if (active == nullptr) {
        return this;
    }
    if (tuinator::Widget* hit = active->hit_test(point)) {
        return hit;
    }
    return this;
}

bool StackedPane::has_focused_descendant() const {
    const tuinator::Widget* active = entries_[static_cast<std::size_t>(active_index_)].widget.get();
    return active != nullptr && active->has_focused_descendant();
}

void StackedPane::collect_focusable(std::vector<tuinator::Widget*>& out) {
    tuinator::Widget* active = entries_[static_cast<std::size_t>(active_index_)].widget.get();
    if (active != nullptr) {
        active->collect_focusable(out);
    }
}

void StackedPane::for_each_child(const std::function<void(tuinator::Widget*)>& visitor) {
    for (EntryData& entry : entries_) {
        if (entry.widget != nullptr) {
            visitor(entry.widget.get());
        }
    }
}

}  // namespace tui_debug_ui
