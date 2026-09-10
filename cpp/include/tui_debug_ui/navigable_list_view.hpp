#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/views/list_view.hpp>

#include <chrono>
#include <functional>
#include <string>
#include <unordered_set>
#include <variant>
#include <vector>

namespace tuinator {
class ScrollView;
}  // namespace tuinator

namespace tui_debug_ui {

struct DapUiTheme;

enum class ListPaintMode {
    Plain,
    Scopes,
    Stacks,
    DebugSidebar,
    Breakpoints,
    Watches,
};

enum class ListRowActionLayout {
    None,
    BreakpointRow,
    WatchRow,
    VariableRow,
};

enum class RowActionType {
    Add,
    Edit,
    Remove,
};

/// ListView that also accepts j/k for selection (vim-style navigation).
class NavigableListView : public tuinator::ListView {
  public:
    NavigableListView(tuinator::Style item_style, tuinator::Style selected_style,
                      tuinator::Style row_background = {}, bool interactive = true);

    void set_scroll_parent(tuinator::ScrollView* scroll_parent);
    void set_paint_mode(ListPaintMode mode, const DapUiTheme* theme);
    void set_stopped_thread_headers(std::unordered_set<std::string> headers);

    void paint(tuinator::PaintContext& ctx) const override;
    void layout(tuinator::Rect bounds) override;
    bool handle_event(const tuinator::Event& event) override;

    /// Replace items and reset scroll so the first row stays visible.
    void assign_items(std::vector<std::string> items);

    using ActivateCallback = std::function<void(int index)>;
    using RowClickCallback = std::function<bool(int index, const std::string& item)>;
    using RowContextCallback = std::function<void(int index, const std::string& item, tuinator::Point anchor)>;
    void set_on_activate(ActivateCallback callback);
    void set_on_row_click(RowClickCallback callback);
    void set_on_row_context(RowContextCallback callback);
    void set_row_action_layout(ListRowActionLayout layout);
    using RowActionCallback = std::function<void(int index, RowActionType action)>;
    void set_on_row_action(RowActionCallback callback);
    /// When true for a row, the add action is shown as an edit (pencil) glyph instead of '+'.
    using RowActionEditCallback = std::function<bool(int index)>;
    void set_row_action_edit_state(RowActionEditCallback callback);

  private:
    void clamp_scroll_offset();
    void paint_read_only(tuinator::PaintContext& ctx) const;
    void paint_plain_interactive(tuinator::PaintContext& ctx) const;
    void paint_themed(tuinator::PaintContext& ctx) const;
    void paint_themed_row(tuinator::Canvas& canvas, int row, int index, const std::string& item,
                          const std::string& prefix, bool selected, int max_width) const;
    [[nodiscard]] bool row_shows_actions(int index, const std::string& item) const;
    [[nodiscard]] int row_action_reserve_width() const;
    void paint_row_actions(tuinator::Canvas& canvas, int row, const std::string& item, int max_width) const;
    [[nodiscard]] std::optional<RowActionType> row_action_at(int index, const std::string& item, int local_x) const;
    [[nodiscard]] std::string row_add_action_glyph(int index) const;
    [[nodiscard]] int row_add_action_width() const;

    tuinator::Style row_background_;
    tuinator::Style item_style_;
    tuinator::Style selected_style_;
    tuinator::ScrollView* scroll_parent_ = nullptr;
    const DapUiTheme* theme_ = nullptr;
    ListPaintMode paint_mode_ = ListPaintMode::Plain;
    ListRowActionLayout row_action_layout_ = ListRowActionLayout::None;
    std::unordered_set<std::string> stopped_thread_headers_;
    bool interactive_ = true;
    int scroll_offset_ = 0;
    int last_click_row_ = -1;
    std::chrono::steady_clock::time_point last_click_time_{};
    ActivateCallback on_activate_;
    RowClickCallback on_row_click_;
    RowContextCallback on_row_context_;
    RowActionCallback on_row_action_;
    RowActionEditCallback row_action_edit_state_;
};

}  // namespace tui_debug_ui
