#include "tui_debug_ui/dap_ui_theme.hpp"

namespace tui_debug_ui {

void DapUiTheme::apply_to(tuinator::Theme& theme) const {
    theme.border = border_normal;
    theme.heading = title_normal;
    theme.label = label;
    theme.button = label;
    theme.button_focused = selection;
    theme.text_input = label;
    theme.text_input_focused = selection;
}

}  // namespace tui_debug_ui
