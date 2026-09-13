#include "tui_debug_ui/resizable_split_pane.hpp"

#include "tui_debug_ui/divider_paint.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/render/paint_context.hpp>

#include <algorithm>
#include <variant>

namespace tui_debug_ui {

namespace {

constexpr int kDividerThickness = 1;
constexpr int kGrabPadding = 0;
constexpr int kMinPaneSize = 6;

bool is_wheel_action(tuinator::MouseAction action) {
    return action == tuinator::MouseAction::WheelUp || action == tuinator::MouseAction::WheelDown ||
           action == tuinator::MouseAction::WheelLeft || action == tuinator::MouseAction::WheelRight;
}

bool dispatch_wheel_to_pane_under_cursor(tuinator::Widget* first, tuinator::Widget* second,
                                         const tuinator::Event& event, const tuinator::MouseEvent& mouse) {
    // Prefer the second pane when both bounds overlap (e.g. on a divider).
    for (tuinator::Widget* pane : {second, first}) {
        if (pane != nullptr && pane->bounds().contains(mouse.position)) {
            return pane->handle_event(event);
        }
    }
    return false;
}

}  // namespace

ResizableSplitPane::ResizableSplitPane(std::unique_ptr<tuinator::Widget> first,
                                       std::unique_ptr<tuinator::Widget> second,
                                       tuinator::SplitPaneOptions options, tuinator::Style background)
    : first_(std::move(first)), second_(std::move(second)), options_(std::move(options)),
      background_(std::move(background)) {}

void ResizableSplitPane::set_first_size(int size) {
    proportional_first_size_ = false;
    options_.first_size = std::max(0, size);
    mark_dirty();
}

void ResizableSplitPane::set_proportional_first_size(std::uint16_t pct) {
    proportional_first_size_ = true;
    first_size_pct_ = static_cast<std::uint16_t>(std::clamp(static_cast<int>(pct), 1, 99));
    mark_dirty();
}

void ResizableSplitPane::set_on_first_size_changed(SizeChangedCallback callback) {
    on_first_size_changed_ = std::move(callback);
}

void ResizableSplitPane::set_on_drag_state_changed(DragStateCallback callback) {
    on_drag_state_changed_ = std::move(callback);
}

void ResizableSplitPane::set_on_screen_refresh(ScreenRefreshCallback callback) {
    on_screen_refresh_ = std::move(callback);
}

void ResizableSplitPane::set_on_dirty(std::function<void(tuinator::Rect)> callback) {
    Widget::set_on_dirty(std::move(callback));
    // Children with their own override (nested splits, stacked panes, hosts)
    // recurse on their own via virtual dispatch.
    if (first_) {
        first_->set_on_dirty(on_dirty_);
    }
    if (second_) {
        second_->set_on_dirty(on_dirty_);
    }
}

void ResizableSplitPane::propagate_on_dirty(std::function<void(tuinator::Rect)> callback) {
    set_on_dirty(std::move(callback));
}

void ResizableSplitPane::request_full_redraw() {
    // Report the split's own bounds: children always stay within them, so this
    // covers both their old and new positions. An empty rect would be discarded
    // as "nothing to paint" by the Application.
    if (on_dirty_) {
        on_dirty_(bounds_);
    } else {
        mark_dirty();
    }
}

tuinator::Size ResizableSplitPane::preferred_size() const {
    const tuinator::Size first_size = first_ ? first_->preferred_size() : tuinator::Size{};
    const tuinator::Size second_size = second_ ? second_->preferred_size() : tuinator::Size{};

    if (options_.orientation == tuinator::SplitOrientation::Horizontal) {
        return {first_size.width + kDividerThickness + second_size.width,
                std::max(first_size.height, second_size.height)};
    }

    return {std::max(first_size.width, second_size.width),
            first_size.height + kDividerThickness + second_size.height};
}

int ResizableSplitPane::divider_position() const {
    if (options_.orientation == tuinator::SplitOrientation::Horizontal) {
        return first_ ? first_->bounds().width : options_.first_size;
    }
    return first_ ? first_->bounds().height : options_.first_size;
}

bool ResizableSplitPane::is_on_divider(tuinator::Point position) const {
    const int divider = divider_position();
    if (options_.orientation == tuinator::SplitOrientation::Horizontal) {
        const int local_x = position.x - bounds_.x;
        return local_x >= divider - kGrabPadding && local_x <= divider + kGrabPadding;
    }

    const int local_y = position.y - bounds_.y;
    return local_y >= divider - kGrabPadding && local_y <= divider + kGrabPadding;
}

void ResizableSplitPane::apply_drag_position(tuinator::Point position) {
    int new_first = options_.first_size;
    if (options_.orientation == tuinator::SplitOrientation::Horizontal) {
        const int total = bounds_.width - kDividerThickness;
        new_first = position.x - bounds_.x;
        new_first = std::clamp(new_first, kMinPaneSize, std::max(kMinPaneSize, total - kMinPaneSize));
    } else {
        const int total = bounds_.height - kDividerThickness;
        new_first = position.y - bounds_.y;
        new_first = std::clamp(new_first, kMinPaneSize, std::max(kMinPaneSize, total - kMinPaneSize));
    }

    if (new_first == options_.first_size) {
        return;
    }

    proportional_first_size_ = false;
    options_.first_size = new_first;
    layout(bounds_);
    request_full_redraw();
    if (on_first_size_changed_) {
        on_first_size_changed_(options_.first_size);
    }
}

void ResizableSplitPane::layout(tuinator::Rect bounds) {
    bounds_ = bounds;

    if (proportional_first_size_) {
        const int total = (options_.orientation == tuinator::SplitOrientation::Horizontal ? bounds.width
                                                                                        : bounds.height) -
                          kDividerThickness;
        if (total > 0) {
            int computed = total * static_cast<int>(first_size_pct_) / 100;
            if (total < kMinPaneSize * 2) {
                computed = std::max(1, total / 2);
            } else {
                computed = std::clamp(computed, kMinPaneSize, std::max(kMinPaneSize, total - kMinPaneSize));
            }
            options_.first_size = computed;
        }
    }

    if (options_.orientation == tuinator::SplitOrientation::Horizontal) {
        const int first_width =
            std::clamp(options_.first_size, 0, std::max(0, bounds.width - kDividerThickness));
        const int second_width = std::max(0, bounds.width - first_width - kDividerThickness);

        if (first_) {
            first_->layout({bounds.x, bounds.y, first_width, bounds.height});
        }
        if (second_) {
            second_->layout({bounds.x + first_width + kDividerThickness, bounds.y, second_width, bounds.height});
        }
        return;
    }

    const int first_height =
        std::clamp(options_.first_size, 0, std::max(0, bounds.height - kDividerThickness));
    const int second_height = std::max(0, bounds.height - first_height - kDividerThickness);

    if (first_) {
        first_->layout({bounds.x, bounds.y, bounds.width, first_height});
    }
    if (second_) {
        second_->layout({bounds.x, bounds.y + first_height + kDividerThickness, bounds.width, second_height});
    }
}

void ResizableSplitPane::paint(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds_.width > 0 && bounds_.height > 0) {
        canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', background_);
    }

