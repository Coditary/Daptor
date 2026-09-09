#pragma once

#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class Widget;
}  // namespace tuinator

namespace tui_debug_ui {
class NavigableListView;
class TitledScrollPane;
}  // namespace tui_debug_ui

namespace tui_debug_ui {

struct StackFrameRow {
    std::string name;
    std::uint32_t line = 0;
    std::string path;
};

/// Stack frames as a titled, scrollable list (no bordered panel frame).
class StacksPanel {
  public:
    StacksPanel(tuinator::Style title_style, tuinator::Style item_style, tuinator::Style row_background,
                tuinator::ScrollViewOptions scroll_options, const std::string& title = "MainThread");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_frames(std::vector<StackFrameRow> frames);
    void set_lines(std::vector<std::string> lines);
    tuinator::Widget* list_widget() const;

  private:
    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
};

}  // namespace tui_debug_ui
