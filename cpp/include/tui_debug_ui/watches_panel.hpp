#pragma once

#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>

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

struct DapUiTheme;

/// Watch expressions with an input row for adding new entries.
class WatchesPanel {
  public:
    using SubmitCallback = std::function<void(const std::string& expression)>;
    using ChangeCallback = std::function<void(const std::string& expression)>;
    using RemoveCallback = std::function<void(int index)>;
    using EditCallback = std::function<void(int index)>;

    WatchesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options);

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_lines(std::vector<std::string> lines);
    void set_on_submit(SubmitCallback callback);
    void set_on_change(ChangeCallback callback);
    void set_on_remove(RemoveCallback callback);
    void set_on_edit(EditCallback callback);
    void set_input_value(std::string value);
    [[nodiscard]] std::string input_value() const;
    void focus_input();
    [[nodiscard]] int selected_index() const;
    tuinator::Widget* list_widget() const;
    tuinator::TextInput* input_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    tuinator::TextInput* input_ = nullptr;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    RemoveCallback on_remove_;
    EditCallback on_edit_;
};

}  // namespace tui_debug_ui
