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
    {kPlay, "play_pause", "Continue (F5)", false, false},
    {kStepInto, "step_into", "Step Into (F11)", true, false},
    {kStepOver, "step_over", "Step Over (F10)", true, false},
    {kStepOut, "step_out", "Step Out (Shift+F11)", true, false},
    {kStepBack, "step_back", "Step Back", true, false},
    {kStepBackInto, "step_back_into", "Step Back Into", true, false},
    {kContinueBack, "reverse_continue", "Reverse Continue", true, false},
    {kRestart, "restart", "Restart", false, false},
    {kTerminate, "terminate", "Stop", false, true},
    {kDisconnect, "disconnect", "Disconnect", false, true},
};

ControlsBar::ControlsBar(DapUiTheme theme) : theme_(theme) {}

void ControlsBar::set_on_action(ActionCallback callback) { on_action_ = std::move(callback); }

void ControlsBar::set_on_hover_changed(std::function<void()> callback) {
    on_hover_changed_ = std::move(callback);
}

void ControlsBar::set_hover_index(int index) {
    if (hover_index_ == index) {
        return;
    }

    hover_index_ = index;
    tooltip_visible_ = false;
    hover_started_at_ = index >= 0 ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    mark_dirty();
    if (on_hover_changed_ != nullptr) {
        on_hover_changed_();
    }
}

void ControlsBar::clear_hover() {
    set_hover_index(-1);
}

void ControlsBar::tick_hover() {
    if (hover_index_ < 0 || tooltip_visible_ || hover_started_at_ == std::chrono::steady_clock::time_point{}) {
        return;
    }

    const auto elapsed = std::chrono::steady_clock::now() - hover_started_at_;
    if (elapsed < kTooltipDelay) {
        return;
    }

    tooltip_visible_ = true;
    mark_dirty();
    if (on_hover_changed_ != nullptr) {
        on_hover_changed_();
    }
}

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

std::string ControlsBar::tooltip_for_button(int index) const {
    if (index < 0 || index >= kButtonCount) {
        return {};
    }

    std::string text = kButtons[index].tooltip;
    if (index == 0 && session_active_ && !stopped_) {
        text = "Pause";
    }

    const char number_hint = index < 9 ? static_cast<char>('1' + index) : '0';
    text += " · ";
    text += number_hint;
    return text;
}

tuinator::Rect ControlsBar::tooltip_bounds(tuinator::Rect clip_bounds) const {
    if (!tooltip_visible_ || hover_index_ < 0 || clip_bounds.width <= 0 || clip_bounds.height <= 0) {
        return {};
    }

    const std::string text = tooltip_for_button(hover_index_);
    if (text.empty()) {
        return {};
    }

    constexpr int kPaddingX = 2;
    constexpr int kHeight = 3;
    const int width = tuinator::text_display_width(text) + kPaddingX * 2 + 2;
    const int total_width = kButtonCount * kButtonWidth;
    const int start_x = bounds_.x + std::max(0, (bounds_.width - total_width) / 2);
    const int button_x = start_x + hover_index_ * kButtonWidth;
    const int center_x = button_x + kButtonWidth / 2;

    int x = center_x - width / 2;
    int y = bounds_.y + bounds_.height;
    if (y + kHeight > clip_bounds.y + clip_bounds.height) {
        y = bounds_.y - kHeight;
    }
    if (x < clip_bounds.x) {
        x = clip_bounds.x;
    }
    if (x + width > clip_bounds.x + clip_bounds.width) {
        x = std::max(clip_bounds.x, clip_bounds.x + clip_bounds.width - width);
    }
    if (y < clip_bounds.y) {
        y = clip_bounds.y;
    }
    return {x, y, width, kHeight};
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

void ControlsBar::paint_tooltip(tuinator::PaintContext& ctx, tuinator::Rect clip_bounds) const {
    const tuinator::Rect tooltip = tooltip_bounds(clip_bounds);
    if (tooltip.width <= 0 || tooltip.height <= 0) {
        return;
    }

    const std::string text = tooltip_for_button(hover_index_);
    tuinator::Canvas& canvas = ctx.canvas;
    const tuinator::Point origin{tooltip.x, tooltip.y};
    const tuinator::Size size{tooltip.width, tooltip.height};
    const tuinator::Style text_style =
        is_button_available(hover_index_) ? theme_.label : theme_.control_disabled;

    canvas.fill_rect({origin, size}, ' ', theme_.panel_background);
    canvas.draw_box({origin, size}, theme_.border_focused, ctx.glyphs());
    canvas.draw_text({origin.x + 2, origin.y + 1}, text, text_style);
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
            set_hover_index(index);
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
