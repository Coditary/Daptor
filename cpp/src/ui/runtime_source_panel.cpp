#include "tui_debug_ui/runtime_source_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <utility>

namespace tui_debug_ui {

RuntimeSourcePanel::RuntimeSourcePanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                                       const std::string& title)
    : title_style_(theme.title_source) {
    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::Plain, &theme);

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), title_style_, theme.panel_background,
                                               std::move(scroll_options), true, false);
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> RuntimeSourcePanel::release_widget() { return pane_->release_widget(); }

void RuntimeSourcePanel::set_lines(std::vector<std::string> lines) {
    if (list_ != nullptr) {
        list_->assign_items(std::move(lines));
    }
}

void RuntimeSourcePanel::set_title(std::string title) { pane_->set_title(std::move(title)); }

void RuntimeSourcePanel::set_on_refresh(std::function<void()> callback) {
    if (callback == nullptr) {
        return;
    }
    pane_->set_title_action("\u21bb", title_style_, std::move(callback));
}

tuinator::Widget* RuntimeSourcePanel::list_widget() const { return list_; }

tuinator::ScrollView* RuntimeSourcePanel::scroll_view() const { return pane_->scroll_view(); }

}  // namespace tui_debug_ui
