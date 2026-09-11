#include "tui_debug_ui/context_menu.hpp"

#include <tuinator/render/canvas.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>
#include <utility>

namespace tui_debug_ui {

namespace {

constexpr int kMenuPadding = 1;

}  // namespace

ContextMenu::ContextMenu(tuinator::Style background, tuinator::Style item_style, tuinator::Style selected_style,
                         tuinator::Style border_style)
    : background_(std::move(background)),
      item_style_(std::move(item_style)),
      selected_style_(std::move(selected_style)),
      border_style_(std::move(border_style)) {}

void ContextMenu::open(tuinator::Point anchor, tuinator::Rect clip_bounds, std::vector<Item> items, bool open_above) {
    pending_action_ = nullptr;
    items_ = std::move(items);
    if (items_.empty()) {
        close();
        return;
    }
    anchor_ = anchor;
    clip_bounds_ = clip_bounds;
    open_above_ = open_above;
    selected_ = 0;
    open_ = true;
    clamp_selection();
    mark_dirty();
}

void ContextMenu::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    open_above_ = false;
    items_.clear();
    selected_ = 0;
    mark_dirty();
}

std::function<void()> ContextMenu::take_pending_action() { return std::move(pending_action_); }

int ContextMenu::menu_width() const {
    int width = 12;
    for (const Item& item : items_) {
        width = std::max(width, tuinator::text_display_width(item.label) + kMenuPadding * 2 + 2);
    }
    return width;
}

int ContextMenu::menu_height() const {
    return static_cast<int>(items_.size()) + kMenuPadding * 2;
}

tuinator::Rect ContextMenu::menu_bounds() const {
    if (!open_ || clip_bounds_.width <= 0 || clip_bounds_.height <= 0) {
        return {};
    }

    const int width = menu_width();
    const int height = menu_height();
    int x = open_above_ ? anchor_.x - width / 2 : anchor_.x;
    int y = open_above_ ? anchor_.y - height : anchor_.y;
    if (x + width > clip_bounds_.x + clip_bounds_.width) {
        x = std::max(clip_bounds_.x, clip_bounds_.x + clip_bounds_.width - width);
    }
    if (y < clip_bounds_.y) {
        y = clip_bounds_.y;
    }
    if (y + height > clip_bounds_.y + clip_bounds_.height) {
        y = std::max(clip_bounds_.y, clip_bounds_.y + clip_bounds_.height - height);
    }
    x = std::max(clip_bounds_.x, x);
    y = std::max(clip_bounds_.y, y);
    return {x, y, width, height};
}

int ContextMenu::row_at_position(tuinator::Point position) const {
    const tuinator::Rect menu = menu_bounds();
    if (!menu.contains(position)) {
        return -1;
    }

    const int local_y = position.y - menu.y - kMenuPadding;
    if (local_y < 0 || local_y >= static_cast<int>(items_.size())) {
        return -1;
    }
    return local_y;
}

tuinator::Size ContextMenu::preferred_size() const {
    if (!open_) {
        return {};
    }
    return {menu_width(), menu_height()};
}

void ContextMenu::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    mark_dirty();
}

void ContextMenu::paint(tuinator::PaintContext& ctx) const {
    if (!open_) {
        return;
    }

    const tuinator::Rect menu = menu_bounds();
    if (menu.width <= 0 || menu.height <= 0) {
        return;
    }

    tuinator::Canvas& canvas = ctx.canvas;
    const tuinator::Point origin{menu.x - bounds_.x, menu.y - bounds_.y};
    const tuinator::Size size{menu.width, menu.height};

    canvas.fill_rect({origin, size}, ' ', background_);
    canvas.draw_box({origin, size}, border_style_, ctx.glyphs());

    for (int index = 0; index < static_cast<int>(items_.size()); ++index) {
        const Item& item = items_[static_cast<std::size_t>(index)];
        const tuinator::Style& style =
            index == selected_ && item.enabled ? selected_style_ : (item.enabled ? item_style_ : item_style_);
        const int row = origin.y + kMenuPadding + index;
        const std::size_t bytes =
            tuinator::text_byte_length_for_width(item.label, std::max(0, menu.width - kMenuPadding * 2));
        canvas.draw_text({origin.x + kMenuPadding, row}, item.label.substr(0, bytes), style);
    }
}

void ContextMenu::clamp_selection() {
    if (items_.empty()) {
        selected_ = 0;
        return;
    }
    selected_ = std::clamp(selected_, 0, static_cast<int>(items_.size()) - 1);
    while (selected_ >= 0 && selected_ < static_cast<int>(items_.size()) &&
           !items_[static_cast<std::size_t>(selected_)].enabled) {
        ++selected_;
    }
    if (selected_ >= static_cast<int>(items_.size())) {
        selected_ = 0;
    }
}

void ContextMenu::activate_selected() {
    if (!open_ || selected_ < 0 || selected_ >= static_cast<int>(items_.size())) {
        return;
    }

    const Item& item = items_[static_cast<std::size_t>(selected_)];
    if (!item.enabled || !item.action) {
        return;
    }

    pending_action_ = item.action;
    close();
}

bool ContextMenu::handle_event(const tuinator::Event& event) {
    if (!open_) {
        return false;
    }

    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (key->key == tuinator::Key::Escape) {
            close();
            return true;
        }
        if (key->key == tuinator::Key::Up || key->character == 'k') {
            --selected_;
            clamp_selection();
            mark_dirty();
            return true;
        }
        if (key->key == tuinator::Key::Down || key->character == 'j') {
            ++selected_;
            clamp_selection();
            mark_dirty();
            return true;
        }
        if (key->key == tuinator::Key::Enter) {
            activate_selected();
            return true;
        }
        close();
        return true;
    }

    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        const bool left = mouse->button == tuinator::MouseButton::Left;
        const bool right = mouse->button == tuinator::MouseButton::Right;

        if (mouse->action == tuinator::MouseAction::Move) {
            const int row = row_at_position(mouse->position);
            if (row >= 0) {
                selected_ = row;
                mark_dirty();
            }
            return true;
        }

        const bool pointer_pick = mouse->action == tuinator::MouseAction::Click ||
                                  mouse->action == tuinator::MouseAction::Release;
        if (pointer_pick) {
            const int row = row_at_position(mouse->position);
            if (left) {
                if (row >= 0) {
                    selected_ = row;
                    activate_selected();
                } else {
                    close();
                }
                return true;
            }

            if (right) {
                if (row >= 0) {
                    selected_ = row;
                    activate_selected();
                } else {
                    close();
                }
                return true;
            }

            return true;
        }

        if (mouse->action == tuinator::MouseAction::Press) {
            const int row = row_at_position(mouse->position);
            if (row >= 0) {
                selected_ = row;
                mark_dirty();
            } else if (left) {
                close();
            }
            return true;
        }
    }

    return open_;
}

}  // namespace tui_debug_ui
