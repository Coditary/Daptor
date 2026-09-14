#pragma once

#include <tuinator/render/glyphs.hpp>
#include <tuinator/render/scrollbar.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/render/theme.hpp>

namespace tui_debug_ui {

/// Color palette aligned with the Ratatui nvim-dap-ui port (`src/ui/theme.rs`).
struct DapUiTheme {
    DapUiTheme();

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
    tuinator::Style scope_locals_header{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 255}, kBackground)};
    tuinator::Style scope_globals_header{tuinator::style_fg_bg(tuinator::Rgb{200, 140, 220}, kBackground)};
    tuinator::Style variable_name{tuinator::style_fg_bg(tuinator::Rgb{180, 200, 255}, kBackground)};
    tuinator::Style variable_value{tuinator::style_fg_bg(tuinator::Rgb{220, 220, 220}, kBackground)};
    tuinator::Style breakpoint_marker{tuinator::style_fg_bg(tuinator::Rgb{220, 60, 60}, kBackground)};
    tuinator::Style breakpoint_file{tuinator::style_fg_bg(tuinator::Rgb{0, 220, 220}, kBackground)};
    tuinator::Style breakpoint_line_number{tuinator::style_fg_bg(tuinator::Rgb{120, 200, 140}, kBackground)};
    tuinator::Style breakpoint_condition{tuinator::style_fg_bg(tuinator::Rgb{255, 200, 80}, kBackground)};
    tuinator::Style breakpoint_hit_condition{tuinator::style_fg_bg(tuinator::Rgb{200, 160, 255}, kBackground)};
    tuinator::Style breakpoint_hit_count{tuinator::style_fg_bg(tuinator::Rgb{140, 140, 155}, kBackground)};
    tuinator::Style console_stderr{tuinator::style_fg_bg(tuinator::Rgb{255, 120, 120}, kBackground)};
    tuinator::Style console_stdout{tuinator::style_fg_bg(tuinator::Rgb{180, 220, 180}, kBackground)};
    tuinator::Style console_event{tuinator::style_fg_bg(tuinator::Rgb{140, 180, 220}, kBackground)};
    tuinator::Style title_scopes{tuinator::style_fg_bg(tuinator::Rgb{0, 220, 220}, kBackground)};
    tuinator::Style title_stacks{tuinator::style_fg_bg(tuinator::Rgb{120, 200, 140}, kBackground)};
    tuinator::Style title_breakpoints{tuinator::style_fg_bg(tuinator::Rgb{220, 100, 100}, kBackground)};
    tuinator::Style title_source{tuinator::style_fg_bg(tuinator::Rgb{220, 200, 100}, kBackground)};
    tuinator::Style title_console{tuinator::style_fg_bg(tuinator::Rgb{120, 180, 255}, kBackground)};
    tuinator::Style title_repl{tuinator::style_fg_bg(tuinator::Rgb{180, 140, 220}, kBackground)};
    tuinator::Style repl_ghost{tuinator::style_fg_bg(tuinator::Rgb{110, 110, 120}, kBackground)};
    tuinator::Style title_watches{tuinator::style_fg_bg(tuinator::Rgb{220, 180, 100}, kBackground)};
    tuinator::Style thread_header{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 255}, kBackground)};
    tuinator::Style thread_stopped{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 0}, kBackground)};
    tuinator::Style frame_current{tuinator::style_fg_bg(tuinator::Rgb{255, 255, 0}, kBackground)};
    tuinator::Style frame_normal{tuinator::style_fg_bg(tuinator::Rgb{200, 200, 210}, kBackground)};
    tuinator::Style frame_location{tuinator::style_fg_bg(tuinator::Rgb{0, 220, 220}, kBackground)};
    tuinator::Style status_bar{tuinator::style_fg_bg(tuinator::Rgb{0, 0, 0}, tuinator::Rgb{80, 160, 200})};
    tuinator::Style divider{tuinator::style_fg_bg(tuinator::Rgb{72, 72, 82}, kBackground)};
    tuinator::Style control_play{tuinator::style_fg_bg(tuinator::Rgb{0, 255, 0}, kBackground)};
    tuinator::Style control_step{tuinator::style_fg_bg(tuinator::Rgb{120, 180, 255}, kBackground)};
    tuinator::Style control_stop{tuinator::style_fg_bg(tuinator::Rgb{220, 80, 80}, kBackground)};
    tuinator::Style control_restart{tuinator::style_fg_bg(tuinator::Rgb{255, 255, 0}, kBackground)};
    tuinator::Style control_disabled{tuinator::style_fg_bg(tuinator::Rgb{80, 80, 90}, kBackground)};
    tuinator::Style row_action_muted{tuinator::style_fg_bg(tuinator::Rgb{72, 72, 82}, kBackground)};
    tuinator::Style layout_drop_swap{tuinator::style_fg_bg(tuinator::Rgb{210, 230, 255}, tuinator::Rgb{36, 72, 130})};
    tuinator::Style layout_drop_insert{tuinator::style_fg_bg(tuinator::Rgb{200, 245, 255}, tuinator::Rgb{28, 90, 120})};
    tuinator::Style layout_drop_merge{tuinator::style_fg_bg(tuinator::Rgb{220, 235, 255}, tuinator::Rgb{50, 80, 150})};
    tuinator::Style layout_drop_span{tuinator::style_fg_bg(tuinator::Rgb{190, 220, 255}, tuinator::Rgb{24, 64, 110})};
    tuinator::Style layout_menu_pane{tuinator::style_fg_bg(tuinator::Rgb{170, 205, 255}, tuinator::Rgb{30, 62, 108})};
    tuinator::Style file_tree_folder{tuinator::style_fg_bg(tuinator::Rgb{220, 160, 80}, kBackground)};
    tuinator::Style file_tree_file{tuinator::style_fg_bg(tuinator::Rgb{120, 200, 140}, kBackground)};
    tuinator::Style file_tree_row_selected{tuinator::style_bg(tuinator::Rgb{42, 42, 50})};

    [[nodiscard]] tuinator::BorderGlyphs border_glyphs() const;
    [[nodiscard]] tuinator::ScrollViewOptions scroll_view_options() const;

    void apply_to(tuinator::Theme& theme) const;
};

}  // namespace tui_debug_ui
