#pragma once

#include "tui_debug_ui/layout_tree.hpp"

#include <tuinator/core/geometry.hpp>

namespace tui_debug_ui {

enum class LayoutDragSourceKind { Pane, Tab };

enum class LayoutDropZone { Center, Above, Below, Left, Right };

struct LayoutDropTarget {
    LayoutNodeId anchor = 0;
    LayoutNodeId hover_leaf = 0;
    LayoutDropZone zone = LayoutDropZone::Center;
    tuinator::Rect highlight{};
    bool spans_siblings = false;
};

struct LayoutPlacementOption {
    std::string label;
    LayoutDropTarget target{};
    bool swap = false;
};

[[nodiscard]] LayoutDropZone layout_drop_zone_at(tuinator::Rect bounds, tuinator::Point point);

[[nodiscard]] tuinator::Rect layout_drop_zone_rect(tuinator::Rect bounds, LayoutDropZone zone);

[[nodiscard]] tuinator::Rect layout_drop_span_rect(tuinator::Rect bounds, LayoutDropZone zone);

[[nodiscard]] tuinator::Rect layout_drop_outer_shell_rect(tuinator::Rect content, LayoutDropZone zone, int shell_x,
                                                          int shell_y);

[[nodiscard]] PaneSplitDirection layout_drop_zone_to_split(LayoutDropZone zone);

}  // namespace tui_debug_ui
