#include "tui_debug_ui/layout_tree.hpp"

#include <algorithm>

namespace tui_debug_ui {

LayoutNode& LayoutTree::node(LayoutNodeId id) { return nodes_.at(id); }

const LayoutNode& LayoutTree::node(LayoutNodeId id) const { return nodes_.at(id); }

std::vector<LayoutNodeId> LayoutTree::leaf_ids() const {
    std::vector<LayoutNodeId> ids;
    ids.reserve(nodes_.size());
    for (const auto& [id, node] : nodes_) {
        if (node.kind == LayoutNode::Kind::Leaf) {
            ids.push_back(id);
        }
    }
    std::sort(ids.begin(), ids.end());
    return ids;
}

int LayoutTree::leaf_count() const {
    int count = 0;
    for (const auto& [id, node] : nodes_) {
        if (node.kind == LayoutNode::Kind::Leaf) {
            ++count;
        }
    }
    return count;
}

std::optional<LayoutNodeId> LayoutTree::parent_of(LayoutNodeId id) const {
    const auto it = parents_.find(id);
    if (it == parents_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool LayoutTree::is_only_leaf(LayoutNodeId id) const {
    return leaf_count() <= 1 && nodes_.count(id) > 0 && node(id).kind == LayoutNode::Kind::Leaf;
}

std::optional<LayoutNodeId> LayoutTree::find_leaf_for_dock(PanelDock dock) const {
    for (LayoutNodeId id : leaf_ids()) {
        const LayoutLeafData& leaf = node(id).leaf;
        if (leaf.dock.has_value() && *leaf.dock == dock) {
            return id;
        }
    }
    return std::nullopt;
}

bool LayoutTree::subtree_contains_dock(LayoutNodeId node_id, PanelDock dock) const {
    if (!nodes_.contains(node_id)) {
        return false;
    }
    const LayoutNode& current = node(node_id);
    if (current.kind == LayoutNode::Kind::Leaf) {
        return current.leaf.dock.has_value() && *current.leaf.dock == dock;
    }
    return subtree_contains_dock(current.split.first, dock) || subtree_contains_dock(current.split.second, dock);
}

LayoutNodeId LayoutTree::alloc_leaf() {
    const LayoutNodeId id = next_id_++;
    nodes_[id] = LayoutNode{};
    nodes_[id].kind = LayoutNode::Kind::Leaf;
    return id;
}

LayoutNodeId LayoutTree::alloc_split(tuinator::SplitOrientation orientation, LayoutNodeId first, LayoutNodeId second,
                                     std::uint16_t first_pct) {
    const LayoutNodeId id = next_id_++;
    LayoutNode split_node;
    split_node.kind = LayoutNode::Kind::Split;
    split_node.split.orientation = orientation;
    split_node.split.first = first;
    split_node.split.second = second;
    split_node.split.first_pct = first_pct;
    nodes_[id] = std::move(split_node);
    parents_[first] = id;
    parents_[second] = id;
    return id;
}

void LayoutTree::init_default_three_pane(std::uint16_t sidebar_pct, std::uint16_t bottom_pct) {
    nodes_.clear();
    parents_.clear();
    next_id_ = 1;

    const LayoutNodeId sidebar_leaf = alloc_leaf();
    node(sidebar_leaf).leaf.dock = PanelDock::Sidebar;
    const LayoutNodeId main_leaf = alloc_leaf();
    node(main_leaf).leaf.dock = PanelDock::Main;
    const LayoutNodeId bottom_leaf = alloc_leaf();
    node(bottom_leaf).leaf.dock = PanelDock::Bottom;

    const LayoutNodeId row =
        alloc_split(tuinator::SplitOrientation::Horizontal, sidebar_leaf, main_leaf, sidebar_pct);
    const std::uint16_t main_pct = static_cast<std::uint16_t>(std::clamp(100 - bottom_pct, 1, 99));
    root_ = alloc_split(tuinator::SplitOrientation::Vertical, row, bottom_leaf, main_pct);
    parents_[row] = root_;
}

void LayoutTree::replace_child(LayoutNodeId parent, LayoutNodeId old_child, LayoutNodeId new_child) {
    LayoutNode& parent_node = node(parent);
    if (parent_node.kind != LayoutNode::Kind::Split) {
        return;
    }
    if (parent_node.split.first == old_child) {
        parent_node.split.first = new_child;
    } else if (parent_node.split.second == old_child) {
        parent_node.split.second = new_child;
    }
    parents_[new_child] = parent;
}

LayoutNodeId LayoutTree::split_leaf(LayoutNodeId leaf_id, PaneSplitDirection direction) {
    const std::optional<LayoutNodeId> old_parent = parent_of(leaf_id);

    const LayoutNodeId new_leaf = alloc_leaf();
    const tuinator::SplitOrientation orientation =
        direction == PaneSplitDirection::Left || direction == PaneSplitDirection::Right
            ? tuinator::SplitOrientation::Horizontal
            : tuinator::SplitOrientation::Vertical;
    const LayoutNodeId first =
        direction == PaneSplitDirection::Left || direction == PaneSplitDirection::Up ? new_leaf : leaf_id;
    const LayoutNodeId second =
        direction == PaneSplitDirection::Left || direction == PaneSplitDirection::Up ? leaf_id : new_leaf;
    const LayoutNodeId split_id = alloc_split(orientation, first, second, 50);

    if (leaf_id == root_) {
        root_ = split_id;
        parents_.erase(leaf_id);
    } else if (old_parent.has_value()) {
        replace_child(*old_parent, leaf_id, split_id);
    } else {
        root_ = split_id;
        parents_.erase(leaf_id);
    }

    return new_leaf;
}

bool LayoutTree::delete_leaf(LayoutNodeId leaf_id) {
    if (leaf_count() <= 1) {
        return false;
    }
    if (!nodes_.contains(leaf_id) || node(leaf_id).kind != LayoutNode::Kind::Leaf) {
        return false;
    }

    const std::optional<LayoutNodeId> parent = parent_of(leaf_id);
    if (!parent.has_value()) {
        return false;
    }

    LayoutNode& parent_node = node(*parent);
    if (parent_node.kind != LayoutNode::Kind::Split) {
        return false;
    }

    const LayoutNodeId sibling =
        parent_node.split.first == leaf_id ? parent_node.split.second : parent_node.split.first;
    const LayoutNodeId parent_id = *parent;
    const std::optional<LayoutNodeId> grandparent = parent_of(parent_id);
    const bool parent_is_root = parent_id == root_;

    nodes_.erase(leaf_id);
    parents_.erase(leaf_id);

    nodes_.erase(parent_id);
    parents_.erase(parent_id);

    if (parent_is_root) {
        root_ = sibling;
        parents_.erase(sibling);
    } else if (grandparent.has_value()) {
        replace_child(*grandparent, parent_id, sibling);
    }

    return true;
}

}  // namespace tui_debug_ui
