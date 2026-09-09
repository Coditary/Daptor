#include "tui_debug_ui/navigable_list_view.hpp"

#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>

namespace tui_debug_ui {

NavigableListView::NavigableListView(tuinator::Style item_style, tuinator::Style selected_style,
                                     tuinator::Style row_background, bool interactive)
    : tuinator::ListView(std::move(item_style), std::move(selected_style)),
      row_background_(std::move(row_background)), item_style_(item_style), interactive_(interactive) {}

void NavigableListView::assign_items(std::vector<std::string> items) {
    set_items(std::move(items));
    scroll_offset_ = 0;
    if (interactive_) {
        set_selected_index(0);
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
        paint_read_only(ctx);
        return;
    }

    tuinator::ListView::paint(ctx);
}

void NavigableListView::paint_read_only(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds().width <= 0 || bounds().height <= 0) {
        return;
    }

    const int max_width = std::max(0, bounds().width);
    for (int index = 0; index < static_cast<int>(items().size()); ++index) {
        const std::string& item = items()[static_cast<std::size_t>(index)];
        const std::size_t bytes = tuinator::text_byte_length_for_width(item, max_width);
        canvas.draw_text({0, index}, item.substr(0, bytes), item_style_);
    }
}

bool NavigableListView::handle_event(const tuinator::Event& event) {
    if (!interactive_) {
        return false;
    }

    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (is_focused()) {
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
