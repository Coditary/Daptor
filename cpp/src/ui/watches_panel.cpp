#include "tui_debug_ui/watches_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/layout/box.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

#include <utility>

namespace tui_debug_ui {

WatchesPanel::WatchesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options) {
    auto root = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});

    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::Watches, &theme);
    list_->set_row_action_layout(ListRowActionLayout::WatchRow);
    list_->set_on_row_action([this](int index, RowActionType action, tuinator::Point /*anchor*/) {
        if (action == RowActionType::Remove && on_remove_ != nullptr) {
            on_remove_(index);
        } else if (action == RowActionType::Edit && on_edit_ != nullptr) {
            on_edit_(index);
        }
    });
    list_->set_flex(1);

    auto input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.placeholder = "> watch expression"}, theme.label, theme.selection);
    input->set_flex(0);
    input_ = input.get();

    root->add_child(std::move(list));
    root->add_child(std::move(input));

    pane_ = std::make_unique<TitledScrollPane>("Watches", std::move(root), theme.title_watches,
                                                theme.panel_background, std::move(scroll_options), false);
}

std::unique_ptr<tuinator::Widget> WatchesPanel::release_widget() { return pane_->release_widget(); }

void WatchesPanel::set_lines(std::vector<std::string> lines) {
    if (list_ != nullptr) {
        list_->assign_items(std::move(lines));
    }
}

void WatchesPanel::set_on_submit(SubmitCallback callback) {
    on_submit_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_submit([this](const std::string& value) {
            if (on_submit_ != nullptr) {
                on_submit_(value);
            }
        });
    }
}

void WatchesPanel::set_on_remove(RemoveCallback callback) { on_remove_ = std::move(callback); }

void WatchesPanel::set_on_edit(EditCallback callback) { on_edit_ = std::move(callback); }

void WatchesPanel::set_on_change(ChangeCallback callback) {
    on_change_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_change([this](const std::string& value) {
            if (on_change_ != nullptr) {
                on_change_(value);
            }
        });
    }
}

void WatchesPanel::set_input_value(std::string value) {
    if (input_ != nullptr) {
        input_->set_value(std::move(value));
    }
}

std::string WatchesPanel::input_value() const {
    return input_ != nullptr ? input_->value() : std::string{};
}

void WatchesPanel::focus_input() {
    if (input_ != nullptr) {
        input_->set_focused(true);
        if (list_ != nullptr) {
            list_->set_focused(false);
        }
    }
}

int WatchesPanel::selected_index() const {
    return list_ != nullptr ? list_->selected_index() : -1;
}

tuinator::Widget* WatchesPanel::list_widget() const { return list_; }

tuinator::TextInput* WatchesPanel::input_widget() const { return input_; }

tuinator::ScrollView* WatchesPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
