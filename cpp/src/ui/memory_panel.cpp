#include "tui_debug_ui/memory_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/layout/box.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

#include <utility>

namespace tui_debug_ui {

MemoryPanel::MemoryPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                         const std::string& title)
    : title_style_(theme.title_scopes) {
    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::Plain, &theme);
    list_->set_on_activate([this](int index) {
        if (on_activate_ != nullptr) {
            on_activate_(index);
        }
    });
    list_->set_on_inline_edit_submit([this](const std::string& value) {
        if (on_submit_ != nullptr && inline_edit_row_ >= 0) {
            on_submit_(inline_edit_row_, value);
        }
        inline_edit_row_ = -1;
        inline_edit_value_.clear();
    });
    list_->set_on_inline_edit_cancel([this]() {
        inline_edit_row_ = -1;
        inline_edit_value_.clear();
        if (on_inline_edit_cancel_ != nullptr) {
            on_inline_edit_cancel_();
        }
    });

    auto header = std::make_unique<tuinator::HBox>(tuinator::BoxOptions{.gap = 1, .padding = 0});
    auto address_input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.min_width = 10, .placeholder = "Addr 0x… or &g_buffer"}, theme.label,
        theme.frame_current);
    address_input_ = address_input.get();
    address_input_->set_flex(1);
    address_input_->set_on_submit([this](const std::string& value) {
        if (on_address_submit_ != nullptr) {
            on_address_submit_(value);
        }
    });

    auto search_input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.min_width = 8, .placeholder = "Find text/hex"}, theme.label, theme.frame_current);
    search_input_ = search_input.get();
    search_input_->set_flex(1);
    search_input_->set_on_submit([this](const std::string& value) {
        if (on_search_submit_ != nullptr) {
            on_search_submit_(value);
        }
    });

    header->add_child(std::move(address_input));
    header->add_child(std::move(search_input));

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), title_style_, theme.panel_background,
                                               std::move(scroll_options), true, true, std::move(header));
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> MemoryPanel::release_widget() { return pane_->release_widget(); }

void MemoryPanel::set_lines(std::vector<std::string> lines) {
    if (list_ != nullptr) {
        list_->assign_items(std::move(lines));
    }
}

void MemoryPanel::set_title(std::string title) { pane_->set_title(std::move(title)); }

void MemoryPanel::set_writable(bool writable) { writable_ = writable; }

void MemoryPanel::set_address_value(std::string value) {
    if (address_input_ != nullptr) {
        address_input_->set_value(std::move(value));
    }
}

std::string MemoryPanel::address_value() const {
    if (address_input_ == nullptr) {
        return {};
    }
    return address_input_->value();
}

void MemoryPanel::set_search_value(std::string value) {
    if (search_input_ != nullptr) {
        search_input_->set_value(std::move(value));
    }
}

std::string MemoryPanel::search_value() const {
    if (search_input_ == nullptr) {
        return {};
    }
    return search_input_->value();
}

void MemoryPanel::focus_address_input() {
    if (address_input_ != nullptr) {
        if (search_input_ != nullptr) {
            search_input_->set_focused(false);
        }
        address_input_->set_focused(true);
        if (list_ != nullptr) {
            list_->set_focused(false);
        }
    }
}

void MemoryPanel::focus_search_input() {
    if (search_input_ != nullptr) {
        if (address_input_ != nullptr) {
            address_input_->set_focused(false);
        }
        search_input_->set_focused(true);
        if (list_ != nullptr) {
            list_->set_focused(false);
        }
    }
}

void MemoryPanel::blur_toolbar_inputs() {
    if (address_input_ != nullptr) {
        address_input_->set_focused(false);
    }
    if (search_input_ != nullptr) {
        search_input_->set_focused(false);
    }
    if (list_ != nullptr) {
        list_->set_focused(true);
    }
}

bool MemoryPanel::is_address_input_focused() const {
    return address_input_ != nullptr && address_input_->is_focused();
}

