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
    StepInCandidate,
    StepInActive,
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
    static constexpr tuinator::Rgb kDefaultBackground{30, 30, 36};

    tuinator::Rgb background{kDefaultBackground};

    tuinator::Style keyword;
    tuinator::Style string;
    tuinator::Style comment;
    tuinator::Style function;
    tuinator::Style type;
    tuinator::Style number;
    tuinator::Style operator_;
    tuinator::Style variable;
    tuinator::Style default_text;

    tuinator::Style line_number;
    tuinator::Style breakpoint_marker;
    tuinator::Style breakpoint_conditional_marker;
    tuinator::Style execution_row;
    tuinator::Style execution_marker;
    tuinator::Style cursor_row;
    tuinator::Style step_in_candidate;
    tuinator::Style step_in_active;
    tuinator::Style panel_background;

    SyntaxTheme();

    void apply_defaults();
    void finalize_styles();

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
        case HighlightKind::StepInCandidate:
            return step_in_candidate;
        case HighlightKind::StepInActive:
            return step_in_active;
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

}  // namespace tui_debug_ui
