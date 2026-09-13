#include "tui_debug_ui/stacked_pane.hpp"

#include "tui_debug_ui/divider_paint.hpp"
#include "tui_debug_ui/ui_icons.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

#include <algorithm>
#include <cctype>
#include <variant>

namespace tui_debug_ui {

namespace {

constexpr const char* kLeadingPad = "  ";
constexpr const char* kPrevGlyph = "◄";
constexpr const char* kNextGlyph = "►";
constexpr const char* kAddGlyph = kUiAddIcon;
constexpr int kTabGapWidth = 3;
constexpr int kArrowGap = 1;
constexpr int kClusterGap = 1;
constexpr int kActionHitPad = 1;

std::string trim_label(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

}  // namespace

StackedPane::StackedPane(std::vector<Entry> entries, tuinator::Style background, tuinator::Style chrome_label,
                         tuinator::Style chrome_hint, tuinator::Style chrome_divider, tuinator::Style chrome_add)
    : background_(std::move(background)),
      chrome_label_(std::move(chrome_label)),
      chrome_hint_(std::move(chrome_hint)),
      chrome_divider_(std::move(chrome_divider)),
      chrome_add_(std::move(chrome_add)) {
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

int StackedPane::add_glyph_width() const { return tuinator::text_display_width(kAddGlyph); }

int StackedPane::add_action_width() const {
    if (!add_action_) {
        return 0;
    }
    return add_glyph_width();
}

int StackedPane::far_right_add_x() const {
    if (!add_action_ || bounds_.width <= 0) {
        return -1;
    }
    return bounds_.width - add_glyph_width();
}

int StackedPane::far_right_counter_x(bool show_counter) const {
    if (!show_counter || bounds_.width <= 0) {
        return -1;
    }
    int x = bounds_.width;
    if (add_action_) {
        x -= add_glyph_width() + kClusterGap;
    }
    x -= counter_width();
    return std::max(0, x);
}

void StackedPane::apply_right_cluster(ChromeLayout& layout, bool show_counter) const {
    layout.add_x = far_right_add_x();
    layout.counter_x = far_right_counter_x(show_counter);
}

int StackedPane::right_cluster_width(bool include_counter) const {
    int width = add_action_width();
    if (include_counter) {
        width += kClusterGap + counter_width();
    }
    return width;
}

int StackedPane::scroll_left_overhead() const {
    const int pad_width = tuinator::text_display_width(kLeadingPad);
    const int arrow_width = tuinator::text_display_width(kPrevGlyph);
    return pad_width + arrow_width + kArrowGap;
}

bool StackedPane::scroll_needs_counter(int first_visible_index, int visible_count) const {
    return first_visible_index > 0 || first_visible_index + visible_count < count();
}

int StackedPane::scroll_trailing_chain_width() const {
    const int arrow_width = tuinator::text_display_width(kNextGlyph);
    return kArrowGap + arrow_width + kArrowGap;
}

int StackedPane::scroll_chrome_overhead() const {
    return scroll_left_overhead() + scroll_trailing_chain_width() + right_cluster_width(true);
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

int StackedPane::max_scroll_visible_tabs(int start_index) const {
    if (start_index < 0 || start_index >= count() || bounds_.width <= 0) {
        return 0;
    }

    int visible = 0;
    int tabs_w = 0;
    for (int i = start_index; i < count(); ++i) {
        const int segment = label_width(i) + (visible > 0 ? kTabGapWidth : 0);
        const int next_visible = visible + 1;
        const bool needs_counter = scroll_needs_counter(start_index, next_visible);
        const int total =
            scroll_left_overhead() + tabs_w + segment + scroll_trailing_chain_width() + right_cluster_width(needs_counter);
        if (total > bounds_.width) {
            break;
        }
        tabs_w += segment;
        ++visible;
    }
    return std::max(visible, 1);
}

int StackedPane::max_tab_scroll_offset() const {
    int max_start = 0;
    for (int start = 0; start < count(); ++start) {
        const int visible = max_scroll_visible_tabs(start);
        if (visible > 0 && start + visible >= count()) {
            max_start = start;
        }
    }
    return max_start;
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

void StackedPane::ensure_rename_input() {
    if (rename_input_ != nullptr) {
        return;
    }
    rename_input_ = std::make_unique<tuinator::TextInput>(tuinator::TextInputOptions{.min_width = 12}, chrome_label_,
                                                          chrome_button_style());
    rename_input_->set_on_submit([this](const std::string& value) { commit_rename(value); });
    if (on_dirty_) {
        rename_input_->set_on_dirty(on_dirty_);
    }
}

void StackedPane::begin_rename(int index) {
    if (!rename_action_ || index < 0 || index >= count()) {
        return;
    }
    ensure_rename_input();
    rename_index_ = index;
    rename_input_->set_value(entries_[static_cast<std::size_t>(index)].label);
    rename_input_->set_focused(true);
    layout(bounds_);
    mark_dirty();
}

void StackedPane::cancel_rename() {
    if (!is_renaming()) {
        return;
    }
    rename_index_ = -1;
    if (rename_input_ != nullptr) {
        rename_input_->set_focused(false);
    }
    mark_dirty();
}

bool StackedPane::chrome_action_contains(tuinator::Point local) const {
    const ChromeLayout chrome = chrome_layout();

    if (chrome.prev_arrow.visible && local.x >= chrome.prev_arrow.x - kActionHitPad &&
        local.x < chrome.prev_arrow.x + chrome.prev_arrow.width + kActionHitPad) {
        return true;
    }
    if (chrome.next_arrow.visible && local.x >= chrome.next_arrow.x - kActionHitPad &&
        local.x < chrome.next_arrow.x + chrome.next_arrow.width + kActionHitPad) {
        return true;
    }
    if (chrome.add_x >= 0 && local.x >= chrome.add_x - kActionHitPad &&
        local.x < chrome.add_x + add_glyph_width() + kActionHitPad) {
        return true;
    }
    return false;
}

bool StackedPane::rename_field_contains(tuinator::Point global_point) const {
    if (!is_renaming() || !bounds_.contains(global_point)) {
        return false;
    }
    const tuinator::Rect local_bounds = rename_input_bounds();
    const tuinator::Rect global_bounds{bounds_.x + local_bounds.x, bounds_.y + local_bounds.y, local_bounds.width,
                                       local_bounds.height};
    return global_bounds.contains(global_point);
}

void StackedPane::finish_rename_on_click_outside(tuinator::Point global_point) {
    if (!is_renaming() || rename_input_ == nullptr) {
        return;
    }
    if (!rename_field_contains(global_point)) {
        cancel_rename();
    }
}

void StackedPane::commit_rename(const std::string& value) {
    if (!is_renaming()) {
        return;
    }
    const int index = rename_index_;
    rename_index_ = -1;
    if (rename_input_ != nullptr) {
        rename_input_->set_focused(false);
    }
    const std::string trimmed = trim_label(value);
    if (trimmed.empty() || index < 0 || index >= count()) {
        mark_dirty();
        return;
    }
    set_entry_label(index, trimmed);
    rename_action_(index, trimmed);
    mark_dirty();
}

void StackedPane::ensure_active_tab_visible() {
    if (count() <= 0 || bounds_.width <= 0) {
        tab_scroll_offset_ = 0;
        return;
    }

    if (all_tabs_width() + right_cluster_width(false) <= bounds_.width) {
        tab_scroll_offset_ = 0;
        return;
    }

    int max_visible_cap = 0;
    for (int start = 0; start < count(); ++start) {
        max_visible_cap = std::max(max_visible_cap, max_scroll_visible_tabs(start));
    }
    if (max_visible_cap < 2) {
        tab_scroll_offset_ = 0;
        return;
    }

    const int max_start = max_tab_scroll_offset();
    tab_scroll_offset_ = std::clamp(tab_scroll_offset_, 0, max_start);

    for (int guard = 0; guard < count(); ++guard) {
        const int visible = max_scroll_visible_tabs(tab_scroll_offset_);
        if (visible <= 0) {
            break;
        }
        if (active_index_ < tab_scroll_offset_) {
            tab_scroll_offset_ = active_index_;
        } else if (active_index_ >= tab_scroll_offset_ + visible) {
            tab_scroll_offset_ = active_index_ - visible + 1;
        } else {
            break;
        }
        tab_scroll_offset_ = std::clamp(tab_scroll_offset_, 0, max_start);
    }
}

void StackedPane::set_active_index(int index) {
    if (entries_.empty()) {
        return;
    }
    if (is_renaming() && index != rename_index_) {
        cancel_rename();
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

void StackedPane::set_add_action(std::function<void()> callback) { add_action_ = std::move(callback); }

void StackedPane::set_rename_action(std::function<void(int index, const std::string& label)> callback) {
    rename_action_ = std::move(callback);
}

void StackedPane::set_pane_menu_action(std::function<void(tuinator::Point anchor)> callback) {
    pane_menu_action_ = std::move(callback);
}

void StackedPane::set_layout_drag_press_handler(
    std::function<void(LayoutDragSourceKind kind, int tab_index, tuinator::Point position)> handler) {
    layout_drag_press_handler_ = std::move(handler);
}

void StackedPane::begin_rename_active_tab() { begin_rename(active_index_); }

void StackedPane::append_entry(std::string label, std::unique_ptr<tuinator::Widget> widget) {
    if (entries_.size() == 1 && entries_[0].widget == nullptr && entries_[0].label.empty()) {
        entries_.clear();
        active_index_ = 0;
    }
    if (on_dirty_ && widget != nullptr) {
        widget->set_on_dirty(on_dirty_);
    }
    entries_.push_back(EntryData{std::move(label), std::move(widget)});
    ensure_active_tab_visible();
    layout(bounds_);
    mark_dirty();
}

void StackedPane::remove_entry(int index) {
    if (index < 0 || index >= count()) {
        return;
    }
    if (count() == 1 && entries_[0].widget == nullptr) {
        return;
    }

    const int previous_active = active_index_;
    entries_.erase(entries_.begin() + index);
    if (entries_.empty()) {
        entries_.push_back(EntryData{"", nullptr});
    }

    if (active_index_ > index) {
        active_index_--;
    } else if (active_index_ == index) {
        active_index_ = std::min(index, count() - 1);
    }
    active_index_ = std::clamp(active_index_, 0, count() - 1);

    ensure_active_tab_visible();
    layout(bounds_);
    mark_dirty();
    if (active_index_ != previous_active) {
        notify_active_changed();
    }
}

void StackedPane::set_entry_label(int index, std::string label) {
    if (index < 0 || index >= count()) {
        return;
    }
    entries_[static_cast<std::size_t>(index)].label = std::move(label);
    if (is_renaming() && rename_index_ == index && rename_input_ != nullptr) {
        rename_input_->set_value(entries_[static_cast<std::size_t>(index)].label);
    }
    ensure_active_tab_visible();
    mark_dirty();
}

void StackedPane::set_on_dirty(std::function<void(tuinator::Rect)> callback) {
    Widget::set_on_dirty(std::move(callback));
    for (EntryData& entry : entries_) {
        if (entry.widget != nullptr) {
            entry.widget->set_on_dirty(on_dirty_);
        }
    }
    if (rename_input_ != nullptr) {
        rename_input_->set_on_dirty(on_dirty_);
    }
}

void StackedPane::propagate_on_dirty(std::function<void(tuinator::Rect)> callback) {
    set_on_dirty(std::move(callback));
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

int StackedPane::rename_field_right_limit(const ChromeLayout& chrome) const {
    int limit = bounds_.width;
    if (chrome.add_x >= 0) {
        limit = std::min(limit, chrome.add_x - kClusterGap);
    }
    if (chrome.counter_x >= 0) {
        limit = std::min(limit, chrome.counter_x - kClusterGap);
    }
    return limit;
}

tuinator::Rect StackedPane::rename_input_bounds() const {
    if (!is_renaming() || bounds_.width <= 0) {
        return {0, 0, 0, kChromeLabelRows};
    }

    constexpr int kMinRenameWidth = 12;
    const int label_w = label_width(rename_index_);
    const ChromeLayout chrome = chrome_layout();

    int x = tuinator::text_display_width(kLeadingPad);
    int right_limit = rename_field_right_limit(chrome);

    if (chrome.mode == ChromeMode::Compact) {
        if (chrome.prev_arrow.visible) {
            x = chrome.prev_arrow.x + chrome.prev_arrow.width + kArrowGap;
        }
        if (chrome.next_arrow.visible) {
            right_limit = std::min(right_limit, chrome.next_arrow.x - kClusterGap);
        }
    } else {
        for (const TabSegment& tab : chrome.tabs) {
            if (tab.index == rename_index_) {
                x = tab.x;
                break;
            }
        }
    }

    int width = std::max(0, right_limit - x);
    width = std::max(width, kMinRenameWidth);
    width = std::max(width, label_w + 2);
    width = std::min(width, bounds_.width - x);

    return {x, 0, width, kChromeLabelRows};
}

StackedPane::ChromeLayout StackedPane::chrome_layout() const {
    ChromeLayout layout;
    layout.counter = counter_text();

    if (bounds_.width <= 0 || count() == 0) {
        return layout;
    }

    const int pad_width = tuinator::text_display_width(kLeadingPad);
    const int arrow_width = tuinator::text_display_width(kPrevGlyph);

    if (all_tabs_width() + right_cluster_width(false) <= bounds_.width) {
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
        apply_right_cluster(layout, false);
        return layout;
    }

    int max_visible = 0;
    for (int start = 0; start < count(); ++start) {
        max_visible = std::max(max_visible, max_scroll_visible_tabs(start));
    }

    if (max_visible >= 2) {
        layout.mode = ChromeMode::ScrollTabs;
        layout.first_visible_index = std::clamp(tab_scroll_offset_, 0, max_tab_scroll_offset());
        const int visible_count = max_scroll_visible_tabs(layout.first_visible_index);
        const bool show_counter = scroll_needs_counter(layout.first_visible_index, visible_count);

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
        apply_right_cluster(layout, show_counter);
        return layout;
    }

    layout.mode = ChromeMode::Compact;
    layout.prev_arrow = {pad_width, arrow_width, true};
    const int title_x = pad_width + arrow_width + kArrowGap;
    const int max_title_w = std::max(0, bounds_.width - scroll_left_overhead() - scroll_trailing_chain_width() -
                                                  right_cluster_width(true));
    const int title_w = std::min(label_width(active_index_), max_title_w);
    layout.tabs.push_back(TabSegment{active_index_, title_x, title_w});
    int x = title_x + title_w + kArrowGap;
    layout.next_arrow = {x, arrow_width, true};
    apply_right_cluster(layout, true);
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
    if (is_renaming() && rename_input_ != nullptr) {
        rename_input_->layout(rename_input_bounds());
    }

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
        if (chrome.counter_x >= 0) {
            canvas.draw_text({chrome.counter_x, 0}, chrome.counter, chrome_label_);
        }
        if (chrome.add_x >= 0) {
            canvas.draw_text({chrome.add_x, 0}, kAddGlyph, chrome_add_);
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
    if (chrome.add_x >= 0) {
        canvas.draw_text({chrome.add_x, 0}, kAddGlyph, chrome_add_);
    }
}

void StackedPane::paint(tuinator::PaintContext& ctx) const {
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    tuinator::Canvas& canvas = ctx.canvas;
    canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', background_);

    if (bounds_.height >= kChromeLabelRows) {
        paint_chrome(canvas);
        if (is_renaming() && rename_input_ != nullptr) {
            const tuinator::Rect rename_bounds = rename_input_bounds();
            ctx.with_clip(rename_bounds, [&](tuinator::PaintContext& child_ctx) { rename_input_->paint(child_ctx); });
        }
    }
    if (bounds_.height >= kChromeHeight) {
        draw_thin_hline(canvas, 0, kChromeLabelRows, bounds_.width, chrome_divider_);
    }

    const EntryData& active = entries_[static_cast<std::size_t>(active_index_)];
    if (active.widget == nullptr || content_bounds().height <= 0) {
        return;
    }

    const tuinator::Rect local{active.widget->bounds().x - bounds_.x, active.widget->bounds().y - bounds_.y,
                               active.widget->bounds().width, active.widget->bounds().height};
    ctx.with_clip(local, [&](tuinator::PaintContext& child_ctx) { active.widget->paint(child_ctx); });
}

std::optional<std::pair<LayoutDragSourceKind, int>> StackedPane::chrome_drag_target(
    tuinator::Point position) const {
    if (!chrome_bounds().contains(position) || is_renaming()) {
        return std::nullopt;
    }

    const tuinator::Point local{position.x - bounds_.x, position.y - bounds_.y};
    const ChromeLayout chrome = chrome_layout();

    if (chrome.prev_arrow.visible && local.x >= chrome.prev_arrow.x &&
        local.x < chrome.prev_arrow.x + chrome.prev_arrow.width) {
        return std::nullopt;
    }
    if (chrome.next_arrow.visible && local.x >= chrome.next_arrow.x &&
        local.x < chrome.next_arrow.x + chrome.next_arrow.width) {
        return std::nullopt;
    }
    if (chrome.add_x >= 0 && local.x >= chrome.add_x && local.x < chrome.add_x + add_glyph_width()) {
        return std::nullopt;
    }

    if (chrome.mode == ChromeMode::ScrollTabs || chrome.mode == ChromeMode::AllTabs) {
        for (const TabSegment& tab : chrome.tabs) {
            if (local.x >= tab.x && local.x < tab.x + tab.width) {
                return std::make_pair(LayoutDragSourceKind::Tab, tab.index);
            }
        }
    }

    return std::make_pair(LayoutDragSourceKind::Pane, active_index_);
}

void StackedPane::handle_chrome_click(tuinator::Point position) {
    if (!chrome_bounds().contains(position)) {
        return;
    }

    const tuinator::Point local{position.x - bounds_.x, position.y - bounds_.y};
    const ChromeLayout chrome = chrome_layout();

    if (chrome.prev_arrow.visible && local.x >= chrome.prev_arrow.x &&
        local.x < chrome.prev_arrow.x + chrome.prev_arrow.width) {
        cycle(-1);
        return;
    }
    if (chrome.next_arrow.visible && local.x >= chrome.next_arrow.x &&
        local.x < chrome.next_arrow.x + chrome.next_arrow.width) {
        cycle(1);
        return;
    }

    if (chrome.mode == ChromeMode::ScrollTabs || chrome.mode == ChromeMode::AllTabs) {
        for (const TabSegment& tab : chrome.tabs) {
            if (local.x >= tab.x && local.x < tab.x + tab.width) {
                set_active_index(tab.index);
                return;
            }
        }
    }

    if (chrome.add_x >= 0 && local.x >= chrome.add_x && local.x < chrome.add_x + add_glyph_width()) {
        add_action_();
    }
}

bool StackedPane::handle_event(const tuinator::Event& event) {
    if (is_renaming() && rename_input_ != nullptr) {
        if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
            if (key->key == tuinator::Key::Escape) {
                cancel_rename();
                return true;
            }
            if (rename_input_->handle_event(event)) {
                return true;
            }
            return true;
        }

        if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
            const bool pick =
                mouse->action == tuinator::MouseAction::Click || mouse->action == tuinator::MouseAction::Release;
            if (pick && !rename_field_contains(mouse->position)) {
                cancel_rename();
            } else if (rename_input_->handle_event(event)) {
                return true;
            } else if (pick) {
                return true;
            } else {
                return false;
            }
        } else {
            return true;
        }
    }

    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        const tuinator::Point local{mouse->position.x - bounds_.x, mouse->position.y - bounds_.y};
        const bool in_chrome = chrome_bounds().contains(mouse->position);

        if (mouse->action == tuinator::MouseAction::Press && mouse->button == tuinator::MouseButton::Left) {
            if (in_chrome && chrome_action_contains(local)) {
                return true;
            }
            if (const auto drag_target = chrome_drag_target(mouse->position); drag_target.has_value()) {
                if (layout_drag_press_handler_) {
                    layout_drag_press_handler_(drag_target->first, drag_target->second, mouse->position);
                }
                return true;
            }
        }

        const bool pick =
            mouse->action == tuinator::MouseAction::Click || mouse->action == tuinator::MouseAction::Release;
        if (pick && in_chrome && mouse->button == tuinator::MouseButton::Right) {
            if (pane_menu_action_) {
                pane_menu_action_(mouse->position);
            } else if (rename_action_) {
                begin_rename(active_index_);
            }
            return true;
        }
        if (pick && in_chrome && chrome_action_contains(local)) {
            handle_chrome_click(mouse->position);
            return true;
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

    if (is_renaming() && rename_field_contains(point)) {
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
    if (is_renaming()) {
        return true;
    }
    const tuinator::Widget* active = entries_[static_cast<std::size_t>(active_index_)].widget.get();
    return active != nullptr && active->has_focused_descendant();
}

void StackedPane::collect_focusable(std::vector<tuinator::Widget*>& out) {
    if (is_renaming() && rename_input_ != nullptr) {
        out.push_back(rename_input_.get());
        return;
    }
    tuinator::Widget* active = entries_[static_cast<std::size_t>(active_index_)].widget.get();
    if (active != nullptr) {
        active->collect_focusable(out);
    }
}

void StackedPane::for_each_child(const std::function<void(tuinator::Widget*)>& visitor) {
    if (rename_input_ != nullptr) {
        visitor(rename_input_.get());
    }
    for (EntryData& entry : entries_) {
        if (entry.widget != nullptr) {
            visitor(entry.widget.get());
        }
    }
}

}  // namespace tui_debug_ui
