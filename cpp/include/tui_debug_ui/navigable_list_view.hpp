#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/views/list_view.hpp>

#include <string>
#include <variant>
#include <vector>

namespace tuinator {
class ScrollView;
}  // namespace tuinator

namespace tui_debug_ui {

/// ListView that also accepts j/k for selection (vim-style navigation).
class NavigableListView : public tuinator::ListView {
  public:
    NavigableListView(tuinator::Style item_style, tuinator::Style selected_style,
                      tuinator::Style row_background = {}, bool interactive = true);

    void set_scroll_parent(tuinator::ScrollView* scroll_parent);

    void paint(tuinator::PaintContext& ctx) const override;
    void layout(tuinator::Rect bounds) override;
    bool handle_event(const tuinator::Event& event) override;

    /// Replace items and reset scroll so the first row stays visible.
    void assign_items(std::vector<std::string> items);

  private:
    void clamp_scroll_offset();
    void paint_read_only(tuinator::PaintContext& ctx) const;

    tuinator::Style row_background_;
    tuinator::Style item_style_;
    tuinator::ScrollView* scroll_parent_ = nullptr;
    bool interactive_ = true;
    int scroll_offset_ = 0;
};

}  // namespace tui_debug_ui
