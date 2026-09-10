#pragma once

#include <tuinator/render/style.hpp>

#include <string_view>

namespace tui_debug_ui {

enum class HighlightKind {
    Default,
    Keyword,
    String,
    Comment,
    Function,
    Type,
    Number,
    Operator,
    Variable,
};

inline HighlightKind highlight_kind_from_string(std::string_view kind) {
    if (kind == "keyword") {
        return HighlightKind::Keyword;
    }
    if (kind == "string") {
        return HighlightKind::String;
    }
    if (kind == "comment") {
        return HighlightKind::Comment;
    }
    if (kind == "function") {
        return HighlightKind::Function;
    }
    if (kind == "type") {
        return HighlightKind::Type;
    }
    if (kind == "number") {
        return HighlightKind::Number;
    }
    if (kind == "operator") {
        return HighlightKind::Operator;
    }
    if (kind == "variable") {
        return HighlightKind::Variable;
    }
    return HighlightKind::Default;
}

/// Maps semantic highlight kinds and gutter chrome to tuinator styles.
struct SyntaxTheme {
    static constexpr tuinator::Rgb kBackground{30, 30, 36};

    tuinator::Style keyword{tuinator::style_fg_bg(tuinator::Rgb{200, 140, 220}, kBackground)};
    tuinator::Style string{tuinator::style_fg_bg(tuinator::Rgb{180, 220, 140}, kBackground)};
    tuinator::Style comment{tuinator::style_fg_bg(tuinator::Rgb{100, 140, 100}, kBackground)};
    tuinator::Style function{tuinator::style_fg_bg(tuinator::Rgb{120, 180, 255}, kBackground)};
    tuinator::Style type{tuinator::style_fg_bg(tuinator::Rgb{140, 180, 220}, kBackground)};
    tuinator::Style number{tuinator::style_fg_bg(tuinator::Rgb{140, 200, 220}, kBackground)};
    tuinator::Style operator_{tuinator::style_fg_bg(tuinator::Rgb{180, 180, 190}, kBackground)};
    tuinator::Style variable{tuinator::style_fg_bg(tuinator::Rgb{180, 200, 255}, kBackground)};
    tuinator::Style default_text{tuinator::style_fg_bg(tuinator::Rgb{220, 220, 225}, kBackground)};

    tuinator::Style line_number{tuinator::style_fg_bg(tuinator::Rgb{100, 100, 110}, kBackground)};
    tuinator::Style breakpoint_marker{tuinator::style_fg_bg(tuinator::Rgb{220, 60, 60}, kBackground)};
    tuinator::Style breakpoint_conditional_marker{tuinator::style_fg_bg(tuinator::Rgb{255, 200, 80}, kBackground)};
    tuinator::Style execution_row{tuinator::style_fg_bg(tuinator::Rgb{255, 255, 255}, tuinator::Rgb{28, 80, 48})};
    tuinator::Style cursor_row{tuinator::style_fg_bg(tuinator::Rgb{255, 255, 255}, tuinator::Rgb{45, 45, 55})};
    tuinator::Style panel_background{tuinator::style_bg(tuinator::Rgb{30, 30, 36})};

    tuinator::Style style_for(HighlightKind kind) const {
        switch (kind) {
        case HighlightKind::Keyword:
            return keyword;
        case HighlightKind::String:
            return string;
        case HighlightKind::Comment:
            return comment;
        case HighlightKind::Function:
            return function;
        case HighlightKind::Type:
            return type;
        case HighlightKind::Number:
            return number;
        case HighlightKind::Operator:
            return operator_;
        case HighlightKind::Variable:
            return variable;
        case HighlightKind::Default:
            return default_text;
        }
        return default_text;
    }

    tuinator::Style merge_row_background(tuinator::Style span_style, const tuinator::Style& row_style) const {
        if (row_style.background_rgb.has_value()) {
            span_style.background_rgb = row_style.background_rgb;
            span_style.background = row_style.background;
        } else if (!span_style.background_rgb.has_value()) {
            span_style.background_rgb = panel_background.background_rgb;
            span_style.background = panel_background.background;
        }
        if (row_style.foreground_rgb.has_value() && span_style.foreground_rgb == default_text.foreground_rgb) {
            span_style.foreground_rgb = row_style.foreground_rgb;
            span_style.foreground = row_style.foreground;
        }
        if (row_style.bold) {
            span_style.bold = true;
        }
        return span_style;
    }
};

} // namespace tui_debug_ui
