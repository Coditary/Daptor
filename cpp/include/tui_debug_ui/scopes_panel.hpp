#pragma once

#include <tuinator/render/style.hpp>

#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class ListView;
class Panel;
class Widget;
} // namespace tuinator

namespace tui_debug_ui {

/// Scopes list wrapped in a titled panel.
class ScopesPanel {
  public:
    ScopesPanel(tuinator::Style border_style, tuinator::Style title_style, tuinator::Style item_style,
                tuinator::Style selected_style, tuinator::Style row_background,
                const std::string& title = "Locals");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_scope_names(std::vector<std::string> names);
    tuinator::Widget* list_widget() const;

  private:
    std::unique_ptr<tuinator::Widget> panel_;
    tuinator::ListView* list_ = nullptr;
};

} // namespace tui_debug_ui
