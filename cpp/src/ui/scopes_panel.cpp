#include "tui_debug_ui/scopes_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/layout/box.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

#include <optional>
#include <utility>

namespace tui_debug_ui {

namespace {

std::optional<std::string> variable_name_from_row(const std::string& line) {
    if (line.size() < 5 || line[0] != ' ' || line[1] != ' ') {
        return std::nullopt;
    }
    const std::size_t equals = line.find(" = ", 2);
    if (equals == std::string::npos) {
        return std::nullopt;
    }
    return line.substr(2, equals - 2);
}

}  // namespace

ScopesPanel::ScopesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                         const std::string& title) {
    auto root = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});

    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::Scopes, &theme);
    list_->set_row_action_layout(ListRowActionLayout::VariableRow);
    list_->set_on_row_action([this](int index, RowActionType action, tuinator::Point /*anchor*/) {
        if (list_ == nullptr) {
            return;
        }
        if (index < 0 || index >= static_cast<int>(list_->items().size())) {
            return;
        }
        const std::optional<std::string> name = variable_name_from_row(list_->items()[static_cast<std::size_t>(index)]);
        if (!name.has_value()) {
            return;
        }
        if (action == RowActionType::Add && on_watch_ != nullptr) {
            on_watch_(*name);
        } else if (action == RowActionType::Edit && on_edit_variable_ != nullptr) {
            on_edit_variable_(*name);
        }
    });
    list_->set_flex(1);

    auto input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.placeholder = "> value"}, theme.label, theme.selection);
    input->set_flex(0);
    input_ = input.get();

    root->add_child(std::move(list));
    root->add_child(std::move(input));

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(root), theme.title_scopes, theme.panel_background,
                                               std::move(scroll_options), false);
}

std::unique_ptr<tuinator::Widget> ScopesPanel::release_widget() { return pane_->release_widget(); }

void ScopesPanel::set_scope_names(std::vector<std::string> names) {
    if (list_ != nullptr && list_->items() != names) {
        list_->assign_items(std::move(names));
    }
}

void ScopesPanel::set_on_watch(WatchCallback callback) { on_watch_ = std::move(callback); }

void ScopesPanel::set_on_edit_variable(EditVariableCallback callback) {
    on_edit_variable_ = std::move(callback);
}

void ScopesPanel::set_on_submit(SubmitCallback callback) {
    on_submit_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_submit([this](const std::string& value) {
            if (on_submit_ != nullptr) {
                on_submit_(value);
            }
        });
    }
}

void ScopesPanel::set_on_change(ChangeCallback callback) {
    on_change_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_change([this](const std::string& value) {
            if (on_change_ != nullptr) {
                on_change_(value);
            }
        });
    }
}

void ScopesPanel::set_input_value(std::string value) {
    if (input_ != nullptr) {
        input_->set_value(std::move(value));
    }
}

std::string ScopesPanel::input_value() const {
    return input_ != nullptr ? input_->value() : std::string{};
}

void ScopesPanel::focus_input() {
    if (input_ != nullptr) {
        input_->set_focused(true);
        if (list_ != nullptr) {
            list_->set_focused(false);
        }
    }
}

tuinator::Widget* ScopesPanel::list_widget() const { return list_; }

tuinator::TextInput* ScopesPanel::input_widget() const { return input_; }

tuinator::ScrollView* ScopesPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
