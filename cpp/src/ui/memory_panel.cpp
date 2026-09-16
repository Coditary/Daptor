#include "tui_debug_ui/memory_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/layout/box.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/widgets/controls/text_input.hpp>
#include <tuinator/widgets/widget.hpp>

#include <algorithm>
#include <utility>

namespace tui_debug_ui {

namespace {

bool is_pointer_pick(const tuinator::MouseEvent& mouse) {
    return mouse.action == tuinator::MouseAction::Click || mouse.action == tuinator::MouseAction::Release ||
           mouse.action == tuinator::MouseAction::Press;
}

class FixedWidthSpacer : public tuinator::Widget {
  public:
    explicit FixedWidthSpacer(int width) : width_(std::max(0, width)) {}

    tuinator::Size preferred_size() const override { return {width_, 1}; }

    void layout(tuinator::Rect bounds) override { bounds_ = bounds; }

    void paint(tuinator::PaintContext&) const override {}

  private:
    int width_ = 0;
};

class MatchCounterLabel : public tuinator::Widget {
  public:
    explicit MatchCounterLabel(tuinator::Style style) : style_(std::move(style)) {}

    void set_text(std::string text) {
        if (text_ == text) {
            return;
        }
        text_ = std::move(text);
        mark_layout_dirty();
        mark_dirty();
    }

    tuinator::Size preferred_size() const override { return {kWidth, 1}; }

    void layout(tuinator::Rect bounds) override { bounds_ = bounds; }

    void paint(tuinator::PaintContext& ctx) const override {
        if (bounds_.width <= 0 || bounds_.height <= 0 || text_.empty()) {
            return;
        }
        ctx.canvas.draw_text({0, 0}, text_, style_);
    }

  private:
    static constexpr int kWidth = 7;
    std::string text_;
    tuinator::Style style_;
};

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
            if (panel_->is_toolbar_active() && panel_->handle_toolbar_input_key(event)) {
                return true;
            }
            if (panel_->handle_list_navigation_key(event)) {
                return true;
            }
            if (panel_->handle_list_activate_key(event)) {
                return true;
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
        clear_inline_edit();
        if (on_inline_edit_cancel_ != nullptr) {
            on_inline_edit_cancel_();
        }
    });
    list_->set_on_select([this](int row, const std::string& /*line*/) {
        reveal_row(row);
        if (on_row_selected_ != nullptr) {
            on_row_selected_(row);
        }
    });

