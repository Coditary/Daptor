#include "tui_debug_ui/scopes_panel.hpp"

#include "tui_debug_ui/background_widget.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"

#include <tuinator/tuinator.hpp>

#include <utility>

namespace tui_debug_ui {

ScopesPanel::ScopesPanel(tuinator::Style border_style, tuinator::Style title_style, tuinator::Style item_style,
                         tuinator::Style selected_style, tuinator::Style row_background, const std::string& title) {
    auto list = std::make_unique<NavigableListView>(item_style, selected_style, row_background);
    list_ = list.get();

    auto panel = std::make_unique<tuinator::Panel>(title, border_style, title_style);
    panel->set_content(std::move(list));
    panel_ = std::make_unique<BackgroundWidget>(std::move(panel), row_background);
}

std::unique_ptr<tuinator::Widget> ScopesPanel::release_widget() { return std::move(panel_); }

void ScopesPanel::set_scope_names(std::vector<std::string> names) {
    if (list_ != nullptr) {
        list_->set_items(std::move(names));
    }
}

tuinator::Widget* ScopesPanel::list_widget() const { return list_; }

} // namespace tui_debug_ui
