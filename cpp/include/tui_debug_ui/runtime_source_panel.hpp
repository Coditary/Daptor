#pragma once

#include <tuinator/render/style.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class ScrollView;
class Widget;
}  // namespace tuinator

namespace tui_debug_ui {
class NavigableListView;
class TitledScrollPane;
struct DapUiTheme;
}  // namespace tui_debug_ui

namespace tui_debug_ui {

/// Adapter-provided source from DAP `source` (refreshed on each stop).
class RuntimeSourcePanel {
  public:
    RuntimeSourcePanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                       const std::string& title = "Runtime Source");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_lines(std::vector<std::string> lines);
    void set_title(std::string title);
    void set_on_refresh(std::function<void()> callback);
    tuinator::Widget* list_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    tuinator::Style title_style_;
};

}  // namespace tui_debug_ui
