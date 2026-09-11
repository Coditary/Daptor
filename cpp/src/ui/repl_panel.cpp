#include "tui_debug_ui/repl_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/layout/box.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

#include <utility>

namespace tui_debug_ui {

namespace {

bool is_pointer_pick(const tuinator::MouseEvent& mouse) {
    return mouse.action == tuinator::MouseAction::Click || mouse.action == tuinator::MouseAction::Release ||
           mouse.action == tuinator::MouseAction::Press;
}

/// Clicking anywhere in the REPL panel focuses the expression input for immediate typing.
class ReplActivateShell : public tuinator::Widget {
  public:
    ReplActivateShell(std::unique_ptr<tuinator::Widget> child, ReplPanel* panel, NavigableListView* history,
                      tuinator::TextInput* input, ReplPanel::ActivateCallback on_activate)
        : child_(std::move(child)),
          panel_(panel),
          history_(history),
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
            if (panel_ != nullptr && panel_->input_active() && input_ != nullptr) {
                input_->set_focused(true);
                if (history_ != nullptr) {
                    history_->set_focused(false);
                }
                input_->handle_event(event);
                return true;
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
    tuinator::TextInput* input_ = nullptr;
    ReplPanel::ActivateCallback on_activate_;
};

}  // namespace

ReplPanel::ReplPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options) {
    auto root = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});

    auto history = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, false);
    history_ = history.get();
    history_->set_paint_mode(ListPaintMode::Plain, &theme);
    history_->set_flex(1);

    auto input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.placeholder = "> expression"}, theme.label, theme.frame_current);
    input->set_flex(0);
    input_ = input.get();

    root->add_child(std::move(history));
    root->add_child(std::move(input));

    pane_ = std::make_unique<TitledScrollPane>("REPL", std::move(root), theme.title_repl, theme.panel_background,
                                                std::move(scroll_options), false);
}

std::unique_ptr<tuinator::Widget> ReplPanel::release_widget() {
    auto shell = std::make_unique<ReplActivateShell>(pane_->release_widget(), this, history_, input_, on_activate_);
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

void ReplPanel::set_input_active(bool active) { input_active_ = active; }

void ReplPanel::set_input_value(std::string value) {
    if (input_ != nullptr) {
        input_->set_value(std::move(value));
    }
}

std::string ReplPanel::input_value() const {
    return input_ != nullptr ? input_->value() : std::string{};
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

tuinator::Widget* ReplPanel::history_widget() const { return history_; }

tuinator::TextInput* ReplPanel::input_widget() const { return input_; }

tuinator::ScrollView* ReplPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
