#include "tui_debug_ui/scopes_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/core/event.hpp>

#include <algorithm>
#include <utility>

namespace tui_debug_ui {

ScopesPanel::ScopesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                         const std::string& title) {
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
        const std::optional<std::string> name =
            variable_name_from_row(list_->items()[static_cast<std::size_t>(index)]);
        if (!name.has_value()) {
            return;
        }
        if (action == RowActionType::Edit && on_edit_variable_ != nullptr) {
            on_edit_variable_(*name);
        }
    });
    list_->set_on_inline_edit_change([this](const std::string& value) {
        if (inline_edit_.active) {
            inline_edit_.value = value;
        }
        if (on_change_ != nullptr) {
            on_change_(value);
        }
    });
    list_->set_on_inline_edit_submit([this](const std::string& value) {
        if (on_submit_ != nullptr) {
            on_submit_(value);
        }
    });
    list_->set_on_inline_edit_cancel([this]() {
        if (on_inline_edit_cancel_ != nullptr) {
            on_inline_edit_cancel_();
        }
    });
    list_->set_on_row_context([this](int index, const std::string& /*item*/, tuinator::Point anchor) {
        if (on_context_ != nullptr) {
            on_context_(index, anchor);
        }
    });
    list_->set_on_row_click([this](int index, const std::string& item, int local_x) {
        if (!NavigableListView::scope_expand_arrow_hit(item, local_x)) {
            return false;
        }
        if (on_activate_ != nullptr) {
            on_activate_(index);
        }
        return true;
    });

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), theme.title_scopes, theme.panel_background,
                                               std::move(scroll_options), true, false);
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> ScopesPanel::release_widget() { return pane_->release_widget(); }

std::optional<std::string> ScopesPanel::variable_name_from_row(const std::string& line) {
    if (const auto parsed = NavigableListView::parse_scope_variable_row(line)) {
        return std::string(parsed->name);
    }
    return std::nullopt;
}

void ScopesPanel::set_on_activate(ActivateCallback callback) {
    on_activate_ = std::move(callback);
    if (list_ != nullptr) {
        list_->set_on_activate([this](int index) {
            if (on_activate_) {
                on_activate_(index);
            }
        });
    }
}

void ScopesPanel::set_scope_names(std::vector<std::string> names, std::vector<bool> show_edit) {
    if (list_ == nullptr) {
        return;
    }

    std::vector<std::string> items;
    items.reserve(names.size());
    std::vector<bool> row_show_edit;
    row_show_edit.reserve(names.size());
    inline_edit_display_index_ = -1;

    for (std::size_t row_index = 0; row_index < names.size(); ++row_index) {
        std::string& row = names[row_index];
        const bool editable =
            row_index < show_edit.size() ? show_edit[row_index] : false;
        if (inline_edit_.active) {
            const std::optional<std::string> name = variable_name_from_row(row);
            if (name.has_value() && *name == inline_edit_.variable_name) {
                items.push_back(kInlineVariableEditRow);
                row_show_edit.push_back(false);
                inline_edit_display_index_ = static_cast<int>(items.size()) - 1;
                continue;
            }
        }
        row_show_edit.push_back(editable);
        items.push_back(std::move(row));
    }

    const bool items_changed = list_->items() != items;
    list_->set_variable_row_show_edit(row_show_edit);
    if (items_changed) {
        list_->assign_items(std::move(items));
    }
    sync_inline_edit_to_list();
    if (items_changed || inline_edit_.active) {
        list_->mark_dirty();
    }
    if (tuinator::ScrollView* scroll = scroll_view()) {
        scroll->mark_dirty();
    }
    if (pane_ != nullptr) {
        pane_->refresh_scroll_content();
    }
}

void ScopesPanel::sync_inline_edit_to_list() {
    if (list_ == nullptr || !inline_edit_.active || inline_edit_display_index_ < 0) {
        if (list_ != nullptr) {
            list_->clear_inline_row_edit();
        }
        return;
    }

    list_->set_inline_variable_row_edit(inline_edit_display_index_, inline_edit_.variable_name, inline_edit_.value);
    list_->set_selected_index(inline_edit_display_index_);
    list_->set_focused(true);

    if (tuinator::ScrollView* scroll = scroll_view()) {
        const int scroll_y = std::max(0, inline_edit_display_index_ - 1);
        scroll->scroll_to(0, scroll_y);
    }
}

void ScopesPanel::set_inline_edit(const std::string& variable_name, std::string value) {
    inline_edit_.variable_name = variable_name;
    inline_edit_.value = std::move(value);
    inline_edit_.active = true;
}

void ScopesPanel::clear_inline_edit() {
    inline_edit_ = {};
    inline_edit_display_index_ = -1;
    if (list_ != nullptr) {
        list_->clear_inline_row_edit();
        list_->mark_dirty();
    }
}

bool ScopesPanel::has_inline_edit() const { return inline_edit_.active; }

bool ScopesPanel::has_active_inline_edit() const {
    return inline_edit_.active || (list_ != nullptr && list_->has_inline_row_edit());
}

std::string ScopesPanel::inline_edit_value() const {
    if (list_ != nullptr && list_->has_inline_row_edit()) {
        return list_->inline_row_edit_value();
    }
    return inline_edit_.active ? inline_edit_.value : std::string{};
}

void ScopesPanel::focus_inline_edit() {
    if (list_ == nullptr) {
        return;
    }
    sync_inline_edit_to_list();
    list_->set_focused(true);
    list_->mark_dirty();
}

bool ScopesPanel::handle_inline_edit_key(const tuinator::Event& event) {
    if (list_ == nullptr || !list_->has_inline_row_edit()) {
        return false;
    }
    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (!list_->is_focused()) {
            list_->set_focused(true);
        }
        return list_->handle_inline_row_edit_key(*key);
    }
    return false;
}

void ScopesPanel::set_on_watch(WatchCallback callback) { on_watch_ = std::move(callback); }

void ScopesPanel::set_on_context(ContextCallback callback) { on_context_ = std::move(callback); }

void ScopesPanel::set_on_edit_variable(EditVariableCallback callback) {
    on_edit_variable_ = std::move(callback);
}

void ScopesPanel::set_on_submit(SubmitCallback callback) { on_submit_ = std::move(callback); }

void ScopesPanel::set_on_change(ChangeCallback callback) { on_change_ = std::move(callback); }

void ScopesPanel::set_on_inline_edit_cancel(std::function<void()> callback) {
    on_inline_edit_cancel_ = std::move(callback);
}

std::string ScopesPanel::input_value() const { return inline_edit_value(); }

void ScopesPanel::focus_input() { focus_inline_edit(); }

tuinator::Widget* ScopesPanel::list_widget() const { return list_; }

tuinator::ScrollView* ScopesPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
