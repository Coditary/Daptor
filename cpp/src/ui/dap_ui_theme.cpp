#include "tui_debug_ui/dap_ui_theme.hpp"

namespace tui_debug_ui {

namespace {

tuinator::Style bold(tuinator::Style style) {
    style.bold = true;
    return style;
}

}  // namespace

DapUiTheme::DapUiTheme() {
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
    frame_current = bold(frame_current);
}

tuinator::BorderGlyphs DapUiTheme::border_glyphs() const {
    if (tuinator::supports_unicode_text()) {
        return tuinator::unicode_rounded_border_glyphs();
    }
    return tuinator::ascii_border_glyphs();
}

tuinator::ScrollViewOptions DapUiTheme::scroll_view_options() const {
    tuinator::ScrollViewOptions options;
    // Tuinator defaults to 40x10; that breaks flex panes smaller than 10 rows.
    options.width = 1;
    options.height = 1;
    options.background = panel_background;

    tuinator::ScrollbarPreset preset =
        tuinator::supports_unicode_text() ? tuinator::ScrollbarPreset::Thin : tuinator::ScrollbarPreset::Ascii;
    tuinator::ScrollbarOptions scrollbars = tuinator::scrollbar_options(preset).with_horizontal(false);
    scrollbars = scrollbars.with_thumb_style(style_fg_bg(tuinator::Rgb{110, 110, 125}, kBackground));
    scrollbars = scrollbars.with_track_style(style_fg_bg(tuinator::Rgb{42, 42, 50}, kBackground));
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
