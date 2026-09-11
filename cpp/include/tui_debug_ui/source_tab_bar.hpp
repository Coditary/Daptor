#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/widget.hpp>

#include <functional>
#include <string>
#include <vector>

namespace tui_debug_ui {

/// Horizontal file tabs with close buttons and scroll arrows (DAP-style buffer bar).
class SourceTabBar : public tuinator::Widget {
  public:
    struct Tab {
        std::string label;
    };

    using SelectCallback = std::function<void(int index)>;
    using CloseCallback = std::function<void(int index)>;

    SourceTabBar(tuinator::Style background, tuinator::Style inactive_label, tuinator::Style active_label,
                 tuinator::Style arrow_style, tuinator::Style close_style, tuinator::Style divider_style);

    void set_tabs(std::vector<Tab> tabs, int active_index);
    void set_on_select(SelectCallback callback);
    void set_on_close(CloseCallback callback);

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    tuinator::Widget* hit_test(tuinator::Point point) override;

  private:
    static constexpr int kDividerRows = 1;
    static constexpr int kLabelRows = 1;
    static constexpr int kHeight = kDividerRows + kLabelRows;
    static constexpr int kTabSeparatorPad = 1;
    static constexpr const char* kTabSeparator = "│";
    static constexpr const char* kPrevGlyph = "◄";
    static constexpr const char* kNextGlyph = "►";
    static constexpr const char* kCloseGlyph = "×";
    static constexpr const char* kLeadingPad = " ";

    struct TabSegment {
        int index = -1;
        int x = 0;
        int width = 0;
        int close_x = -1;
        int close_width = 0;
    };

    struct ArrowSegment {
        int x = 0;
        int width = 0;
        bool visible = false;
    };

    struct Layout {
        int first_visible_index = 0;
        std::vector<TabSegment> tabs;
        std::vector<int> separator_x;
        ArrowSegment prev_arrow;
        ArrowSegment next_arrow;
    };

    [[nodiscard]] int tab_separator_width() const;
    [[nodiscard]] int tab_content_width(int index) const;
    [[nodiscard]] int close_glyph_width() const;
    [[nodiscard]] int arrow_glyph_width() const;
    [[nodiscard]] int scroll_left_overhead() const;
    [[nodiscard]] int scroll_right_overhead() const;
    [[nodiscard]] int tabs_budget() const;
    [[nodiscard]] int max_visible_tabs(int start_index) const;
    void ensure_active_tab_visible();
    [[nodiscard]] Layout build_layout() const;
    [[nodiscard]] tuinator::Style tab_style(int index) const;
    bool handle_click(tuinator::Point position);

    std::vector<Tab> tabs_;
    tuinator::Style background_;
    tuinator::Style inactive_label_;
    tuinator::Style active_label_;
    tuinator::Style arrow_style_;
    tuinator::Style close_style_;
    tuinator::Style divider_style_;
    int active_index_ = 0;
    int tab_scroll_offset_ = 0;
    SelectCallback on_select_;
    CloseCallback on_close_;
};

}  // namespace tui_debug_ui
