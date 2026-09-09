#include "tui_debug_ui/scopes_panel.hpp"

#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <utility>

namespace tui_debug_ui {

ScopesPanel::ScopesPanel(tuinator::Style title_style, tuinator::Style item_style, tuinator::Style row_background,
                         tuinator::ScrollViewOptions scroll_options, const std::string& title) {
    auto list = std::make_unique<NavigableListView>(item_style, item_style, row_background, false);
    list_ = list.get();

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), title_style, row_background,
                                               std::move(scroll_options));
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> ScopesPanel::release_widget() { return pane_->release_widget(); }

void ScopesPanel::set_scope_names(std::vector<std::string> names) {
    if (list_ != nullptr) {
        list_->assign_items(std::move(names));
    }
}

tuinator::Widget* ScopesPanel::list_widget() const { return list_; }

tuinator::ScrollView* ScopesPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
