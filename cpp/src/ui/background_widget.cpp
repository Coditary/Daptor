#include "tui_debug_ui/background_widget.hpp"

#include <tuinator/render/paint_context.hpp>

namespace tui_debug_ui {

BackgroundWidget::BackgroundWidget(std::unique_ptr<tuinator::Widget> child, tuinator::Style background)
    : child_(std::move(child)), background_(std::move(background)) {
    if (child_ != nullptr) {
        child_->set_on_dirty([this](tuinator::Rect /*region*/) { mark_dirty(); });
    }
}

tuinator::Size BackgroundWidget::preferred_size() const {
    return child_ != nullptr ? child_->preferred_size() : tuinator::Size{};
}

void BackgroundWidget::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    if (child_ != nullptr) {
        child_->layout(bounds);
    }
}

void BackgroundWidget::paint(tuinator::PaintContext& ctx) const {
    if (bounds_.width > 0 && bounds_.height > 0) {
        ctx.canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', background_);
    }

    if (child_ == nullptr) {
        return;
    }

    const tuinator::Rect local{{0, 0}, bounds_.size()};
    ctx.with_clip(local, [&](tuinator::PaintContext& child_ctx) { child_->paint(child_ctx); });
}

bool BackgroundWidget::handle_event(const tuinator::Event& event) {
    return child_ != nullptr && child_->handle_event(event);
}

tuinator::Widget* BackgroundWidget::hit_test(tuinator::Point point) {
    return child_ != nullptr ? child_->hit_test(point) : nullptr;
}

tuinator::Widget* BackgroundWidget::hit_test_focusable(tuinator::Point point) {
    return child_ != nullptr ? child_->hit_test_focusable(point) : nullptr;
}

bool BackgroundWidget::has_focused_descendant() const {
    return child_ != nullptr && child_->has_focused_descendant();
}

void BackgroundWidget::set_on_dirty(std::function<void(tuinator::Rect)> callback) {
    Widget::set_on_dirty(std::move(callback));
    if (child_ != nullptr) {
        child_->set_on_dirty(on_dirty_);
    }
}

void BackgroundWidget::collect_focusable(std::vector<tuinator::Widget*>& out) {
    if (child_ != nullptr) {
        child_->collect_focusable(out);
    }
}

void BackgroundWidget::for_each_child(const std::function<void(tuinator::Widget*)>& visitor) {
    if (child_ != nullptr) {
        visitor(child_.get());
    }
}

bool BackgroundWidget::pointer_active() const { return child_ != nullptr && child_->pointer_active(); }

}  // namespace tui_debug_ui