    auto paint_child = [&](const tuinator::Widget* child) {
        if (child == nullptr) {
            return;
        }

        const tuinator::Rect local{child->bounds().x - bounds_.x, child->bounds().y - bounds_.y,
                                   child->bounds().width, child->bounds().height};
        ctx.with_clip(local, [&](tuinator::PaintContext& child_ctx) { child->paint(child_ctx); });
    };

    paint_child(first_.get());
    paint_child(second_.get());

    const tuinator::Style divider = options_.divider_style;

    if (options_.orientation == tuinator::SplitOrientation::Horizontal) {
        const int x = divider_position();
        draw_thick_vline(canvas, x, 0, bounds_.height, divider);
        return;
    }

    const int y = divider_position();
    draw_thick_hline(canvas, 0, y, bounds_.width, divider);
}

tuinator::Widget* ResizableSplitPane::hit_test(tuinator::Point point) {
    if (!bounds_.contains(point)) {
        return nullptr;
    }

    for (tuinator::Widget* pane : {second_.get(), first_.get()}) {
        if (pane != nullptr) {
            if (tuinator::Widget* hit = pane->hit_test(point)) {
                return hit;
            }
        }
    }

    if (is_on_divider(point)) {
        return this;
    }

    return this;
}

bool ResizableSplitPane::handle_event(const tuinator::Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        const bool press = mouse->action == tuinator::MouseAction::Press ||
                           mouse->action == tuinator::MouseAction::Click;
        const bool release = mouse->action == tuinator::MouseAction::Release;
        const bool move = mouse->action == tuinator::MouseAction::Move;

        if (press) {
            for (tuinator::Widget* pane : {first_.get(), second_.get()}) {
                if (pane != nullptr && pane->bounds().contains(mouse->position) && pane->handle_event(event)) {
                    return true;
                }
            }

            if (is_on_divider(mouse->position)) {
                if (!dragging_divider_) {
                    dragging_divider_ = true;
                    if (on_drag_state_changed_) {
                        on_drag_state_changed_(true);
                    }
                }
                apply_drag_position(mouse->position);
                return true;
            }
        }

        if (dragging_divider_ && move && mouse->left_pressed) {
            apply_drag_position(mouse->position);
            return true;
        }

        if (dragging_divider_ && release) {
            dragging_divider_ = false;
            if (on_screen_refresh_) {
                on_screen_refresh_();
            } else {
                request_full_redraw();
            }
            if (on_drag_state_changed_) {
                on_drag_state_changed_(false);
            }
            return true;
        }

        if (is_wheel_action(mouse->action)) {
            return dispatch_wheel_to_pane_under_cursor(first_.get(), second_.get(), event, *mouse);
        }

        if (bounds_.contains(mouse->position)) {
            for (tuinator::Widget* pane : {second_.get(), first_.get()}) {
                if (pane != nullptr && pane->bounds().contains(mouse->position)) {
                    return pane->handle_event(event);
                }
            }
        }
        return false;
    }

    for (tuinator::Widget* pane : {first_.get(), second_.get()}) {
        if (pane != nullptr && pane->has_focused_descendant() && pane->handle_event(event)) {
            return true;
        }
    }

    return false;
}

bool ResizableSplitPane::has_focused_descendant() const {
    return (first_ && first_->has_focused_descendant()) || (second_ && second_->has_focused_descendant());
}

void ResizableSplitPane::collect_focusable(std::vector<tuinator::Widget*>& out) {
    if (first_) {
        first_->collect_focusable(out);
    }
    if (second_) {
        second_->collect_focusable(out);
    }
}

void ResizableSplitPane::for_each_child(const std::function<void(tuinator::Widget*)>& visitor) {
    if (first_) {
        visitor(first_.get());
    }
    if (second_) {
        visitor(second_.get());
    }
}

}  // namespace tui_debug_ui
