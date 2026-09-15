#include "tui_debug_ui/dap_ui_theme.hpp"

namespace tui_debug_ui {

namespace {

tuinator::Style bold(tuinator::Style style) {
    style.bold = true;
    return style;
}

tuinator::Style fg_bg(const tuinator::Rgb& fg, const tuinator::Rgb& bg) {
    return tuinator::style_fg_bg(fg, bg);
}

}  // namespace

void DapUiTheme::apply_defaults() {
    background = kDefaultBackground;

    border_normal = fg_bg({68, 68, 76}, background);
    border_focused = fg_bg({0, 255, 255}, background);
    title_normal = fg_bg({120, 120, 130}, background);
    title_focused = fg_bg({0, 255, 255}, background);
    panel_background = tuinator::style_bg(background);
    root_background = tuinator::style_bg(background);
    control_bar_background = tuinator::style_bg(background);
    label = fg_bg({220, 220, 225}, background);
    selection = fg_bg({0, 0, 0}, {0, 255, 255});
    scope_header = fg_bg({0, 255, 255}, background);
    scope_locals_header = fg_bg({0, 255, 255}, background);
    scope_globals_header = fg_bg({200, 140, 220}, background);
    variable_name = fg_bg({180, 200, 255}, background);
    variable_value = fg_bg({220, 220, 220}, background);
    breakpoint_marker = fg_bg({220, 60, 60}, background);
    breakpoint_file = fg_bg({0, 220, 220}, background);
    breakpoint_line_number = fg_bg({120, 200, 140}, background);
    breakpoint_condition = fg_bg({255, 200, 80}, background);
    breakpoint_hit_condition = fg_bg({200, 160, 255}, background);
    breakpoint_hit_count = fg_bg({140, 140, 155}, background);
    console_stderr = fg_bg({255, 120, 120}, background);
    console_stdout = fg_bg({180, 220, 180}, background);
    console_event = fg_bg({140, 180, 220}, background);
    title_scopes = fg_bg({0, 220, 220}, background);
    title_stacks = fg_bg({120, 200, 140}, background);
    title_breakpoints = fg_bg({220, 100, 100}, background);
    title_source = fg_bg({220, 200, 100}, background);
    title_console = fg_bg({120, 180, 255}, background);
    title_repl = fg_bg({180, 140, 220}, background);
    repl_ghost = fg_bg({110, 110, 120}, background);
    title_watches = fg_bg({220, 180, 100}, background);
    thread_header = fg_bg({0, 255, 255}, background);
    thread_stopped = fg_bg({0, 255, 0}, background);
    frame_current = fg_bg({255, 255, 0}, background);
    frame_normal = fg_bg({200, 200, 210}, background);
    frame_location = fg_bg({0, 220, 220}, background);
    status_bar = fg_bg({0, 0, 0}, {80, 160, 200});
    divider = fg_bg({72, 72, 82}, background);
    control_play = fg_bg({0, 255, 0}, background);
    control_step = fg_bg({120, 180, 255}, background);
    control_stop = fg_bg({220, 80, 80}, background);
    control_restart = fg_bg({255, 255, 0}, background);
    control_disabled = fg_bg({80, 80, 90}, background);
    row_action_muted = fg_bg({72, 72, 82}, background);
    layout_drop_swap = fg_bg({210, 230, 255}, {36, 72, 130});
    layout_drop_insert = fg_bg({200, 245, 255}, {28, 90, 120});
    layout_drop_merge = fg_bg({220, 235, 255}, {50, 80, 150});
    layout_drop_span = fg_bg({190, 220, 255}, {24, 64, 110});
    layout_menu_pane = fg_bg({170, 205, 255}, {30, 62, 108});
    file_tree_folder = fg_bg({220, 160, 80}, background);
    file_tree_file = fg_bg({120, 200, 140}, background);
    file_tree_row_selected = tuinator::style_bg({42, 42, 50});
    scrollbar_thumb = fg_bg({110, 110, 125}, background);
    scrollbar_track = fg_bg({42, 42, 50}, background);
}

void DapUiTheme::finalize_styles() {
    scope_header = bold(scope_header);
    scope_locals_header = bold(scope_locals_header);
    scope_globals_header = bold(scope_globals_header);
    thread_header = bold(thread_header);
    thread_stopped = bold(thread_stopped);
    breakpoint_marker = bold(breakpoint_marker);
    breakpoint_condition = bold(breakpoint_condition);
    breakpoint_hit_condition = bold(breakpoint_hit_condition);
    breakpoint_file = bold(breakpoint_file);
    title_scopes = bold(title_scopes);
    title_stacks = bold(title_stacks);
    title_breakpoints = bold(title_breakpoints);
    title_source = bold(title_source);
    title_console = bold(title_console);
    title_repl = bold(title_repl);
    frame_current = bold(frame_current);
    repl_ghost.dim = true;
}

DapUiTheme::DapUiTheme() {
    apply_defaults();
    finalize_styles();
}

tuinator::BorderGlyphs DapUiTheme::border_glyphs() const {
    if (tuinator::supports_unicode_text()) {
        return tuinator::unicode_rounded_border_glyphs();
    }
    return tuinator::ascii_border_glyphs();
}

tuinator::ScrollViewOptions DapUiTheme::scroll_view_options() const {
    tuinator::ScrollViewOptions options;
    options.width = 1;
    options.height = 1;
    options.background = panel_background;

    tuinator::ScrollbarPreset preset =
        tuinator::supports_unicode_text() ? tuinator::ScrollbarPreset::Thin : tuinator::ScrollbarPreset::Ascii;
    tuinator::ScrollbarOptions scrollbars = tuinator::scrollbar_options(preset).with_horizontal(false);
    scrollbars = scrollbars.with_thumb_style(scrollbar_thumb);
    scrollbars = scrollbars.with_track_style(scrollbar_track);
    options.scrollbars = scrollbars;
    return options;
}

void DapUiTheme::apply_to(tuinator::Theme& theme) const {
    theme.border = border_normal;
    theme.heading = title_normal;
    theme.label = label;
    theme.button = label;
    theme.button_focused = selection;
    theme.text_input = label;
    theme.text_input_focused = selection;
    theme.glyphs = border_glyphs();
}

}  // namespace tui_debug_ui
