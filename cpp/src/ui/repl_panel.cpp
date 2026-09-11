#include "tui_debug_ui/repl_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/layout/box.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

#include <utility>

namespace tui_debug_ui {

namespace {

bool is_pointer_pick(const tuinator::MouseEvent& mouse) {
    return mouse.action == tuinator::MouseAction::Click || mouse.action == tuinator::MouseAction::Release ||
           mouse.action == tuinator::MouseAction::Press;
}

bool is_cursor_move_key(const tuinator::KeyPress& key) {
    switch (key.key) {
    case tuinator::Key::Left:
    case tuinator::Key::Right:
    case tuinator::Key::Home:
    case tuinator::Key::End:
        return true;
    default:
        return false;
    }
}

/// Wraps the expression input and paints inline ghost completion text.
class ReplInputWithGhost : public tuinator::Widget {
  public:
    ReplInputWithGhost(std::unique_ptr<tuinator::TextInput> input, ReplPanel* panel, tuinator::Style ghost_style)
        : input_(std::move(input)), panel_(panel), ghost_style_(ghost_style) {
        input_->set_flex(0);
    }

    tuinator::TextInput* input() const { return input_.get(); }

    tuinator::Size preferred_size() const override { return input_->preferred_size(); }

    void layout(tuinator::Rect bounds) override {
        bounds_ = bounds;
        input_->layout(bounds);
    }

    void paint(tuinator::PaintContext& ctx) const override {
        input_->paint(ctx);
        if (panel_ == nullptr || !panel_->has_ghost_suggestion() || !input_->is_focused()) {
            return;
        }

        const ReplGhostSuggestion& ghost = panel_->ghost_suggestion().value();
        if (ghost.suffix.empty() || bounds_.width <= 2) {
            return;
        }

        const std::string& value = input_->value();
        const std::size_t cursor = input_->cursor_position();
        const int scroll_x = input_->horizontal_scroll();
        const int inner_width = std::max(0, bounds_.width - 2);
        const int col = 1 + tuinator::text_display_width(value.substr(0, cursor)) - scroll_x;
        if (col < 1 || col >= 1 + inner_width) {
            return;
        }

        const std::size_t bytes =
            tuinator::text_byte_length_for_width(ghost.suffix, std::max(0, inner_width - (col - 1)));
        if (bytes == 0) {
            return;
        }

        ctx.canvas.draw_text({col, 0}, ghost.suffix.substr(0, bytes), ghost_style_);
    }

    bool handle_event(const tuinator::Event& event) override {
        if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
            if (key->key == tuinator::Key::Tab) {
                if (panel_ != nullptr) {
                    return panel_->handle_tab_key();
                }
                return false;
            }
            if (panel_ != nullptr && is_cursor_move_key(*key)) {
                panel_->cancel_completion_preview();
            }
        }

        const bool handled = input_->handle_event(event);
        if (handled && panel_ != nullptr) {
            if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
                if (key->key != tuinator::Key::Tab) {
                    panel_->notify_input_edited();
                }
            }
        }
        return handled;
    }

    bool is_focusable() const override { return input_->is_focusable(); }

    tuinator::Widget* hit_test(tuinator::Point point) override { return input_->hit_test(point); }
    tuinator::Widget* hit_test_focusable(tuinator::Point point) override {
        return input_->hit_test_focusable(point);
    }

    void collect_focusable(std::vector<tuinator::Widget*>& out) override { input_->collect_focusable(out); }

    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override {
        visitor(input_.get());
    }

  private:
    std::unique_ptr<tuinator::TextInput> input_;
    ReplPanel* panel_ = nullptr;
    tuinator::Style ghost_style_;
};

