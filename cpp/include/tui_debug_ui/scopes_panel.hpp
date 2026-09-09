#pragma once

#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>

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

/// Locals / scope variables as a titled, scrollable list (no bordered panel frame).
class ScopesPanel {
  public:
    ScopesPanel(tuinator::Style title_style, tuinator::Style item_style, tuinator::Style row_background,
                tuinator::ScrollViewOptions scroll_options, const std::string& title = "Locals");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_scope_names(std::vector<std::string> names);
    tuinator::Widget* list_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
};

}  // namespace tui_debug_ui
