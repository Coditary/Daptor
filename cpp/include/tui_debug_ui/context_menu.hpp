#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/core/geometry.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/widget.hpp>

#include <functional>
#include <string>
#include <vector>

namespace tui_debug_ui {

/// Small floating action list for right-click menus.
class ContextMenu : public tuinator::Widget {
  public:
    struct Item {
        std::string label;
        std::function<void()> action;
        bool enabled = true;
    };

    explicit ContextMenu(tuinator::Style background, tuinator::Style item_style, tuinator::Style selected_style,
                         tuinator::Style border_style);

    [[nodiscard]] bool is_open() const { return open_; }

    void open(tuinator::Point anchor, tuinator::Rect clip_bounds, std::vector<Item> items, bool open_above = false);
    void close();
    void set_on_selection_changed(std::function<void(int index)> callback);
    [[nodiscard]] int selected_index() const { return selected_; }
    /// Action queued by activate_selected(); run after handle_event() returns.
    std::function<void()> take_pending_action();

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    bool is_focusable() const override { return open_; }

  private:
    void clamp_selection();
    void activate_selected();
    [[nodiscard]] int row_at_position(tuinator::Point position) const;
    [[nodiscard]] int menu_width() const;
    [[nodiscard]] int menu_height() const;
    [[nodiscard]] tuinator::Rect menu_bounds() const;

    tuinator::Style background_;
    tuinator::Style item_style_;
    tuinator::Style selected_style_;
    tuinator::Style border_style_;
    tuinator::Rect clip_bounds_{};
    tuinator::Point anchor_{};
    std::vector<Item> items_;
    std::function<void()> pending_action_;
    std::function<void(int index)> on_selection_changed_;
    int selected_ = 0;
    bool open_ = false;
    bool open_above_ = false;
};

}  // namespace tui_debug_ui
