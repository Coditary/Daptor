#include "tui_debug_ui/syntax_theme.hpp"

namespace tui_debug_ui {

namespace {

tuinator::Style fg_bg(const tuinator::Rgb& fg, const tuinator::Rgb& bg) {
    return tuinator::style_fg_bg(fg, bg);
}

}  // namespace

void SyntaxTheme::apply_defaults() {
    background = kDefaultBackground;

    keyword = fg_bg({200, 140, 220}, background);
    string = fg_bg({180, 220, 140}, background);
    comment = fg_bg({100, 140, 100}, background);
    function = fg_bg({120, 180, 255}, background);
    type = fg_bg({140, 180, 220}, background);
    number = fg_bg({140, 200, 220}, background);
    operator_ = fg_bg({180, 180, 190}, background);
    variable = fg_bg({180, 200, 255}, background);
    default_text = fg_bg({220, 220, 225}, background);

    line_number = fg_bg({100, 100, 110}, background);
    breakpoint_marker = fg_bg({220, 60, 60}, background);
    breakpoint_conditional_marker = fg_bg({255, 200, 80}, background);
    execution_row = fg_bg({255, 255, 255}, {28, 80, 48});
    execution_marker = fg_bg({120, 200, 140}, background);
    cursor_row = fg_bg({255, 255, 255}, {45, 45, 55});
    step_in_candidate = fg_bg({30, 25, 0}, {200, 160, 40});
    step_in_active = fg_bg({0, 0, 0}, {255, 235, 80});
    panel_background = tuinator::style_bg(background);
}

void SyntaxTheme::finalize_styles() {
    keyword.bold = true;
    breakpoint_marker.bold = true;
    execution_row.bold = true;
}

SyntaxTheme::SyntaxTheme() {
    apply_defaults();
    finalize_styles();
}

}  // namespace tui_debug_ui
