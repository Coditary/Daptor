#include "tui_debug_ui/file_tree_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/render/text.hpp>

#include <algorithm>
#include <utility>

namespace tui_debug_ui {

std::string FileTreePanel::encode_row(const TreeRow& row) {
    const WorkspaceNode& node = *row.node;
    std::string out = kFileTreeRowMarker;
    out.push_back(static_cast<char>('0' + std::clamp(row.depth, 0, 9)));
    out.push_back(node.is_directory ? 'D' : 'F');
    out.push_back(node.is_directory && node.expanded ? 'E' : 'C');
    out.push_back(row.last_sibling ? '1' : '0');
    for (const bool last : row.ancestor_is_last) {
        out.push_back(last ? '1' : '0');
    }
    out.push_back('\x1F');
    out += node.name;
    return out;
}

FileTreePanel::FileTreePanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                             const std::string& title) {
    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::FileTree, &theme);
    list_->set_on_activate([this](int index) {
        if (try_toggle_row(index)) {
            return;
        }
        const WorkspaceNode* node = node_at_display_index(index);
        if (node == nullptr || node->is_directory || on_open_file_ == nullptr) {
            return;
        }
        on_open_file_(node->path);
    });
    list_->set_on_row_click([this](int index, const std::string& item, int local_x) {
        if (!NavigableListView::file_tree_expand_arrow_hit(item, local_x)) {
            return false;
        }
        return try_toggle_row(index);
    });

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), theme.title_stacks, theme.panel_background,
                                               std::move(scroll_options), true, false);
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> FileTreePanel::release_widget() { return pane_->release_widget(); }

void FileTreePanel::set_on_open_file(std::function<void(const std::filesystem::path& path)> callback) {
    on_open_file_ = std::move(callback);
}

void FileTreePanel::set_workspace(const std::filesystem::path& root,
                                  const std::vector<std::filesystem::path>& files) {
    if (workspace_root_ == root && workspace_files_ == files) {
        return;
    }
    workspace_root_ = root;
    workspace_files_ = files;
    workspace_tree_ = build_workspace_tree(root, files);
    rebuild_display();
}

void FileTreePanel::append_rows(WorkspaceNode& node, int depth, std::vector<bool> ancestor_is_last,
                                bool last_sibling) {
    display_rows_.push_back({&node, depth, ancestor_is_last, last_sibling});
    if (!node.is_directory || !node.expanded) {
        return;
    }
    for (std::size_t i = 0; i < node.children.size(); ++i) {
        std::vector<bool> child_ancestors = ancestor_is_last;
        child_ancestors.push_back(last_sibling);
        append_rows(node.children[i], depth + 1, child_ancestors, i == node.children.size() - 1);
    }
}

void FileTreePanel::rebuild_display() {
    if (list_ == nullptr) {
        return;
    }

    std::filesystem::path selected_path;
    if (const WorkspaceNode* selected = node_at_display_index(list_->selected_index())) {
        selected_path = selected->path;
    }

    display_rows_.clear();
    if (!workspace_root_.empty()) {
        append_rows(workspace_tree_, 0, {}, true);
    }

    std::vector<std::string> items;
    items.reserve(display_rows_.size());
    for (const TreeRow& row : display_rows_) {
        if (row.node == nullptr) {
            continue;
        }
        items.push_back(encode_row(row));
    }
    if (items.empty()) {
        items.push_back("(no source files in workspace)");
    }

    list_->assign_items(std::move(items), true);

    if (!selected_path.empty()) {
        for (std::size_t i = 0; i < display_rows_.size(); ++i) {
            if (display_rows_[i].node != nullptr && display_rows_[i].node->path == selected_path) {
                list_->set_selected_index(static_cast<int>(i));
                break;
            }
        }
    }
}

WorkspaceNode* FileTreePanel::node_at_display_index(int index) const {
    if (index < 0 || index >= static_cast<int>(display_rows_.size())) {
        return nullptr;
    }
    return display_rows_[static_cast<std::size_t>(index)].node;
}

bool FileTreePanel::try_toggle_row(int index) {
    WorkspaceNode* node = node_at_display_index(index);
    if (node == nullptr || !node->is_directory) {
        return false;
    }
    node->expanded = !node->expanded;
    rebuild_display();
    return true;
}

tuinator::Widget* FileTreePanel::list_widget() const { return list_; }

tuinator::ScrollView* FileTreePanel::scroll_view() const { return pane_->scroll_view(); }

} // namespace tui_debug_ui
