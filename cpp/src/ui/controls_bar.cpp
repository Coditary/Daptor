#include "tui_debug_ui/controls_bar.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>
#include <cstring>
#include <variant>

namespace tui_debug_ui {

namespace {

constexpr const char* kPlay = "\u{ead3}";
constexpr const char* kPause = "\u{ead1}";
constexpr const char* kStepInto = "\u{ead4}";
constexpr const char* kStepOver = "\u{ead6}";
constexpr const char* kStepOut = "\u{ead5}";
constexpr const char* kStepBack = "\u{eb8f}";
constexpr const char* kStepBackInto = "\u{2196}";
constexpr const char* kContinueBack = "\u{25c1}";
constexpr const char* kRestart = "\u{eb37}";
constexpr const char* kTerminate = "\u{ead7}";
constexpr const char* kDisconnect = "\u{ead0}";

constexpr int kButtonWidth = 5;

}  // namespace

constexpr ControlsBar::ControlButton ControlsBar::kButtons[ControlsBar::kButtonCount] = {
    {kPlay, "play_pause", false, false},
    {kStepInto, "step_into", true, false},
    {kStepOver, "step_over", true, false},
    {kStepOut, "step_out", true, false},
    {kStepBack, "step_back", true, false},
    {kStepBackInto, "step_back_into", true, false},
    {kContinueBack, "reverse_continue", true, false},
    {kRestart, "restart", false, false},
    {kTerminate, "terminate", false, true},
    {kDisconnect, "disconnect", false, true},
};

ControlsBar::ControlsBar(DapUiTheme theme) : theme_(theme) {}

void ControlsBar::set_on_action(ActionCallback callback) { on_action_ = std::move(callback); }

void ControlsBar::set_session_active(bool active) {
    session_active_ = active;
    mark_dirty();
}

void ControlsBar::set_stopped(bool stopped) {
    stopped_ = stopped;
    mark_dirty();
}

void ControlsBar::set_session_ended(bool ended) {
    session_ended_ = ended;
    mark_dirty();
}

void ControlsBar::set_supports_step_back(bool supported) {
    if (supports_step_back_ == supported) {
        return;
    }
    supports_step_back_ = supported;
    mark_dirty();
}

tuinator::Size ControlsBar::preferred_size() const { return {50, 1}; }

void ControlsBar::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
}

tuinator::Widget* ControlsBar::hit_test(tuinator::Point point) {
    if (bounds_.contains(point)) {
        return this;
    }
    return nullptr;
}

int ControlsBar::button_index_at(tuinator::Point position) const {
    if (!bounds_.contains(position)) {
        return -1;
    }

    const int total_width = kButtonCount * kButtonWidth;
    const int start_x = bounds_.x + std::max(0, (bounds_.width - total_width) / 2);
    const int local_x = position.x - start_x;
    if (local_x < 0 || local_x >= total_width) {
        return -1;
    }
    return local_x / kButtonWidth;
}

bool ControlsBar::is_button_available(int index) const {
    if (index < 0 || index >= kButtonCount) {
        return false;
    }

    const ControlButton& button = kButtons[index];
    if (!session_active_) {
        return false;
    }
    if (session_ended_) {
        if (std::strcmp(button.op, "restart") == 0) {
            return true;
        }
        return button.always_available;
    }
    if (button.always_available) {
        return true;
    }
    if (std::strcmp(button.op, "play_pause") == 0) {
        return stopped_ || !stopped_;
    }
    if (std::strcmp(button.op, "restart") == 0) {
        return true;
    }
    if (button.needs_stopped) {
        if (std::strcmp(button.op, "step_back") == 0 || std::strcmp(button.op, "step_back_into") == 0 ||
            std::strcmp(button.op, "reverse_continue") == 0) {
            return stopped_ && supports_step_back_;
        }
        return stopped_;
    }
    return true;
}

tuinator::Style ControlsBar::button_style(int index) const {
    if (index < 0 || index >= kButtonCount) {
        return theme_.control_disabled;
    }

    if (!is_button_available(index)) {
        return theme_.control_disabled;
    }

    const ControlButton& button = kButtons[index];

    tuinator::Style style = theme_.control_step;
    if (index == 0) {
        style = theme_.control_play;
    } else if (index == 7) {
        style = theme_.control_restart;
    } else if (index == 8 || index == 9) {
        style = theme_.control_stop;
    }

    if (index == hover_index_) {
        style.reverse = true;
    }
    return style;
}

void ControlsBar::paint(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', theme_.panel_background);

    const int total_width = kButtonCount * kButtonWidth;
    int x = std::max(0, (bounds_.width - total_width) / 2);
    for (int index = 0; index < kButtonCount; ++index) {
        const char* icon = kButtons[index].icon;
        if (index == 0 && session_active_ && !stopped_) {
            icon = kPause;
        }
        canvas.draw_text({x, 0}, std::string(" ") + icon + " ", button_style(index));
        x += kButtonWidth;
    }
}

bool ControlsBar::handle_event(const tuinator::Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        const int index = button_index_at(mouse->position);
        if (mouse->action == tuinator::MouseAction::Move) {
            if (hover_index_ != index) {
                hover_index_ = index;
                mark_dirty();
            }
            return index >= 0;
        }

        if (mouse->action == tuinator::MouseAction::Click || mouse->action == tuinator::MouseAction::Release) {
            if (index >= 0 && is_button_available(index) && on_action_ != nullptr) {
                on_action_(kButtons[index].op);
                return true;
            }
        }

        if (mouse->action == tuinator::MouseAction::Press) {
            return index >= 0;
        }
    }

    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (key->character >= '1' && key->character <= '9') {
            const int index = key->character - '1';
            if (is_button_available(index) && on_action_ != nullptr) {
                on_action_(kButtons[index].op);
                return true;
            }
        }
        if (key->character == '0') {
            const int index = 9;
            if (is_button_available(index) && on_action_ != nullptr) {
                on_action_(kButtons[index].op);
                return true;
            }
        }
    }

    return false;
}

}  // namespace tui_debug_ui
