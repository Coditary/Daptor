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
class ScrollView;
class Widget;
}  // namespace tuinator

namespace tui_debug_ui {
class NavigableListView;
class TitledScrollPane;
}  // namespace tui_debug_ui

namespace tui_debug_ui {

struct StackFrameRow {
    std::string name;
    std::uint32_t line = 0;
    std::string path;
    std::int64_t source_reference = 0;
};

struct ThreadStackContent {
    std::int64_t id = 0;
    std::string name;
    bool stopped = false;
    std::vector<StackFrameRow> frames;
};

struct DapUiTheme;

/// Threads and stack frames (nvim-dap-ui stacks element).
class StacksPanel {
  public:
    StacksPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                const std::string& title = "Threads");

    using ActivateCallback = std::function<void(const StackFrameRow&)>;
    using ContextCallback = std::function<void(const StackFrameRow&, tuinator::Point anchor)>;

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_thread_stacks(std::vector<ThreadStackContent> threads);
    void set_lines(std::vector<std::string> lines);
    void set_on_activate(ActivateCallback callback);
    void set_on_continue(std::function<void()> callback);
    void set_on_context(ContextCallback callback);
    tuinator::Widget* list_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    [[nodiscard]] const StackFrameRow* frame_at_display_index(int index) const;

    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    std::vector<StackFrameRow> frames_;
    std::vector<int> display_to_frame_;
    ActivateCallback on_activate_;
    ContextCallback on_context_;
    std::function<void()> on_continue_;
};

}  // namespace tui_debug_ui
