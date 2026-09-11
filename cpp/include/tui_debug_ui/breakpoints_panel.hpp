#pragma once

#include <tuinator/core/geometry.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace tuinator {
class Widget;
}  // namespace tuinator

namespace tui_debug_ui {
class TitledScrollPane;
}  // namespace tui_debug_ui

#include "tui_debug_ui/navigable_list_view.hpp"

namespace tui_debug_ui {

enum class BreakpointRowKind {
    Source,
    Data,
    Function,
    Exception,
};

struct BreakpointRow {
    BreakpointRowKind kind = BreakpointRowKind::Source;
    std::string path;
    int line = 0;
    std::string source_text;
    std::string condition;
    std::string hit_condition;
    std::uint64_t hit_count = 0;
    std::string data_id;
    std::string access_type;
    bool exception_enabled = false;
    bool exception_supports_condition = false;
};

struct DapUiTheme;

/// All breakpoints grouped as a flat list for navigation.
class BreakpointsPanel {
  public:
    using ActivateCallback = std::function<void(const BreakpointRow&)>;
    using ContextCallback = std::function<void(const BreakpointRow&, tuinator::Point anchor)>;
    using RemoveCallback = std::function<void(const BreakpointRow&)>;
    using AddConditionCallback = std::function<void(const BreakpointRow&, int display_index, tuinator::Point action_anchor)>;
    using EditConditionCallback = std::function<void(const BreakpointRow&, tuinator::Point action_anchor)>;
    using ClearConditionCallback = std::function<void(const BreakpointRow&)>;
    using SubmitCallback = std::function<void(const std::string& condition)>;
    using ChangeCallback = std::function<void(const std::string& condition)>;

    BreakpointsPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                     const std::string& title = "Breakpoints");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_breakpoints(std::vector<BreakpointRow> rows);
    void set_on_activate(ActivateCallback callback);
    void set_on_context(ContextCallback callback);
    void set_on_remove(RemoveCallback callback);
    void set_on_add_condition(AddConditionCallback callback);
    void set_on_edit_when_condition(EditConditionCallback callback);
    void set_on_edit_hit_condition(EditConditionCallback callback);
    void set_on_clear_when_condition(ClearConditionCallback callback);
    void set_on_clear_hit_condition(ClearConditionCallback callback);
    void set_on_submit(SubmitCallback callback);
    void set_on_change(ChangeCallback callback);
    void set_on_inline_edit_cancel(std::function<void()> callback);
    void set_inline_edit(const std::string& path, int line, bool hit, std::string value);
    void set_exception_inline_edit(const std::string& filter, std::string value);
    void clear_inline_edit();
    [[nodiscard]] bool has_inline_edit() const;
    [[nodiscard]] std::string inline_edit_value() const;
    void focus_inline_edit();
    bool handle_inline_edit_key(const tuinator::Event& event);
    [[nodiscard]] std::string input_value() const;
    void focus_input();
    [[nodiscard]] tuinator::Point row_anchor(int display_index) const;
    [[nodiscard]] tuinator::Point row_action_anchor(int display_index, RowActionType action) const;
    [[nodiscard]] tuinator::Rect panel_bounds() const;
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] const BreakpointRow* selected_row() const;
    tuinator::Widget* panel_widget() const;
    tuinator::Widget* list_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    enum class DisplayLineKind {
        None,
        GroupHeader,
        Breakpoint,
        WhenCondition,
        HitCondition,
        WhenConditionEditing,
        HitConditionEditing,
        DataBreakpoint,
        FunctionBreakpoint,
        ExceptionBreakpoint,
    };

    void rebuild_display();
    [[nodiscard]] bool try_toggle_expand(int display_index);
    [[nodiscard]] static std::string breakpoint_condition_key(const BreakpointRow& row);
    [[nodiscard]] bool row_has_collapsible_conditions(const BreakpointRow& row) const;
    [[nodiscard]] bool conditions_expanded_for_row(const BreakpointRow& row) const;
    [[nodiscard]] const BreakpointRow* breakpoint_at_display_index(int index) const;
    [[nodiscard]] DisplayLineKind display_kind_at(int index) const;

    struct InlineEditTarget {
        std::string path;
        int line = 0;
        bool hit = false;
        std::string exception_filter;
        bool exception = false;
        std::string value;
        bool active = false;
    };

    void sync_inline_edit_to_list();
    [[nodiscard]] static bool paths_match(const std::string& left, const std::string& right);

    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    InlineEditTarget inline_edit_;
    int inline_edit_display_index_ = -1;
    std::vector<BreakpointRow> rows_;
    std::vector<int> display_to_row_;
    std::vector<DisplayLineKind> display_kind_;
    std::vector<std::string> display_expand_keys_;
    std::unordered_set<std::string> collapsed_group_keys_;
    std::unordered_set<std::string> collapsed_condition_keys_;
    ActivateCallback on_activate_;
    ContextCallback on_context_;
    RemoveCallback on_remove_;
    AddConditionCallback on_add_condition_;
    EditConditionCallback on_edit_when_condition_;
    EditConditionCallback on_edit_hit_condition_;
    ClearConditionCallback on_clear_when_condition_;
    ClearConditionCallback on_clear_hit_condition_;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    std::function<void()> on_inline_edit_cancel_;
};

}  // namespace tui_debug_ui
