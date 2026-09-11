#pragma once

#include <tuinator/render/canvas.hpp>
#include <tuinator/render/style.hpp>

namespace tui_debug_ui {

void draw_thin_hline(tuinator::Canvas& canvas, int x, int y, int length, tuinator::Style style);
void draw_thick_hline(tuinator::Canvas& canvas, int x, int y, int length, tuinator::Style style);
void draw_thick_vline(tuinator::Canvas& canvas, int x, int y, int length, tuinator::Style style);

}  // namespace tui_debug_ui
