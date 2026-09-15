#pragma once

#include <string>

namespace tui_debug_ui {

/// Copy text to the system clipboard (OSC 52, then wl-copy / xclip fallback).
[[nodiscard]] bool copy_to_clipboard(const std::string& text);

}  // namespace tui_debug_ui
