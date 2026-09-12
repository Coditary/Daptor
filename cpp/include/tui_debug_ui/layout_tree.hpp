#pragma once

#include "tui_debug_ui/panel_slot.hpp"
#include "tui_debug_ui/sidebar_slot.hpp"

#include <tuinator/widgets/containers/split_pane.hpp>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace tui_debug_ui {

using LayoutNodeId = std::uint64_t;

enum class PaneSplitDirection { Left, Right, Up, Down };

struct LayoutLeafData {
    std::vector<SidebarSlot> slots;
    int active_index = 0;
    /// Set for the three default dock regions; unset for panes created by user splits.
    std::optional<PanelDock> dock;
};

struct LayoutSplitData {
    tuinator::SplitOrientation orientation = tuinator::SplitOrientation::Horizontal;
    LayoutNodeId first = 0;
    LayoutNodeId second = 0;
    std::uint16_t first_pct = 50;
};

struct LayoutNode {
    enum class Kind { Leaf, Split } kind = Kind::Leaf;
    LayoutLeafData leaf;
    LayoutSplitData split;
};

/// Binary split tree of panel leaves.
class LayoutTree {
  public:
    [[nodiscard]] bool empty() const { return nodes_.empty(); }
    [[nodiscard]] LayoutNodeId root() const { return root_; }

    [[nodiscard]] LayoutNode& node(LayoutNodeId id);
    [[nodiscard]] const LayoutNode& node(LayoutNodeId id) const;
    [[nodiscard]] std::vector<LayoutNodeId> leaf_ids() const;
    [[nodiscard]] int leaf_count() const;
    [[nodiscard]] std::optional<LayoutNodeId> parent_of(LayoutNodeId id) const;
    [[nodiscard]] bool is_only_leaf(LayoutNodeId id) const;
    [[nodiscard]] std::optional<LayoutNodeId> find_leaf_for_dock(PanelDock dock) const;
    [[nodiscard]] bool subtree_contains_dock(LayoutNodeId node_id, PanelDock dock) const;

    void init_default_three_pane(std::uint16_t sidebar_pct, std::uint16_t bottom_pct);
    [[nodiscard]] LayoutNodeId split_leaf(LayoutNodeId leaf_id, PaneSplitDirection direction);
    bool delete_leaf(LayoutNodeId leaf_id);

  private:
    [[nodiscard]] LayoutNodeId alloc_leaf();
    [[nodiscard]] LayoutNodeId alloc_split(tuinator::SplitOrientation orientation, LayoutNodeId first,
                                           LayoutNodeId second, std::uint16_t first_pct);
    void replace_child(LayoutNodeId parent, LayoutNodeId old_child, LayoutNodeId new_child);

    LayoutNodeId root_ = 0;
    LayoutNodeId next_id_ = 1;
    std::unordered_map<LayoutNodeId, LayoutNode> nodes_;
    std::unordered_map<LayoutNodeId, LayoutNodeId> parents_;
};

}  // namespace tui_debug_ui
