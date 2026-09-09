#pragma once

#include <tuinator/render/style.hpp>
#include <tuinator/render/theme.hpp>

namespace tui_debug_ui {

/// Color palette aligned with the Ratatui nvim-dap-ui port (`src/ui/theme.rs`).
struct DapUiTheme {
    static constexpr tuinator::Rgb kBackground{30, 30, 36};

    tuinator::Style border_normal{tuinator::style_fg_bg(tuinator::Rgb{68, 68, 76}, kBackground)};
    tuinator::Style border_focused{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 255}, kBackground)};
    tuinator::Style title_normal{tuinator::style_fg_bg(tuinator::Rgb{120, 120, 130}, kBackground)};
    tuinator::Style title_focused{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 255}, kBackground)};
    tuinator::Style panel_background{tuinator::style_bg(kBackground)};
    tuinator::Style root_background{tuinator::style_bg(kBackground)};
    tuinator::Style control_bar_background{tuinator::style_bg(kBackground)};
    tuinator::Style label{tuinator::style_fg_bg(tuinator::Rgb{220, 220, 225}, kBackground)};
    tuinator::Style selection{tuinator::style_fg_bg(tuinator::Rgb{0, 0, 0}, tuinator::Rgb{0, 255, 255})};
    tuinator::Style scope_header{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 255}, kBackground)};
    tuinator::Style thread_header{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 255}, kBackground)};
    tuinator::Style thread_stopped{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 0}, kBackground)};
    tuinator::Style frame_current{tuinator::style_fg_bg(tuinator::Rgb{255, 255, 0}, kBackground)};
    tuinator::Style frame_normal{tuinator::style_fg_bg(tuinator::Rgb{200, 200, 210}, kBackground)};
    tuinator::Style status_bar{tuinator::style_fg_bg(tuinator::Rgb{0, 0, 0}, tuinator::Rgb{80, 160, 200})};
    tuinator::Style divider{tuinator::style_fg_bg(tuinator::Rgb{72, 72, 82}, kBackground)};
    tuinator::Style control_play{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 0}, kBackground)};
    tuinator::Style control_step{tuinator::style_fg_bg(tuinator::Rgb{120, 180, 255}, kBackground)};
    tuinator::Style control_stop{tuinator::style_fg_bg(tuinator::Rgb{220, 80, 80}, kBackground)};
    tuinator::Style control_restart{tuinator::style_fg_bg(tuinator::Rgb{255, 255, 0}, kBackground)};
    tuinator::Style control_disabled{tuinator::style_fg_bg(tuinator::Rgb{80, 80, 90}, kBackground)};

    void apply_to(tuinator::Theme& theme) const;
};

}  // namespace tui_debug_ui
