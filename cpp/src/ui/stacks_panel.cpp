#include "tui_debug_ui/stacks_panel.hpp"

#include "tui_debug_ui/background_widget.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"

#include <tuinator/tuinator.hpp>

#include <string>
#include <utility>

namespace tui_debug_ui {

StacksPanel::StacksPanel(tuinator::Style border_style, tuinator::Style title_style, tuinator::Style item_style,
                         tuinator::Style selected_style, tuinator::Style row_background, const std::string& title) {
    auto list = std::make_unique<NavigableListView>(item_style, selected_style, row_background);
    list_ = list.get();

    auto panel = std::make_unique<tuinator::Panel>(title, border_style, title_style);
    panel->set_content(std::move(list));
    panel_ = std::make_unique<BackgroundWidget>(std::move(panel), row_background);
}

std::unique_ptr<tuinator::Widget> StacksPanel::release_widget() { return std::move(panel_); }

void StacksPanel::set_frames(std::vector<StackFrameRow> frames) {
    if (list_ == nullptr) {
        return;
    }

    std::vector<std::string> items;
    items.reserve(frames.size());
    for (std::size_t index = 0; index < frames.size(); ++index) {
        const StackFrameRow& frame = frames[index];
        const std::string marker = index == 0 ? "\u{eaf0} " : "  ";
        const std::string path = frame.path.empty() ? "<unknown>" : frame.path;
        items.push_back(marker + "#" + std::to_string(index) + " " + frame.name + " @ " + path + ":" +
                        std::to_string(frame.line));
    }
    list_->set_items(std::move(items));
}

void StacksPanel::set_lines(std::vector<std::string> lines) {
    if (list_ != nullptr) {
        list_->set_items(std::move(lines));
    }
}

tuinator::Widget* StacksPanel::list_widget() const { return list_; }

} // namespace tui_debug_ui
