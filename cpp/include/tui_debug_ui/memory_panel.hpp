#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/core/geometry.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class ScrollView;
class TextInput;
class Widget;
}  // namespace tuinator

namespace tui_debug_ui {
class NavigableListView;
class TitledScrollPane;
struct DapUiTheme;
}  // namespace tui_debug_ui

namespace tui_debug_ui {

enum class MemoryToolbarFocus {
    None,
    Address,
    Search,
};

/// Hex memory dump from DAP `readMemory`, with address navigation and search toolbars.
class MemoryPanel {
  public:
    MemoryPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                const std::string& title = "Memory");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_lines(std::vector<std::string> lines);
    void set_title(std::string title);
    void set_writable(bool writable);
    void set_address_value(std::string value);
    [[nodiscard]] std::string address_value() const;
    void set_search_value(std::string value);
    [[nodiscard]] std::string search_value() const;
    void focus_address_input();
    void focus_search_input();
    void blur_toolbar_inputs();
    [[nodiscard]] bool is_address_input_focused() const;
    [[nodiscard]] bool is_search_input_focused() const;
    [[nodiscard]] bool is_toolbar_input_focused() const;
    [[nodiscard]] bool is_toolbar_active() const;
    [[nodiscard]] bool is_search_toolbar_active() const;
    void set_on_refresh(std::function<void()> callback);
    void set_on_address_submit(std::function<void(const std::string&)> callback);
    void set_on_search_submit(std::function<void(const std::string&)> callback);
    void set_on_activate(std::function<void(int row)> callback);
    void set_on_submit(std::function<void(int row, const std::string& hex)> callback);
    void set_on_inline_edit_cancel(std::function<void()> callback);
    void set_on_toolbar_interact(std::function<void()> callback);
    [[nodiscard]] bool contains_point(tuinator::Point point) const;
    bool activate_toolbar_from_point(tuinator::Point point);
    void begin_row_edit(int row_index, std::string hex_value);
    void clear_inline_edit();
    [[nodiscard]] bool has_inline_edit() const;
    [[nodiscard]] int selected_row() const;
    void set_selected_row(int row_index);
    bool handle_inline_edit_key(const tuinator::Event& event);
    bool handle_toolbar_input_key(const tuinator::Event& event);
    bool focus_toolbar_at_point(tuinator::Point point);
    tuinator::Widget* list_widget() const;
    tuinator::Widget* address_input_widget() const;
    tuinator::Widget* search_input_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    void sync_inline_edit_to_list();
    void reveal_row(int row_index);

    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    tuinator::TextInput* address_input_ = nullptr;
    tuinator::TextInput* search_input_ = nullptr;
    tuinator::Widget* toolbar_row_ = nullptr;
    tuinator::Widget* shell_ = nullptr;
    tuinator::Style title_style_;
    bool writable_ = false;
    MemoryToolbarFocus active_toolbar_ = MemoryToolbarFocus::None;
    int inline_edit_row_ = -1;
    std::string inline_edit_value_;
    std::function<void(int row)> on_activate_;
    std::function<void(int row, const std::string& hex)> on_submit_;
    std::function<void(const std::string&)> on_address_submit_;
    std::function<void(const std::string&)> on_search_submit_;
    std::function<void()> on_inline_edit_cancel_;
    std::function<void()> on_toolbar_interact_;
};

}  // namespace tui_debug_ui
