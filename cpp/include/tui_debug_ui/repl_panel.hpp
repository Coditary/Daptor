#pragma once

#include <tuinator/core/geometry.hpp>
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

/// Debugger REPL — evaluate expressions in the current stack frame.
class ReplPanel {
  public:
    using SubmitCallback = std::function<void(const std::string& expression)>;
    using ChangeCallback = std::function<void(const std::string& expression)>;
    using ActivateCallback = std::function<void()>;

    ReplPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options);

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_history_lines(std::vector<std::string> lines);
    void set_on_submit(SubmitCallback callback);
    void set_on_change(ChangeCallback callback);
    void set_on_activate(ActivateCallback callback);
    void set_input_active(bool active);
    [[nodiscard]] bool input_active() const { return input_active_; }
    void set_input_value(std::string value);
    [[nodiscard]] std::string input_value() const;
    void focus_input();
    [[nodiscard]] bool contains_point(tuinator::Point point) const;
    tuinator::Widget* shell_widget() const { return shell_; }
    tuinator::Widget* history_widget() const;
    tuinator::TextInput* input_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    std::unique_ptr<TitledScrollPane> pane_;
    tuinator::Widget* shell_ = nullptr;
    NavigableListView* history_ = nullptr;
    tuinator::TextInput* input_ = nullptr;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    ActivateCallback on_activate_;
    bool input_active_ = false;
};

}  // namespace tui_debug_ui