/// Clicking anywhere in the REPL panel focuses the expression input for immediate typing.
class ReplActivateShell : public tuinator::Widget {
  public:
    ReplActivateShell(std::unique_ptr<tuinator::Widget> child, ReplPanel* panel, NavigableListView* history,
                      ReplInputWithGhost* input_shell, tuinator::TextInput* input,
                      ReplPanel::ActivateCallback on_activate)
        : child_(std::move(child)),
          panel_(panel),
          history_(history),
          input_shell_(input_shell),
          input_(input),
          on_activate_(std::move(on_activate)) {
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

    bool captures_keyboard() const override { return panel_ != nullptr && panel_->input_active(); }

    bool handle_event(const tuinator::Event& event) override {
        if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
            if (panel_ != nullptr && panel_->input_active() && input_shell_ != nullptr) {
                input_->set_focused(true);
                if (history_ != nullptr) {
                    history_->set_focused(false);
                }
                return input_shell_->handle_event(event);
            }
        }

        if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
            if (is_pointer_pick(*mouse) && bounds_.contains(mouse->position)) {
                activate_input();
                if (child_ != nullptr) {
                    child_->handle_event(event);
                }
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

    tuinator::Widget* hit_test_focusable(tuinator::Point point) override {
        if (!bounds_.contains(point)) {
            return nullptr;
        }
        return this;
    }

    void collect_focusable(std::vector<tuinator::Widget*>& out) override { out.push_back(this); }

    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override {
        if (child_ != nullptr) {
            visitor(child_.get());
        }
    }

    bool pointer_active() const override { return child_ != nullptr && child_->pointer_active(); }

  private:
    void activate_input() {
        if (panel_ != nullptr) {
            panel_->set_input_active(true);
        }
        if (history_ != nullptr) {
            history_->set_focused(false);
        }
        if (input_ != nullptr) {
            input_->set_focused(true);
        }
        if (on_activate_ != nullptr) {
            on_activate_();
        }
    }

    std::unique_ptr<tuinator::Widget> child_;
    ReplPanel* panel_ = nullptr;
    NavigableListView* history_ = nullptr;
    ReplInputWithGhost* input_shell_ = nullptr;
    tuinator::TextInput* input_ = nullptr;
    ReplPanel::ActivateCallback on_activate_;
};

}  // namespace

ReplPanel::ReplPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options) {
    ghost_style_ = theme.repl_ghost;
    auto root = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});

    auto history = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, false);
    history_ = history.get();
    history_->set_paint_mode(ListPaintMode::Plain, &theme);
    history_->set_flex(1);

    auto input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.placeholder = "> expression"}, theme.label, theme.frame_current);
    input_ = input.get();
    auto input_shell = std::make_unique<ReplInputWithGhost>(std::move(input), this, ghost_style_);
    input_shell_ = input_shell.get();

    root->add_child(std::move(history));
    root->add_child(std::move(input_shell));

    pane_ = std::make_unique<TitledScrollPane>("REPL", std::move(root), theme.title_repl, theme.panel_background,
                                                std::move(scroll_options), false);
}

std::unique_ptr<tuinator::Widget> ReplPanel::release_widget() {
    auto shell = std::make_unique<ReplActivateShell>(
        pane_->release_widget(), this, history_, static_cast<ReplInputWithGhost*>(input_shell_), input_, on_activate_);
    shell_ = shell.get();
    return shell;
}

bool ReplPanel::contains_point(tuinator::Point point) const {
    return shell_ != nullptr && shell_->bounds().contains(point);
}

void ReplPanel::set_history_lines(std::vector<std::string> lines) {
    if (history_ != nullptr) {
        history_->assign_items(std::move(lines));
    }
}

void ReplPanel::set_on_submit(SubmitCallback callback) {
    on_submit_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_submit([this](const std::string& value) {
            clear_ghost_suggestion();
            if (on_submit_ != nullptr) {
                on_submit_(value);
            }
        });
    }
}

void ReplPanel::set_on_change(ChangeCallback callback) {
    on_change_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_change([this](const std::string& value) {
            if (on_change_ != nullptr) {
                on_change_(value);
            }
        });
    }
}

