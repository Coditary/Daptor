#pragma once

#include <tuinator/render/style.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class ListView;
class Panel;
class Widget;
} // namespace tuinator

namespace tui_debug_ui {

struct StackFrameRow {
    std::string name;
    std::uint32_t line = 0;
    std::string path;
};

/// Stack frames list wrapped in a titled panel.
class StacksPanel {
  public:
    StacksPanel(tuinator::Style border_style, tuinator::Style title_style, tuinator::Style item_style,
                tuinator::Style selected_style, tuinator::Style row_background,
                const std::string& title = "MainThread");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_frames(std::vector<StackFrameRow> frames);
    void set_lines(std::vector<std::string> lines);
    tuinator::Widget* list_widget() const;

  private:
    std::unique_ptr<tuinator::Widget> panel_;
    tuinator::ListView* list_ = nullptr;
};

} // namespace tui_debug_ui
