#include "tui_debug_ui/stacks_panel.hpp"

#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <string>
#include <utility>

namespace tui_debug_ui {

StacksPanel::StacksPanel(tuinator::Style title_style, tuinator::Style item_style, tuinator::Style row_background,
                         tuinator::ScrollViewOptions scroll_options, const std::string& title) {
    auto list = std::make_unique<NavigableListView>(item_style, item_style, row_background, false);
    list_ = list.get();

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), title_style, row_background,
                                               std::move(scroll_options));
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> StacksPanel::release_widget() { return pane_->release_widget(); }

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
    list_->assign_items(std::move(items));
}

void StacksPanel::set_lines(std::vector<std::string> lines) {
    if (list_ != nullptr) {
        list_->assign_items(std::move(lines));
    }
}

tuinator::Widget* StacksPanel::list_widget() const { return list_; }

tuinator::ScrollView* StacksPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
