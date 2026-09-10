#pragma once

#include <tuinator/core/geometry.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class TextInput;
class Widget;
}  // namespace tuinator

namespace tui_debug_ui {
class NavigableListView;
class TitledScrollPane;
}  // namespace tui_debug_ui

namespace tui_debug_ui {

struct BreakpointRow {
    std::string path;
    int line = 0;
    std::string source_text;
    std::string condition;
};

struct DapUiTheme;

/// All breakpoints grouped as a flat list for navigation.
class BreakpointsPanel {
  public:
    using ActivateCallback = std::function<void(const BreakpointRow&)>;
    using ContextCallback = std::function<void(const BreakpointRow&, tuinator::Point anchor)>;
    using RemoveCallback = std::function<void(const BreakpointRow&)>;
    using AddConditionCallback = std::function<void(const BreakpointRow&)>;
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
    void set_on_submit(SubmitCallback callback);
    void set_on_change(ChangeCallback callback);
    void set_input_value(std::string value);
    [[nodiscard]] std::string input_value() const;
    void focus_input();
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] const BreakpointRow* selected_row() const;
    tuinator::Widget* list_widget() const;
    tuinator::TextInput* input_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    [[nodiscard]] const BreakpointRow* breakpoint_at_display_index(int index) const;

    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    tuinator::TextInput* input_ = nullptr;
    std::vector<BreakpointRow> rows_;
    std::vector<int> display_to_row_;
    ActivateCallback on_activate_;
    ContextCallback on_context_;
    RemoveCallback on_remove_;
    AddConditionCallback on_add_condition_;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
};

}  // namespace tui_debug_ui
