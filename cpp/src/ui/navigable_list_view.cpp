#include "tui_debug_ui/navigable_list_view.hpp"

#include <tuinator/render/paint_context.hpp>

namespace tui_debug_ui {

NavigableListView::NavigableListView(tuinator::Style item_style, tuinator::Style selected_style,
                                     tuinator::Style row_background)
    : tuinator::ListView(std::move(item_style), std::move(selected_style)),
      row_background_(std::move(row_background)) {}

void NavigableListView::paint(tuinator::PaintContext& ctx) const {
    const tuinator::Size size = bounds().size();
    if (size.width > 0 && size.height > 0) {
        ctx.canvas.fill_rect({{0, 0}, size}, ' ', row_background_);
    }
    tuinator::ListView::paint(ctx);
}

} // namespace tui_debug_ui
