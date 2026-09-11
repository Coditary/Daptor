#include "tui_debug_ui/divider_paint.hpp"

#include <tuinator/render/glyphs.hpp>
#include <tuinator/render/text.hpp>

namespace tui_debug_ui {

namespace {

tuinator::Style emphasized_divider(tuinator::Style style) {
    style.bold = true;
    if (style.foreground == tuinator::Color::Default) {
        style.foreground = tuinator::Color::Cyan;
    }
    return style;
}

tuinator::Style subtle_divider(tuinator::Style style) {
    style.bold = false;
    return style;
}

}  // namespace

void draw_thin_hline(tuinator::Canvas& canvas, int x, int y, int length, tuinator::Style style) {
    if (length <= 0) {
        return;
    }
    canvas.draw_hline(x, y, length, subtle_divider(style));
}

void draw_thick_hline(tuinator::Canvas& canvas, int x, int y, int length, tuinator::Style style) {
    if (length <= 0) {
        return;
    }

    const std::string glyph = tuinator::unicode_heavy_border_glyphs().horizontal;
    const std::string& ch = glyph.empty() ? std::string("=") : glyph;
    const int glyph_width = std::max(1, tuinator::text_display_width(ch));

    std::string line;
    for (int placed = 0; placed < length; placed += glyph_width) {
        line += ch;
    }
    canvas.draw_text({x, y}, line, emphasized_divider(style));
}

void draw_thick_vline(tuinator::Canvas& canvas, int x, int y, int length, tuinator::Style style) {
    if (length <= 0) {
        return;
    }

    const std::string glyph = tuinator::unicode_heavy_border_glyphs().vertical;
    const std::string& ch = glyph.empty() ? std::string("|") : glyph;
    const tuinator::Style line_style = emphasized_divider(style);
    for (int row = y; row < y + length; ++row) {
        canvas.draw_text({x, row}, ch, line_style);
    }
}

}  // namespace tui_debug_ui
