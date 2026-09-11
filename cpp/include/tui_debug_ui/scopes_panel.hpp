#pragma once

#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/render/style.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tuinator {
class Widget;
}  // namespace tuinator

namespace tui_debug_ui {
class NavigableListView;
class TitledScrollPane;
}  // namespace tui_debug_ui

namespace tui_debug_ui {

struct DapUiTheme;

/// Scope variables as a titled, scrollable list.
class ScopesPanel {
  public:
    ScopesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                const std::string& title = "Variables");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_scope_names(std::vector<std::string> names, std::vector<bool> show_edit = {});
    using ActivateCallback = std::function<void(int index)>;
    void set_on_activate(ActivateCallback callback);
    using WatchCallback = std::function<void(const std::string& variable_name)>;
    using ContextCallback = std::function<void(int index, tuinator::Point anchor)>;
    using EditVariableCallback = std::function<void(const std::string& variable_name)>;
    using SubmitCallback = std::function<void(const std::string& value)>;
    using ChangeCallback = std::function<void(const std::string& value)>;
    void set_on_watch(WatchCallback callback);
    void set_on_context(ContextCallback callback);
    void set_on_edit_variable(EditVariableCallback callback);
    void set_on_submit(SubmitCallback callback);
    void set_on_change(ChangeCallback callback);
    void set_on_inline_edit_cancel(std::function<void()> callback);
    void set_inline_edit(const std::string& variable_name, std::string value);
    void clear_inline_edit();
    [[nodiscard]] bool has_inline_edit() const;
    [[nodiscard]] bool has_active_inline_edit() const;
    [[nodiscard]] std::string inline_edit_value() const;
    void focus_inline_edit();
    bool handle_inline_edit_key(const tuinator::Event& event);
    [[nodiscard]] std::string input_value() const;
    void focus_input();
    tuinator::Widget* list_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    void sync_inline_edit_to_list();
    [[nodiscard]] static std::optional<std::string> variable_name_from_row(const std::string& line);

    struct InlineEditTarget {
        std::string variable_name;
        std::string value;
        bool active = false;
    };

    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    InlineEditTarget inline_edit_;
    int inline_edit_display_index_ = -1;
    ActivateCallback on_activate_;
    WatchCallback on_watch_;
    ContextCallback on_context_;
    EditVariableCallback on_edit_variable_;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    std::function<void()> on_inline_edit_cancel_;
};

}  // namespace tui_debug_ui
