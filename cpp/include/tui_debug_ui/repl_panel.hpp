#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/core/geometry.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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

struct ReplCompletionCandidate {
    std::string label;
    std::string sort_text;
};

struct ReplGhostSuggestion {
    std::string suffix;
    std::size_t replace_start = 0;
    std::size_t replace_length = 0;
    std::string label;
};

/// Debugger REPL — evaluate expressions in the current stack frame.
class ReplPanel {
  public:
    using SubmitCallback = std::function<void(const std::string& expression)>;
    using ChangeCallback = std::function<void(const std::string& expression)>;
    using ActivateCallback = std::function<void()>;
    using CompletionRequestCallback = std::function<void()>;
    using CompletionCycleCallback = std::function<bool(int delta)>;
    using CompletionCancelCallback = std::function<void()>;

    ReplPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options);

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_history_lines(std::vector<std::string> lines);
    void set_on_submit(SubmitCallback callback);
    void set_on_change(ChangeCallback callback);
    void set_on_activate(ActivateCallback callback);
    void set_on_completion_request(CompletionRequestCallback callback);
    void set_on_completion_cycle(CompletionCycleCallback callback);
    void set_on_completion_cancel(CompletionCancelCallback callback);
    void set_input_active(bool active);
    [[nodiscard]] bool input_active() const { return input_active_; }
    void set_input_value(std::string value);
    [[nodiscard]] std::string input_value() const;
    [[nodiscard]] std::size_t input_cursor_column() const;
    void focus_input();
    [[nodiscard]] bool contains_point(tuinator::Point point) const;
    void set_ghost_suggestion(ReplGhostSuggestion suggestion);
    void clear_ghost_suggestion();
    [[nodiscard]] bool has_ghost_suggestion() const;
    [[nodiscard]] const std::optional<ReplGhostSuggestion>& ghost_suggestion() const { return ghost_; }
    bool accept_ghost_suggestion();
    bool handle_tab_key();
    bool handle_completion_cycle(int delta);
    void set_completion_menu_active(bool active);
    void cancel_completion_preview();
    [[nodiscard]] bool has_completion_menu() const;
    bool handle_input_event(const tuinator::Event& event);
    void notify_input_edited();
    tuinator::Widget* shell_widget() const { return shell_; }
    tuinator::Widget* history_widget() const;
    tuinator::TextInput* input_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    std::unique_ptr<TitledScrollPane> pane_;
    tuinator::Widget* shell_ = nullptr;
    tuinator::Widget* input_shell_ = nullptr;
    NavigableListView* history_ = nullptr;
    tuinator::TextInput* input_ = nullptr;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    ActivateCallback on_activate_;
    CompletionRequestCallback on_completion_request_;
    CompletionCycleCallback on_completion_cycle_;
    CompletionCancelCallback on_completion_cancel_;
    bool completion_menu_active_ = false;
    std::optional<ReplGhostSuggestion> ghost_;
    tuinator::Style ghost_style_{};
    bool input_active_ = false;
};

}  // namespace tui_debug_ui
