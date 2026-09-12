#pragma once

#include <tuinator/widgets/containers/split_pane.hpp>
#include <tuinator/widgets/widget.hpp>

#include <cstdint>
#include <functional>
#include <memory>

namespace tui_debug_ui {

/// SplitPane with mouse drag on the divider (Tuinator's built-in SplitPane does not resize).
class ResizableSplitPane : public tuinator::Widget {
  public:
    using SizeChangedCallback = std::function<void(int first_size)>;
    using DragStateCallback = std::function<void(bool dragging)>;
    using ScreenRefreshCallback = std::function<void()>;

    ResizableSplitPane(std::unique_ptr<tuinator::Widget> first, std::unique_ptr<tuinator::Widget> second,
                       tuinator::SplitPaneOptions options, tuinator::Style background);

    int first_size() const { return options_.first_size; }
    void set_first_size(int size);
    void set_proportional_first_size(std::uint16_t pct);
    void set_on_first_size_changed(SizeChangedCallback callback);
    void set_on_drag_state_changed(DragStateCallback callback);
    void set_on_screen_refresh(ScreenRefreshCallback callback);
    void propagate_on_dirty(std::function<void(tuinator::Rect)> callback);

    tuinator::Widget* first() const { return first_.get(); }
    tuinator::Widget* second() const { return second_.get(); }

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    bool pointer_active() const override { return dragging_divider_; }
    tuinator::Widget* hit_test(tuinator::Point point) override;
    bool has_focused_descendant() const override;
    void collect_focusable(std::vector<tuinator::Widget*>& out) override;
    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override;

  private:
    int divider_position() const;
    bool is_on_divider(tuinator::Point position) const;
    void apply_drag_position(tuinator::Point position);

    std::unique_ptr<tuinator::Widget> first_;
    std::unique_ptr<tuinator::Widget> second_;
    tuinator::SplitPaneOptions options_;
    tuinator::Style background_;
    SizeChangedCallback on_first_size_changed_;
    DragStateCallback on_drag_state_changed_;
    ScreenRefreshCallback on_screen_refresh_;
    bool dragging_divider_ = false;
    bool proportional_first_size_ = false;
    std::uint16_t first_size_pct_ = 50;

    void request_full_redraw();
};

}  // namespace tui_debug_ui
