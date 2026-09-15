#pragma once

#include "tui_debug_ui/dap_ui_theme.hpp"

#include <tuinator/widgets/widget.hpp>

#include <chrono>
#include <functional>
#include <string>

namespace tui_debug_ui {

/// Centered debug controls row (nvim-dap-ui winbar style).
class ControlsBar : public tuinator::Widget {
  public:
    using ActionCallback = std::function<void(const std::string& op)>;

    explicit ControlsBar(DapUiTheme theme);

    void set_on_action(ActionCallback callback);
    void set_on_hover_changed(std::function<void()> callback);
    void set_session_active(bool active);
    void set_stopped(bool stopped);
    void set_session_ended(bool ended);
    void set_supports_step_back(bool supported);
    bool session_active() const { return session_active_; }
    bool stopped() const { return stopped_; }
    bool session_ended() const { return session_ended_; }

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    bool captures_pointer() const override { return true; }
    tuinator::Widget* hit_test(tuinator::Point point) override;

    void clear_hover();
    void tick_hover();
    void paint_tooltip(tuinator::PaintContext& ctx, tuinator::Rect clip_bounds) const;

  private:
    void set_hover_index(int index);
    struct ControlButton {
        const char* icon = "";
        const char* op = "";
        const char* tooltip = "";
        bool needs_stopped = false;
        bool always_available = false;
    };

    int button_index_at(tuinator::Point position) const;
    bool is_button_available(int index) const;
    tuinator::Style button_style(int index) const;
    std::string tooltip_for_button(int index) const;
    tuinator::Rect tooltip_bounds(tuinator::Rect clip_bounds) const;

    DapUiTheme theme_;
    ActionCallback on_action_;
    std::function<void()> on_hover_changed_;
    bool session_active_ = false;
    bool stopped_ = false;
    bool session_ended_ = false;
    bool supports_step_back_ = false;
    int hover_index_ = -1;
    bool tooltip_visible_ = false;
    std::chrono::steady_clock::time_point hover_started_at_{};
    static constexpr int kButtonCount = 10;
    static constexpr std::chrono::milliseconds kTooltipDelay{1500};
    static const ControlButton kButtons[kButtonCount];
};

}  // namespace tui_debug_ui
