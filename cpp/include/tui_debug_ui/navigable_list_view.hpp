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

inline constexpr const char* kInlineWhenEditRow = "\x1E\x01when";
inline constexpr const char* kInlineHitEditRow = "\x1E\x01hit";
inline constexpr const char* kInlineVariableEditRow = "\x1E\x01var";

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
    using RowActionCallback = std::function<void(int index, RowActionType action, tuinator::Point anchor)>;
    void set_on_row_action(RowActionCallback callback);
    [[nodiscard]] tuinator::Point row_action_anchor(int index, RowActionType action) const;

    void set_inline_row_edit(int row, std::string prefix, std::string value);
    void set_inline_variable_row_edit(int row, std::string name, std::string value);
    void clear_inline_row_edit();
    [[nodiscard]] bool has_inline_row_edit() const;
    [[nodiscard]] const std::string& inline_row_edit_value() const;
    [[nodiscard]] bool inline_row_edit_active_row(int row) const;
    [[nodiscard]] static bool is_inline_when_edit_row(const std::string& item);
    [[nodiscard]] static bool is_inline_hit_edit_row(const std::string& item);
    [[nodiscard]] static bool is_inline_variable_edit_row(const std::string& item);
    using InlineEditChangeCallback = std::function<void(const std::string& value)>;
    using InlineEditSubmitCallback = std::function<void(const std::string& value)>;
    using InlineEditCancelCallback = std::function<void()>;
    void set_on_inline_edit_change(InlineEditChangeCallback callback);
    void set_on_inline_edit_submit(InlineEditSubmitCallback callback);
    void set_on_inline_edit_cancel(InlineEditCancelCallback callback);
    bool handle_inline_row_edit_key(const tuinator::KeyPress& key);

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
    void paint_inline_row_edit(tuinator::Canvas& canvas, int row, const std::string& prefix, int max_width) const;
    void paint_inline_variable_row_edit(tuinator::Canvas& canvas, int row, int max_width) const;
    [[nodiscard]] std::optional<RowActionType> row_action_at(int index, const std::string& item, int local_x) const;
    [[nodiscard]] tuinator::Point to_terminal_point(tuinator::Point event_position) const;
    [[nodiscard]] static bool is_breakpoint_condition_row(const std::string& item);

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
    struct InlineRowEdit {
        int row = -1;
        std::string prefix;
        std::string label;
        std::string value;
        std::size_t cursor = 0;
    };
    InlineRowEdit inline_row_edit_;
    InlineEditChangeCallback on_inline_edit_change_;
    InlineEditSubmitCallback on_inline_edit_submit_;
    InlineEditCancelCallback on_inline_edit_cancel_;
};

}  // namespace tui_debug_ui
