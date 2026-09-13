#include "tui_debug_ui/memory_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/layout/box.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/widgets/controls/text_input.hpp>
#include <tuinator/widgets/widget.hpp>

#include <utility>

namespace tui_debug_ui {

namespace {

bool is_pointer_pick(const tuinator::MouseEvent& mouse) {
    return mouse.action == tuinator::MouseAction::Click || mouse.action == tuinator::MouseAction::Release ||
           mouse.action == tuinator::MouseAction::Press;
}

class MemoryPanelShell : public tuinator::Widget {
  public:
    MemoryPanelShell(std::unique_ptr<tuinator::Widget> child, MemoryPanel* panel) : child_(std::move(child)), panel_(panel) {
        if (child_ != nullptr) {
            child_->set_flex(1);
        }
    }

    tuinator::Size preferred_size() const override {
        return child_ != nullptr ? child_->preferred_size() : tuinator::Size{};
    }

    void layout(tuinator::Rect bounds) override {
        bounds_ = bounds;
        if (child_ != nullptr) {
            child_->layout(bounds);
        }
    }

    void paint(tuinator::PaintContext& ctx) const override {
        if (child_ != nullptr) {
            child_->paint(ctx);
        }
    }

    bool captures_keyboard() const override { return panel_ != nullptr && panel_->is_toolbar_active(); }

    bool handle_event(const tuinator::Event& event) override {
        if (panel_ != nullptr) {
            if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
                if (is_pointer_pick(*mouse) && panel_->activate_toolbar_from_point(mouse->position)) {
                    return true;
                }
            }
        }

        return child_ != nullptr && child_->handle_event(event);
    }

    tuinator::Widget* hit_test(tuinator::Point point) override {
        if (!bounds_.contains(point)) {
            return nullptr;
        }
        if (child_ != nullptr) {
            if (tuinator::Widget* hit = child_->hit_test(point)) {
                return hit;
            }
        }
        return this;
    }

    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override {
        if (child_ != nullptr) {
            visitor(child_.get());
        }
    }

  private:
    std::unique_ptr<tuinator::Widget> child_;
    MemoryPanel* panel_ = nullptr;
};

}  // namespace

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
    toolbar_row_ = header.get();

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), title_style_, theme.panel_background,
                                               std::move(scroll_options), true, true, std::move(header));
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> MemoryPanel::release_widget() {
    auto shell = std::make_unique<MemoryPanelShell>(pane_->release_widget(), this);
    shell_ = shell.get();
    return shell;
}

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
    active_toolbar_ = MemoryToolbarFocus::Address;
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
    active_toolbar_ = MemoryToolbarFocus::Search;
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
    active_toolbar_ = MemoryToolbarFocus::None;
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
    return active_toolbar_ != MemoryToolbarFocus::None;
}

bool MemoryPanel::is_toolbar_active() const { return active_toolbar_ != MemoryToolbarFocus::None; }

bool MemoryPanel::is_search_toolbar_active() const { return active_toolbar_ == MemoryToolbarFocus::Search; }

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

void MemoryPanel::set_on_toolbar_interact(std::function<void()> callback) {
    on_toolbar_interact_ = std::move(callback);
}

bool MemoryPanel::contains_point(tuinator::Point point) const {
    return shell_ != nullptr && shell_->bounds().contains(point);
}

bool MemoryPanel::activate_toolbar_from_point(tuinator::Point point) {
    if (!focus_toolbar_at_point(point)) {
        return false;
    }
    if (on_toolbar_interact_ != nullptr) {
        on_toolbar_interact_();
    }
    return true;
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

void MemoryPanel::reveal_row(int row_index) {
    if (list_ == nullptr || row_index < 0) {
        return;
    }

    if (tuinator::ScrollView* scroll = scroll_view()) {
        int scroll_y = std::max(0, row_index - 1);
        if (scroll->bounds().height > 0) {
            scroll_y = std::max(0, row_index - scroll->bounds().height / 2);
        }
        scroll->scroll_to(0, scroll_y);
        scroll->mark_dirty();
    }
    if (pane_ != nullptr) {
        pane_->refresh_scroll_content();
    }
    list_->mark_dirty();
}

void MemoryPanel::set_selected_row(int row_index) {
    if (list_ == nullptr || row_index < 0) {
        return;
    }
    list_->set_selected_index(row_index);
    reveal_row(row_index);
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

bool MemoryPanel::focus_toolbar_at_point(tuinator::Point point) {
    if (toolbar_row_ == nullptr) {
        return false;
    }

    const tuinator::Rect toolbar_bounds = toolbar_row_->bounds();
    if (toolbar_bounds.width <= 0 || toolbar_bounds.height <= 0 || !toolbar_bounds.contains(point)) {
        return false;
    }

    if (address_input_ != nullptr) {
        const tuinator::Rect address_bounds = address_input_->bounds();
        if (address_bounds.width > 0 && address_bounds.contains(point)) {
            focus_address_input();
            return true;
        }
    }
    if (search_input_ != nullptr) {
        const tuinator::Rect search_bounds = search_input_->bounds();
        if (search_bounds.width > 0 && search_bounds.contains(point)) {
            focus_search_input();
            return true;
        }
    }

    const int mid = toolbar_bounds.x + toolbar_bounds.width / 2;
    if (point.x >= mid) {
        focus_search_input();
    } else {
        focus_address_input();
    }
    return true;
}

bool MemoryPanel::handle_toolbar_input_key(const tuinator::Event& event) {
    if (!is_toolbar_active()) {
        return false;
    }
    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (key->key == tuinator::Key::Tab) {
            if (active_toolbar_ == MemoryToolbarFocus::Address) {
                focus_search_input();
            } else {
                focus_address_input();
            }
            return true;
        }
    }
    tuinator::TextInput* input = active_toolbar_ == MemoryToolbarFocus::Search ? search_input_ : address_input_;
    if (input == nullptr) {
        return false;
    }
    if (!input->is_focused()) {
        input->set_focused(true);
    }
    return input->handle_event(event);
}

tuinator::Widget* MemoryPanel::list_widget() const { return list_; }

tuinator::Widget* MemoryPanel::address_input_widget() const { return address_input_; }

tuinator::Widget* MemoryPanel::search_input_widget() const { return search_input_; }

tuinator::ScrollView* MemoryPanel::scroll_view() const { return pane_->scroll_view(); }

}  // namespace tui_debug_ui
