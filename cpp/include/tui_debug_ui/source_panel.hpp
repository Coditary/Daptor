#pragma once

#include <tui_debug_ui/syntax_theme.hpp>

#include <tuinator/core/geometry.hpp>
#include <tuinator/widgets/widget.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace tuinator {
class ScrollView;
}  // namespace tuinator

namespace tui_debug_ui {

struct HighlightSpan {
    std::string text;
    HighlightKind kind = HighlightKind::Default;
};

struct HighlightedLine {
    int line_number = 1;
    std::vector<HighlightSpan> spans;
};

/// Source viewer with gutter (breakpoints, execution marker, line numbers) and styled spans.
class SourcePanel : public tuinator::Widget {
  public:
    explicit SourcePanel(SyntaxTheme theme = {});

    const std::vector<HighlightedLine>& lines() const { return lines_; }
    void set_lines(std::vector<HighlightedLine> lines);
    /// Replace spans for matching line numbers (keeps gutter scroll layout intact).
    void merge_highlighted_lines(const std::vector<HighlightedLine>& highlighted);
    /// Reset styled spans to plain text for a line range (used before re-highlight on scroll).
    void reset_plain_spans_for_line_range(int first_line, int line_count, const std::string& source_text);

    int scroll_offset() const;
    void set_scroll_offset(int offset);

    int execution_line() const { return execution_line_; }
    void set_execution_line(int line);

    int cursor_line() const { return cursor_line_; }
    void set_cursor_line(int line);

    const std::unordered_map<int, std::string>& breakpoints() const { return breakpoints_; }
    void set_breakpoints(std::unordered_map<int, std::string> breakpoints);

    int file_line_count() const { return file_line_count_; }
    void set_file_line_count(int count);

    void set_scroll_parent(tuinator::ScrollView* scroll_parent);
    int viewport_height() const;
    void ensure_cursor_visible();
    void move_cursor_by(int delta);
    int line_number_at_row(int row) const;

    using BreakpointToggleCallback = std::function<void(int line)>;
    using BreakpointContextCallback = std::function<void(int line, int code_column, tuinator::Point anchor)>;
    void set_on_toggle_breakpoint(BreakpointToggleCallback callback);
    void set_on_breakpoint_context(BreakpointContextCallback callback);

    using ViewportRequestCallback = std::function<void(int center_line)>;
    void set_on_request_viewport(ViewportRequestCallback callback);

    const SyntaxTheme& syntax_theme() const { return theme_; }
    void set_syntax_theme(SyntaxTheme theme);

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    bool is_focusable() const override { return true; }

  private:
    void clamp_scroll();
    int max_scroll() const;
    int gutter_width() const;
    int code_column_from_local_x(int local_x) const;
    tuinator::Style row_style_for_line(int line_number) const;
    void paint_empty(tuinator::PaintContext& ctx) const;
    void paint_line(tuinator::PaintContext& ctx, int row, const HighlightedLine& line) const;
    int code_start_x() const;
    bool is_gutter_click(int local_x) const;

    SyntaxTheme theme_;
    BreakpointToggleCallback on_toggle_breakpoint_;
    BreakpointContextCallback on_breakpoint_context_;
    ViewportRequestCallback on_request_viewport_;
    std::vector<HighlightedLine> lines_;
    std::unordered_map<int, std::string> breakpoints_;
    int scroll_offset_ = 0;
    tuinator::ScrollView* scroll_parent_ = nullptr;
    int execution_line_ = 0;
    int cursor_line_ = 1;
    int file_line_count_ = 1;
};

} // namespace tui_debug_ui
