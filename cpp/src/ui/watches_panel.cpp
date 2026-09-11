#include "tui_debug_ui/watches_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/core/event.hpp>

#include <algorithm>
#include <utility>

namespace tui_debug_ui {

WatchesPanel::WatchesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options) {
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
    list_->set_on_inline_edit_change([this](const std::string& value) {
        if (inline_edit_.active) {
            inline_edit_.expression = value;
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

    pane_ = std::make_unique<TitledScrollPane>("Watches", std::move(list), theme.title_watches,
                                                theme.panel_background, std::move(scroll_options), true, false);
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> WatchesPanel::release_widget() { return pane_->release_widget(); }

void WatchesPanel::set_lines(std::vector<std::string> lines) {
    if (list_ == nullptr) {
        return;
    }

    std::vector<std::string> items;
    items.reserve(lines.size() + (inline_edit_.active && inline_edit_.watch_index < 0 ? 1 : 0));
    inline_edit_display_index_ = -1;

    for (std::size_t row_index = 0; row_index < lines.size(); ++row_index) {
        if (inline_edit_.active && inline_edit_.watch_index == static_cast<int>(row_index)) {
            items.push_back(kInlineWatchEditRow);
            inline_edit_display_index_ = static_cast<int>(items.size()) - 1;
            continue;
        }
        items.push_back(std::move(lines[row_index]));
    }

    if (inline_edit_.active && inline_edit_.watch_index < 0) {
        items.push_back(kInlineWatchEditRow);
        inline_edit_display_index_ = static_cast<int>(items.size()) - 1;
    }

    const bool items_changed = list_->items() != items;
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

void WatchesPanel::sync_inline_edit_to_list() {
    if (list_ == nullptr || !inline_edit_.active || inline_edit_display_index_ < 0) {
        if (list_ != nullptr) {
            list_->clear_inline_row_edit();
        }
        return;
    }

    list_->set_inline_watch_row_edit(inline_edit_display_index_, inline_edit_.expression, inline_edit_.display_suffix);
    list_->set_selected_index(inline_edit_display_index_);
    list_->set_focused(true);

    if (tuinator::ScrollView* scroll = scroll_view()) {
        const int scroll_y = std::max(0, inline_edit_display_index_ - 1);
        scroll->scroll_to(0, scroll_y);
    }
}

void WatchesPanel::set_inline_edit(int watch_index, std::string expression, std::string display_suffix) {
    inline_edit_.watch_index = watch_index;
    inline_edit_.expression = std::move(expression);
    inline_edit_.display_suffix = std::move(display_suffix);
    inline_edit_.active = true;
}

void WatchesPanel::clear_inline_edit() {
    inline_edit_ = {};
    inline_edit_display_index_ = -1;
    if (list_ != nullptr) {
        list_->clear_inline_row_edit();
        list_->mark_dirty();
    }
}

bool WatchesPanel::has_inline_edit() const { return inline_edit_.active; }

bool WatchesPanel::has_active_inline_edit() const {
    return inline_edit_.active || (list_ != nullptr && list_->has_inline_row_edit());
}

std::string WatchesPanel::inline_edit_value() const {
    if (list_ != nullptr && list_->has_inline_row_edit()) {
        return list_->inline_row_edit_value();
    }
    return inline_edit_.active ? inline_edit_.expression : std::string{};
}

void WatchesPanel::focus_inline_edit() {
    if (list_ == nullptr) {
        return;
    }
    sync_inline_edit_to_list();
    list_->set_focused(true);
    list_->mark_dirty();
}

bool WatchesPanel::handle_inline_edit_key(const tuinator::Event& event) {
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

void WatchesPanel::set_on_submit(SubmitCallback callback) { on_submit_ = std::move(callback); }

void WatchesPanel::set_on_change(ChangeCallback callback) { on_change_ = std::move(callback); }

void WatchesPanel::set_on_remove(RemoveCallback callback) { on_remove_ = std::move(callback); }

void WatchesPanel::set_on_edit(EditCallback callback) { on_edit_ = std::move(callback); }

void WatchesPanel::set_on_add(AddCallback callback) { on_add_ = std::move(callback); }

void WatchesPanel::set_on_inline_edit_cancel(std::function<void()> callback) {
    on_inline_edit_cancel_ = std::move(callback);
}

void WatchesPanel::set_input_value(std::string value) {
    if (inline_edit_.active) {
        inline_edit_.expression = std::move(value);
        sync_inline_edit_to_list();
    }
}

std::string WatchesPanel::input_value() const { return inline_edit_value(); }

void WatchesPanel::focus_input() { focus_inline_edit(); }

int WatchesPanel::selected_index() const {
    return list_ != nullptr ? list_->selected_index() : -1;
}

tuinator::Widget* WatchesPanel::list_widget() const { return list_; }

tuinator::ScrollView* WatchesPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
