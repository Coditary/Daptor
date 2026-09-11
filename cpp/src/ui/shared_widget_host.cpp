#include "tui_debug_ui/shared_widget_host.hpp"

namespace tui_debug_ui {

SharedWidgetHost::SharedWidgetHost(tuinator::Widget* target) : target_(target) {}

tuinator::Size SharedWidgetHost::preferred_size() const {
    return target_ != nullptr ? target_->preferred_size() : tuinator::Size{};
}

void SharedWidgetHost::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    if (target_ != nullptr && bounds.width > 0 && bounds.height > 0) {
        target_->layout(bounds);
    }
}

void SharedWidgetHost::paint(tuinator::PaintContext& ctx) const {
    if (target_ == nullptr || bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    const tuinator::Rect local{target_->bounds().x - bounds_.x, target_->bounds().y - bounds_.y,
                               target_->bounds().width, target_->bounds().height};
    ctx.with_clip(local, [&](tuinator::PaintContext& child_ctx) { target_->paint(child_ctx); });
}

bool SharedWidgetHost::handle_event(const tuinator::Event& event) {
    return target_ != nullptr && target_->handle_event(event);
}

tuinator::Widget* SharedWidgetHost::hit_test(tuinator::Point point) {
    if (!bounds_.contains(point) || target_ == nullptr) {
        return nullptr;
    }
    if (tuinator::Widget* hit = target_->hit_test(point)) {
        return hit;
    }
    return this;
}

bool SharedWidgetHost::has_focused_descendant() const {
    return target_ != nullptr && target_->has_focused_descendant();
}

void SharedWidgetHost::collect_focusable(std::vector<tuinator::Widget*>& out) {
    if (target_ != nullptr) {
        target_->collect_focusable(out);
    }
}

void SharedWidgetHost::for_each_child(const std::function<void(tuinator::Widget*)>& visitor) {
    if (target_ != nullptr) {
        visitor(target_);
    }
}

}  // namespace tui_debug_ui
