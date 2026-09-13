#include "tui_debug_ui/layout_drag.hpp"

#include "tui_debug_ui/layout_tree.hpp"

#include <algorithm>

namespace tui_debug_ui {

LayoutDropZone layout_drop_zone_at(tuinator::Rect bounds, tuinator::Point point) {
    if (!bounds.contains(point)) {
        return LayoutDropZone::Center;
    }

    const int width = bounds.width;
    const int height = bounds.height;
    const int local_x = point.x - bounds.x;
    const int local_y = point.y - bounds.y;

    if (width < 6 || height < 4) {
        return LayoutDropZone::Center;
    }

    const int edge_x = std::max(2, width / 4);
    const int edge_y = std::max(1, height / 4);

    const bool in_center_x = local_x >= edge_x && local_x < width - edge_x;
    const bool in_center_y = local_y >= edge_y && local_y < height - edge_y;
    if (in_center_x && in_center_y) {
        return LayoutDropZone::Center;
    }

    const int dist_left = local_x;
    const int dist_right = width - 1 - local_x;
    const int dist_top = local_y;
    const int dist_bottom = height - 1 - local_y;
    const int min_dist = std::min({dist_left, dist_right, dist_top, dist_bottom});
    if (min_dist == dist_left) {
        return LayoutDropZone::Left;
    }
    if (min_dist == dist_right) {
        return LayoutDropZone::Right;
    }
    if (min_dist == dist_top) {
        return LayoutDropZone::Above;
    }
    return LayoutDropZone::Below;
}

tuinator::Rect layout_drop_outer_shell_rect(tuinator::Rect content, LayoutDropZone zone, int shell_x, int shell_y) {
    switch (zone) {
    case LayoutDropZone::Below:
        return {content.x, content.y + content.height - shell_y, content.width, shell_y};
    case LayoutDropZone::Above:
        return {content.x, content.y, content.width, shell_y};
    case LayoutDropZone::Right:
        return {content.x + content.width - shell_x, content.y, shell_x, content.height};
    case LayoutDropZone::Left:
        return {content.x, content.y, shell_x, content.height};
    case LayoutDropZone::Center:
        break;
    }
    return content;
}

tuinator::Rect layout_drop_span_rect(tuinator::Rect bounds, LayoutDropZone zone) {
    const int edge_x = std::max(2, bounds.width / 4);
    const int edge_y = std::max(2, bounds.height / 4);

    switch (zone) {
    case LayoutDropZone::Center:
        return {bounds.x + edge_x, bounds.y + edge_y, bounds.width - 2 * edge_x, bounds.height - 2 * edge_y};
    case LayoutDropZone::Left:
        return {bounds.x, bounds.y, edge_x, bounds.height};
    case LayoutDropZone::Right:
        return {bounds.x + bounds.width - edge_x, bounds.y, edge_x, bounds.height};
    case LayoutDropZone::Above:
        return {bounds.x, bounds.y, bounds.width, edge_y};
    case LayoutDropZone::Below:
        return {bounds.x, bounds.y + bounds.height - edge_y, bounds.width, edge_y};
    }
    return bounds;
}

tuinator::Rect layout_drop_zone_rect(tuinator::Rect bounds, LayoutDropZone zone) {
    const int edge_x = std::max(2, bounds.width / 4);
    const int edge_y = std::max(1, bounds.height / 4);

    switch (zone) {
    case LayoutDropZone::Center:
        return {bounds.x + edge_x, bounds.y + edge_y, bounds.width - 2 * edge_x, bounds.height - 2 * edge_y};
    case LayoutDropZone::Left:
        return {bounds.x, bounds.y, edge_x, bounds.height};
    case LayoutDropZone::Right:
        return {bounds.x + bounds.width - edge_x, bounds.y, edge_x, bounds.height};
    case LayoutDropZone::Above:
        return {bounds.x, bounds.y, bounds.width, edge_y};
    case LayoutDropZone::Below:
        return {bounds.x, bounds.y + bounds.height - edge_y, bounds.width, edge_y};
    }
    return bounds;
}

PaneSplitDirection layout_drop_zone_to_split(LayoutDropZone zone) {
    switch (zone) {
    case LayoutDropZone::Above:
        return PaneSplitDirection::Up;
    case LayoutDropZone::Below:
        return PaneSplitDirection::Down;
    case LayoutDropZone::Left:
        return PaneSplitDirection::Left;
    case LayoutDropZone::Center:
    case LayoutDropZone::Right:
        return PaneSplitDirection::Right;
    }
    return PaneSplitDirection::Right;
}

}  // namespace tui_debug_ui
