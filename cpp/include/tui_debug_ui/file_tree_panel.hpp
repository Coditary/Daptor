#pragma once

#include "tui_debug_ui/workspace_files.hpp"

#include <tuinator/widgets/containers/scroll_view.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class ScrollView;
class Widget;
} // namespace tuinator

namespace tui_debug_ui {
class NavigableListView;
class TitledScrollPane;
struct DapUiTheme;
} // namespace tui_debug_ui

namespace tui_debug_ui {

/// Expandable workspace file tree for any layout pane.
class FileTreePanel {
  public:
    FileTreePanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                  const std::string& title = "File Tree");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_workspace(const std::filesystem::path& root, const std::vector<std::filesystem::path>& files);
    void set_on_open_file(std::function<void(const std::filesystem::path& path)> callback);
    tuinator::Widget* list_widget() const;
    tuinator::ScrollView* scroll_view() const;

  private:
    struct TreeRow {
        WorkspaceNode* node = nullptr;
        int depth = 0;
        std::vector<bool> ancestor_is_last;
        bool last_sibling = false;
    };

    void rebuild_display();
    void append_rows(WorkspaceNode& node, int depth, std::vector<bool> ancestor_is_last, bool last_sibling);
    [[nodiscard]] static std::string encode_row(const TreeRow& row);
    [[nodiscard]] WorkspaceNode* node_at_display_index(int index) const;
    [[nodiscard]] bool try_toggle_row(int index);

    std::unique_ptr<TitledScrollPane> pane_;
    NavigableListView* list_ = nullptr;
    std::filesystem::path workspace_root_;
    std::vector<std::filesystem::path> workspace_files_;
    WorkspaceNode workspace_tree_;
    std::vector<TreeRow> display_rows_;
    std::function<void(const std::filesystem::path& path)> on_open_file_;
};

} // namespace tui_debug_ui