    auto header = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 1, .padding = 0});

    auto address_row = std::make_unique<tuinator::HBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});
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
    address_row->add_child(std::move(address_input));

    auto search_row = std::make_unique<tuinator::HBox>(tuinator::BoxOptions{.gap = 1, .padding = 0});
    auto list_prefix_spacer = std::make_unique<FixedWidthSpacer>(2);
    list_prefix_spacer->set_flex(0);
    auto search_input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.min_width = 36, .placeholder = "Find text or hex bytes…"}, theme.label,
        theme.frame_current);
    search_input_ = search_input.get();
    search_input_->set_flex(1);
    search_input_->set_on_submit([this](const std::string& value) {
        if (on_search_submit_ != nullptr) {
            on_search_submit_(value);
        }
    });

    auto search_match_label = std::make_unique<MatchCounterLabel>(theme.selection);
    search_match_label_ = search_match_label.get();
    search_match_label_->set_flex(0);

    search_row->add_child(std::move(list_prefix_spacer));
    search_row->add_child(std::move(search_input));
    search_row->add_child(std::move(search_match_label));

    header->add_child(std::move(address_row));
    header->add_child(std::move(search_row));
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
    if (list_ == nullptr) {
        return;
    }
    const bool items_changed = list_->items() != lines;
    if (items_changed) {
        list_->assign_items(std::move(lines), true);
    }
    if (items_changed) {
        list_->mark_dirty();
        if (tuinator::ScrollView* scroll = scroll_view()) {
            scroll->mark_dirty();
        }
        if (pane_ != nullptr) {
            pane_->refresh_scroll_content();
        }
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
        address_cancel_value_ = address_input_->value();
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
        search_cancel_value_ = search_input_->value();
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

void MemoryPanel::cancel_toolbar_inputs() {
    if (!is_toolbar_active()) {
        return;
    }

    if (active_toolbar_ == MemoryToolbarFocus::Address && address_input_ != nullptr) {
        address_input_->set_value(address_cancel_value_);
    } else if (active_toolbar_ == MemoryToolbarFocus::Search && search_input_ != nullptr) {
        search_input_->set_value(search_cancel_value_);
    }

    if (on_toolbar_cancel_ != nullptr) {
        on_toolbar_cancel_();
    }

    blur_toolbar_inputs();
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

bool MemoryPanel::is_toolbar_text_input_focused() const {
    return is_address_input_focused() || is_search_input_focused();
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

void MemoryPanel::set_on_search_change(std::function<void(const std::string&)> callback) {
    on_search_change_ = std::move(callback);
    if (search_input_ != nullptr) {
        search_input_->set_on_change([this](const std::string& value) {
            if (on_search_change_ != nullptr) {
                on_search_change_(value);
            }
        });
    }
}

void MemoryPanel::set_search_highlight(std::size_t byte_offset, std::size_t length) {
    if (list_ == nullptr) {
        return;
    }
    NavigableListView::MemorySearchHighlight highlight;
    highlight.start_byte = byte_offset;
    highlight.length = length;
    list_->set_memory_search_highlight(highlight);
}

void MemoryPanel::clear_search_highlight() {
    if (list_ != nullptr) {
        list_->clear_memory_search_highlight();
    }
}

void MemoryPanel::set_search_match_counter(int current, int total) {
    if (search_match_label_ == nullptr) {
        return;
    }
    auto* label = dynamic_cast<MatchCounterLabel*>(search_match_label_);
    if (label == nullptr) {
        return;
    }
    if (current <= 0 || total <= 0) {
        label->set_text("");
    } else {
        label->set_text("(" + std::to_string(current) + "/" + std::to_string(total) + ")");
    }
    if (toolbar_row_ != nullptr) {
        toolbar_row_->mark_layout_dirty();
    }
    if (shell_ != nullptr) {
        shell_->mark_layout_dirty();
    }
}

void MemoryPanel::clear_search_match_counter() { set_search_match_counter(0, 0); }

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

void MemoryPanel::set_on_toolbar_cancel(std::function<void()> callback) {
    on_toolbar_cancel_ = std::move(callback);
}

void MemoryPanel::set_on_row_selected(std::function<void(int row)> callback) {
    on_row_selected_ = std::move(callback);
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
        if (search_bounds.height > 0 && point.y >= search_bounds.y &&
            point.y < search_bounds.y + search_bounds.height) {
            focus_search_input();
            return true;
        }
    }

    if (address_input_ != nullptr) {
        const tuinator::Rect address_bounds = address_input_->bounds();
        if (address_bounds.height > 0 && point.y >= address_bounds.y &&
            point.y < address_bounds.y + address_bounds.height) {
            focus_address_input();
            return true;
        }
    }

    return true;
}

bool MemoryPanel::handle_list_navigation_key(const tuinator::Event& event) {
    if (list_ == nullptr || is_toolbar_active() || has_inline_edit()) {
        return false;
    }

    const auto* key = std::get_if<tuinator::KeyPress>(&event);
    if (key == nullptr || key->ctrl || key->alt) {
        return false;
    }

    int delta = 0;
    if (key->key == tuinator::Key::Up || key->character == 'k') {
        delta = -1;
    } else if (key->key == tuinator::Key::Down || key->character == 'j') {
        delta = 1;
    } else {
        return false;
    }

    const int count = static_cast<int>(list_->items().size());
    if (count <= 0) {
        return true;
    }

    if (!list_->is_focused()) {
        list_->set_focused(true);
    }

    const int current = list_->selected_index();
    int next = current < 0 ? 0 : current + delta;
    next = std::clamp(next, 0, count - 1);
    if (next != current) {
        set_selected_row(next);
    }
    return true;
}

bool MemoryPanel::handle_list_activate_key(const tuinator::Event& event) {
    if (list_ == nullptr || is_toolbar_active() || has_inline_edit()) {
        return false;
    }

    const auto* key = std::get_if<tuinator::KeyPress>(&event);
    if (key == nullptr || key->key != tuinator::Key::Enter || key->ctrl || key->alt) {
        return false;
    }

    const int row = list_->selected_index();
    if (row < 0 || row >= static_cast<int>(list_->items().size())) {
        return true;
    }

    if (!list_->is_focused()) {
        list_->set_focused(true);
    }
    if (on_activate_ != nullptr) {
        on_activate_(row);
    }
    return true;
}

bool MemoryPanel::handle_toolbar_input_key(const tuinator::Event& event) {
    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (key->key == tuinator::Key::Escape &&
            (is_toolbar_active() || is_toolbar_text_input_focused())) {
            if (!is_toolbar_active()) {
                if (is_search_input_focused()) {
                    focus_search_input();
                } else {
                    focus_address_input();
                }
            }
            cancel_toolbar_inputs();
            return true;
        }
    }
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
