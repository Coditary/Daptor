#pragma once

#include <tuinator/render/glyphs.hpp>
#include <tuinator/render/scrollbar.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/render/theme.hpp>

namespace tui_debug_ui {

/// Semantic UI color palette (default inspired by nvim-dap-ui).
struct DapUiTheme {
    static constexpr tuinator::Rgb kDefaultBackground{30, 30, 36};

    tuinator::Rgb background{kDefaultBackground};

    tuinator::Style border_normal;
    tuinator::Style border_focused;
    tuinator::Style title_normal;
    tuinator::Style title_focused;
    tuinator::Style panel_background;
    tuinator::Style root_background;
    tuinator::Style control_bar_background;
    tuinator::Style label;
    tuinator::Style selection;
    tuinator::Style scope_header;
    tuinator::Style scope_locals_header;
    tuinator::Style scope_globals_header;
    tuinator::Style variable_name;
    tuinator::Style variable_value;
    tuinator::Style breakpoint_marker;
    tuinator::Style breakpoint_file;
    tuinator::Style breakpoint_line_number;
    tuinator::Style breakpoint_condition;
    tuinator::Style breakpoint_hit_condition;
    tuinator::Style breakpoint_hit_count;
    tuinator::Style console_stderr;
    tuinator::Style console_stdout;
    tuinator::Style console_event;
    tuinator::Style title_scopes;
    tuinator::Style title_stacks;
    tuinator::Style title_breakpoints;
    tuinator::Style title_source;
    tuinator::Style title_console;
    tuinator::Style title_repl;
    tuinator::Style repl_ghost;
    tuinator::Style title_watches;
    tuinator::Style thread_header;
    tuinator::Style thread_stopped;
    tuinator::Style frame_current;
    tuinator::Style frame_normal;
    tuinator::Style frame_location;
    tuinator::Style status_bar;
    tuinator::Style divider;
    tuinator::Style control_play;
    tuinator::Style control_step;
    tuinator::Style control_stop;
    tuinator::Style control_restart;
    tuinator::Style control_disabled;
    tuinator::Style row_action_muted;
    tuinator::Style layout_drop_swap;
    tuinator::Style layout_drop_insert;
    tuinator::Style layout_drop_merge;
    tuinator::Style layout_drop_span;
    tuinator::Style layout_menu_pane;
    tuinator::Style file_tree_folder;
    tuinator::Style file_tree_file;
    tuinator::Style file_tree_row_selected;
    tuinator::Style scrollbar_thumb;
    tuinator::Style scrollbar_track;

    DapUiTheme();

    void apply_defaults();
    void finalize_styles();

    [[nodiscard]] tuinator::BorderGlyphs border_glyphs() const;
    [[nodiscard]] tuinator::ScrollViewOptions scroll_view_options() const;

    void apply_to(tuinator::Theme& theme) const;
};

}  // namespace tui_debug_ui
