#pragma once

#include "tui_debug_ui/dap_ui_theme.hpp"

#include <tuinator/widgets/widget.hpp>

#include <functional>
#include <string>

namespace tui_debug_ui {

/// Centered debug controls row (nvim-dap-ui winbar style).
class ControlsBar : public tuinator::Widget {
  public:
    using ActionCallback = std::function<void(const std::string& op)>;

    explicit ControlsBar(DapUiTheme theme);

    void set_on_action(ActionCallback callback);
    void set_session_active(bool active);
    void set_stopped(bool stopped);
    void set_session_ended(bool ended);
    bool session_active() const { return session_active_; }
    bool stopped() const { return stopped_; }
    bool session_ended() const { return session_ended_; }

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    bool captures_pointer() const override { return true; }
    tuinator::Widget* hit_test(tuinator::Point point) override;

  private:
    struct ControlButton {
        const char* icon = "";
        const char* op = "";
        bool needs_stopped = false;
        bool always_available = false;
    };

    int button_index_at(tuinator::Point position) const;
    bool is_button_available(int index) const;
    tuinator::Style button_style(int index) const;

    DapUiTheme theme_;
    ActionCallback on_action_;
    bool session_active_ = false;
    bool stopped_ = false;
    bool session_ended_ = false;
    int hover_index_ = -1;
    static constexpr int kButtonCount = 8;
    static const ControlButton kButtons[kButtonCount];
};

}  // namespace tui_debug_ui
