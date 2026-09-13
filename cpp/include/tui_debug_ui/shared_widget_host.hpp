#pragma once

#include <tuinator/widgets/widget.hpp>

#include <memory>

namespace tui_debug_ui {

/// Non-owning wrapper so one widget can appear in multiple stacked tabs.
class SharedWidgetHost : public tuinator::Widget {
  public:
    explicit SharedWidgetHost(tuinator::Widget* target);

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    tuinator::Widget* hit_test(tuinator::Point point) override;
    bool has_focused_descendant() const override;
    void collect_focusable(std::vector<tuinator::Widget*>& out) override;
    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override;
    void set_on_dirty(std::function<void(tuinator::Rect)> callback) override;

  private:
    tuinator::Widget* target_ = nullptr;
};

}  // namespace tui_debug_ui