void ReplPanel::set_on_activate(ActivateCallback callback) { on_activate_ = std::move(callback); }

void ReplPanel::set_on_completion_request(CompletionRequestCallback callback) {
    on_completion_request_ = std::move(callback);
}

void ReplPanel::set_on_completion_cycle(CompletionCycleCallback callback) {
    on_completion_cycle_ = std::move(callback);
}

void ReplPanel::set_on_completion_cancel(CompletionCancelCallback callback) {
    on_completion_cancel_ = std::move(callback);
}

void ReplPanel::set_completion_menu_active(bool active) { completion_menu_active_ = active; }

bool ReplPanel::has_completion_menu() const { return completion_menu_active_; }

void ReplPanel::cancel_completion_preview() {
    completion_menu_active_ = false;
    clear_ghost_suggestion();
    if (on_completion_cancel_ != nullptr) {
        on_completion_cancel_();
    }
}

void ReplPanel::set_input_active(bool active) { input_active_ = active; }

void ReplPanel::set_input_value(std::string value) {
    if (input_ != nullptr) {
        input_->set_value(std::move(value));
    }
}

std::string ReplPanel::input_value() const {
    return input_ != nullptr ? input_->value() : std::string{};
}

std::size_t ReplPanel::input_cursor_column() const {
    return input_ != nullptr ? input_->cursor_position() : 0;
}

void ReplPanel::focus_input() {
    set_input_active(true);
    if (input_ != nullptr) {
        input_->set_focused(true);
        if (history_ != nullptr) {
            history_->set_focused(false);
        }
    }
}

void ReplPanel::set_ghost_suggestion(ReplGhostSuggestion suggestion) {
    if (suggestion.suffix.empty()) {
        ghost_.reset();
    } else {
        ghost_ = std::move(suggestion);
    }
    if (input_shell_ != nullptr) {
        input_shell_->mark_dirty();
    }
}

void ReplPanel::clear_ghost_suggestion() {
    if (!ghost_.has_value()) {
        return;
    }
    ghost_.reset();
    if (input_shell_ != nullptr) {
        input_shell_->mark_dirty();
    }
}

bool ReplPanel::has_ghost_suggestion() const { return ghost_.has_value() && !ghost_->suffix.empty(); }

bool ReplPanel::accept_ghost_suggestion() {
    if (!has_ghost_suggestion() || input_ == nullptr) {
        return false;
    }

    const ReplGhostSuggestion& ghost = *ghost_;
    std::string value = input_->value();
    const std::size_t start = std::min(ghost.replace_start, value.size());
    const std::size_t end = std::min(start + ghost.replace_length, value.size());
    std::string new_value = value.substr(0, start) + ghost.label + value.substr(end);
    input_->set_value(std::move(new_value));
    completion_menu_active_ = false;
    clear_ghost_suggestion();
    if (on_change_ != nullptr) {
        on_change_(input_->value());
    }
    return true;
}

bool ReplPanel::handle_tab_key() {
    if (has_ghost_suggestion()) {
        return accept_ghost_suggestion();
    }
    if (on_completion_request_ != nullptr) {
        on_completion_request_();
        return true;
    }
    return false;
}

bool ReplPanel::handle_completion_cycle(int delta) {
    if (on_completion_cycle_ == nullptr) {
        return false;
    }
    return on_completion_cycle_(delta);
}

void ReplPanel::notify_input_edited() { clear_ghost_suggestion(); }

bool ReplPanel::handle_input_event(const tuinator::Event& event) {
    if (input_shell_ == nullptr) {
        return false;
    }
    return static_cast<ReplInputWithGhost*>(input_shell_)->handle_event(event);
}

tuinator::Widget* ReplPanel::history_widget() const { return history_; }

tuinator::TextInput* ReplPanel::input_widget() const { return input_; }

tuinator::ScrollView* ReplPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
