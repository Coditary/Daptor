#pragma once

#include <tuinator/render/style.hpp>
#include <tuinator/widgets/widget.hpp>

#include <functional>
#include <memory>

namespace tui_debug_ui {

/// Paints a solid background before delegating to a child widget.
class BackgroundWidget : public tuinator::Widget {
  public:
    BackgroundWidget(std::unique_ptr<tuinator::Widget> child, tuinator::Style background);

    tuinator::Widget* child() const { return child_.get(); }

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    tuinator::Widget* hit_test(tuinator::Point point) override;
    tuinator::Widget* hit_test_focusable(tuinator::Point point) override;
    bool has_focused_descendant() const override;
    void set_on_dirty(std::function<void(tuinator::Rect)> callback) override;
    void collect_focusable(std::vector<tuinator::Widget*>& out) override;
    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override;
    bool pointer_active() const override;

  private:
    std::unique_ptr<tuinator::Widget> child_;
    tuinator::Style background_;
};

}  // namespace tui_debug_ui
