#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/views/list_view.hpp>

#include <variant>

namespace tui_debug_ui {

/// ListView that also accepts j/k for selection (vim-style navigation).
class NavigableListView : public tuinator::ListView {
  public:
    NavigableListView(tuinator::Style item_style, tuinator::Style selected_style,
                      tuinator::Style row_background = {});

    void paint(tuinator::PaintContext& ctx) const override;

    bool handle_event(const tuinator::Event& event) override {
        if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
            if (is_focused()) {
                if (key->character == 'j') {
                    tuinator::KeyPress down{tuinator::Key::Down, '\0'};
                    return tuinator::ListView::handle_event(down);
                }
                if (key->character == 'k') {
                    tuinator::KeyPress up{tuinator::Key::Up, '\0'};
                    return tuinator::ListView::handle_event(up);
                }
            }
        }
        return tuinator::ListView::handle_event(event);
    }

  private:
    tuinator::Style row_background_;
};

} // namespace tui_debug_ui
