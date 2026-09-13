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

bool LayoutTree::subtree_contains(LayoutNodeId ancestor, LayoutNodeId descendant) const {
    if (ancestor == descendant) {
        return true;
    }
    if (!nodes_.contains(ancestor) || !nodes_.contains(descendant)) {
        return false;
    }
    const LayoutNode& current = node(ancestor);
    if (current.kind == LayoutNode::Kind::Leaf) {
        return false;
    }
    return subtree_contains(current.container.first, descendant) || subtree_contains(current.container.second, descendant);
}

bool LayoutTree::subtree_contains_dock(LayoutNodeId node_id, PanelDock dock) const {
    if (!nodes_.contains(node_id)) {
        return false;
    }
    const LayoutNode& current = node(node_id);
    if (current.kind == LayoutNode::Kind::Leaf) {
        return current.leaf.dock.has_value() && *current.leaf.dock == dock;
    }
    return subtree_contains_dock(current.container.first, dock) ||
           subtree_contains_dock(current.container.second, dock);
}

LayoutNodeId LayoutTree::alloc_leaf() {
    const LayoutNodeId id = next_id_++;
    nodes_[id] = LayoutNode{};
    nodes_[id].kind = LayoutNode::Kind::Leaf;
    return id;
}

LayoutNodeId LayoutTree::alloc_container(tuinator::SplitOrientation orientation, LayoutNodeId first,
                                         LayoutNodeId second, std::uint16_t first_pct) {
    const LayoutNodeId id = next_id_++;
    LayoutNode container_node;
    container_node.kind = LayoutNode::Kind::Container;
    container_node.container.orientation = orientation;
    container_node.container.first = first;
    container_node.container.second = second;
    container_node.container.first_pct = first_pct;
    nodes_[id] = std::move(container_node);
    parents_[first] = id;
    parents_[second] = id;
    return id;
}

void LayoutTree::init_default_three_pane(std::uint16_t sidebar_pct, std::uint16_t bottom_pct) {
    nodes_.clear();
    parents_.clear();
    next_id_ = 1;

    const LayoutNodeId left_leaf = alloc_leaf();
    node(left_leaf).leaf.dock = PanelDock::Left;
    const LayoutNodeId center_leaf = alloc_leaf();
    node(center_leaf).leaf.dock = PanelDock::Center;
    const LayoutNodeId bottom_leaf = alloc_leaf();
    node(bottom_leaf).leaf.dock = PanelDock::Bottom;

    const LayoutNodeId row =
        alloc_container(tuinator::SplitOrientation::Horizontal, left_leaf, center_leaf, sidebar_pct);
    const std::uint16_t upper_pct = static_cast<std::uint16_t>(std::clamp(100 - bottom_pct, 1, 99));
    root_ = alloc_container(tuinator::SplitOrientation::Vertical, row, bottom_leaf, upper_pct);
    parents_[row] = root_;
}

void LayoutTree::replace_child(LayoutNodeId parent, LayoutNodeId old_child, LayoutNodeId new_child) {
    LayoutNode& parent_node = node(parent);
    if (parent_node.kind != LayoutNode::Kind::Container) {
        return;
    }
    if (parent_node.container.first == old_child) {
        parent_node.container.first = new_child;
    } else if (parent_node.container.second == old_child) {
        parent_node.container.second = new_child;
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
    const LayoutNodeId container_id = alloc_container(orientation, first, second, 50);

    if (leaf_id == root_) {
        root_ = container_id;
        parents_.erase(leaf_id);
    } else if (old_parent.has_value()) {
        replace_child(*old_parent, leaf_id, container_id);
    } else {
        root_ = container_id;
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
    if (parent_node.kind != LayoutNode::Kind::Container) {
        return false;
    }

    const LayoutNodeId sibling =
        parent_node.container.first == leaf_id ? parent_node.container.second : parent_node.container.first;
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

void LayoutTree::swap_leaf_contents(LayoutNodeId a, LayoutNodeId b) {
    if (a == b || !nodes_.contains(a) || !nodes_.contains(b)) {
        return;
    }
    if (node(a).kind != LayoutNode::Kind::Leaf || node(b).kind != LayoutNode::Kind::Leaf) {
        return;
    }
    std::swap(node(a).leaf, node(b).leaf);
}

std::optional<LayoutLeafData> LayoutTree::extract_leaf(LayoutNodeId leaf_id) {
    if (leaf_count() <= 1) {
        return std::nullopt;
    }
    if (!nodes_.contains(leaf_id) || node(leaf_id).kind != LayoutNode::Kind::Leaf) {
        return std::nullopt;
    }

    LayoutLeafData extracted = std::move(node(leaf_id).leaf);

    const std::optional<LayoutNodeId> parent = parent_of(leaf_id);
    if (!parent.has_value()) {
        return std::nullopt;
    }

    LayoutNode& parent_node = node(*parent);
    if (parent_node.kind != LayoutNode::Kind::Container) {
        return std::nullopt;
    }

    const LayoutNodeId sibling =
        parent_node.container.first == leaf_id ? parent_node.container.second : parent_node.container.first;
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

    return extracted;
}

bool LayoutTree::insert_adjacent(LayoutNodeId anchor, PaneSplitDirection direction, LayoutLeafData& leaf_data) {
    if (!nodes_.contains(anchor)) {
        return false;
    }

    const LayoutNodeId new_leaf = alloc_leaf();
    node(new_leaf).leaf = std::move(leaf_data);

    const std::optional<LayoutNodeId> old_parent = parent_of(anchor);
    const tuinator::SplitOrientation orientation =
        direction == PaneSplitDirection::Left || direction == PaneSplitDirection::Right
            ? tuinator::SplitOrientation::Horizontal
            : tuinator::SplitOrientation::Vertical;
    const LayoutNodeId first =
        direction == PaneSplitDirection::Left || direction == PaneSplitDirection::Up ? new_leaf : anchor;
    const LayoutNodeId second =
        direction == PaneSplitDirection::Left || direction == PaneSplitDirection::Up ? anchor : new_leaf;
    const LayoutNodeId container_id = alloc_container(orientation, first, second, 50);

    if (anchor == root_) {
        root_ = container_id;
        parents_.erase(anchor);
    } else if (old_parent.has_value()) {
        replace_child(*old_parent, anchor, container_id);
    } else {
        root_ = container_id;
        parents_.erase(anchor);
    }
    parents_[anchor] = container_id;

    return true;
}

}  // namespace tui_debug_ui
