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

/// Watch expressions in a titled, scrollable list with inline add/edit.
class WatchesPanel {
  public:
    using SubmitCallback = std::function<void(const std::string& expression)>;
    using ChangeCallback = std::function<void(const std::string& expression)>;
    using RemoveCallback = std::function<void(int index)>;
    using EditCallback = std::function<void(int index)>;
    using AddCallback = std::function<void()>;

    WatchesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options);

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_lines(std::vector<std::string> lines);
    void set_on_submit(SubmitCallback callback);
    void set_on_change(ChangeCallback callback);
    void set_on_remove(RemoveCallback callback);
    void set_on_edit(EditCallback callback);
    void set_on_add(AddCallback callback);
    void set_on_inline_edit_cancel(std::function<void()> callback);
    void set_inline_edit(int watch_index, std::string expression, std::string display_suffix);
    void clear_inline_edit();
    [[nodiscard]] bool has_inline_edit() const;
    [[nodiscard]] bool has_active_inline_edit() const;
    [[nodiscard]] std::string inline_edit_value() const;
    void focus_inline_edit();
    bool handle_inline_edit_key(const tuinator::Event& event);
    void set_input_value(std::string value);
    [[nodiscard]] std::string input_value() const;
    void focus_input();
    [[nodiscard]] int selected_index() const;
    [[nodiscard]] int selected_watch_index() const;
    tuinator::Widget* list_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    void sync_inline_edit_to_list();
    [[nodiscard]] std::optional<int> watch_index_for_display(int display_index) const;

    struct InlineEditTarget {
        int watch_index = -1;
        std::string expression;
        std::string display_suffix;
        bool active = false;
    };

    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    InlineEditTarget inline_edit_;
    int inline_edit_display_index_ = -1;
    int add_prompt_index_ = -1;
    SubmitCallback on_submit_;
    ChangeCallback on_change_;
    RemoveCallback on_remove_;
    EditCallback on_edit_;
    AddCallback on_add_;
    std::function<void()> on_inline_edit_cancel_;
};

}  // namespace tui_debug_ui