bool MemoryPanel::is_search_input_focused() const {
    return search_input_ != nullptr && search_input_->is_focused();
}

bool MemoryPanel::is_toolbar_input_focused() const {
    return is_address_input_focused() || is_search_input_focused();
}

void MemoryPanel::set_on_refresh(std::function<void()> callback) {
    if (callback == nullptr) {
        return;
    }
    pane_->set_title_action("\u21bb", title_style_, std::move(callback));
}

void MemoryPanel::set_on_address_submit(std::function<void(const std::string&)> callback) {
    on_address_submit_ = std::move(callback);
}

void MemoryPanel::set_on_search_submit(std::function<void(const std::string&)> callback) {
    on_search_submit_ = std::move(callback);
}

void MemoryPanel::set_on_activate(std::function<void(int row)> callback) { on_activate_ = std::move(callback); }

void MemoryPanel::set_on_submit(std::function<void(int row, const std::string& hex)> callback) {
    on_submit_ = std::move(callback);
}

void MemoryPanel::set_on_inline_edit_cancel(std::function<void()> callback) {
    on_inline_edit_cancel_ = std::move(callback);
}

void MemoryPanel::begin_row_edit(int row_index, std::string hex_value) {
    if (!writable_ || list_ == nullptr || row_index < 0 || row_index >= static_cast<int>(list_->items().size())) {
        return;
    }
    blur_toolbar_inputs();
    inline_edit_row_ = row_index;
    inline_edit_value_ = std::move(hex_value);
    sync_inline_edit_to_list();
}

void MemoryPanel::sync_inline_edit_to_list() {
    if (list_ == nullptr || inline_edit_row_ < 0) {
        if (list_ != nullptr) {
            list_->clear_inline_row_edit();
        }
        return;
    }

    list_->set_inline_row_edit(inline_edit_row_, "  hex ", inline_edit_value_);
    list_->set_selected_index(inline_edit_row_);
    list_->set_focused(true);

    if (tuinator::ScrollView* scroll = scroll_view()) {
        scroll->scroll_to(0, std::max(0, inline_edit_row_ - 1));
    }
}

void MemoryPanel::clear_inline_edit() {
    inline_edit_row_ = -1;
    inline_edit_value_.clear();
    if (list_ != nullptr) {
        list_->clear_inline_row_edit();
        list_->mark_dirty();
    }
}

bool MemoryPanel::has_inline_edit() const {
    return inline_edit_row_ >= 0 || (list_ != nullptr && list_->has_inline_row_edit());
}

int MemoryPanel::selected_row() const {
    if (list_ == nullptr) {
        return -1;
    }
    return list_->selected_index();
}

void MemoryPanel::set_selected_row(int row_index) {
    if (list_ == nullptr || row_index < 0) {
        return;
    }
    list_->set_selected_index(row_index);
    if (tuinator::ScrollView* scroll = scroll_view()) {
        scroll->scroll_to(0, std::max(0, row_index - 1));
    }
}

bool MemoryPanel::handle_inline_edit_key(const tuinator::Event& event) {
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

bool MemoryPanel::handle_toolbar_input_key(const tuinator::Event& event) {
    if (!is_toolbar_input_focused()) {
        return false;
    }
    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (key->key == tuinator::Key::Tab) {
            if (is_address_input_focused()) {
                focus_search_input();
            } else {
                focus_address_input();
            }
            return true;
        }
    }
    tuinator::TextInput* input = is_address_input_focused() ? address_input_ : search_input_;
    if (input == nullptr) {
        return false;
    }
    return input->handle_event(event);
}

tuinator::Widget* MemoryPanel::list_widget() const { return list_; }

tuinator::Widget* MemoryPanel::address_input_widget() const { return address_input_; }

tuinator::Widget* MemoryPanel::search_input_widget() const { return search_input_; }

tuinator::ScrollView* MemoryPanel::scroll_view() const { return pane_->scroll_view(); }

}  // namespace tui_debug_ui
