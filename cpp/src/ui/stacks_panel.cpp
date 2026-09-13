#include "tui_debug_ui/stacks_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/render/text.hpp>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

namespace tui_debug_ui {

namespace {

std::string basename_from_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::string frame_location_label(const StackFrameRow& frame) {
    if (frame.path.empty()) {
        return "<unknown>:" + std::to_string(frame.line);
    }
    if (frame.path.rfind("dap:source:", 0) == 0) {
        return frame.path + ":" + std::to_string(frame.line);
    }
    return basename_from_path(frame.path) + ":" + std::to_string(frame.line);
}

}  // namespace

StacksPanel::StacksPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                         const std::string& title) {
    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::Stacks, &theme);
    list_->set_on_activate([this](int index) {
        const StackFrameRow* frame = frame_at_display_index(index);
        if (frame != nullptr && on_activate_ != nullptr) {
            on_activate_(*frame);
        }
    });
    list_->set_on_row_click([this](int /*index*/, const std::string& item, int local_x) {
        constexpr const char* kContinueMarker = "\u25b6 ";
        if (item.rfind(kContinueMarker, 0) != 0 || on_continue_ == nullptr) {
            return false;
        }
        const int marker_width = tuinator::text_display_width(kContinueMarker);
        if (local_x < 0 || local_x >= marker_width) {
            return false;
        }
        on_continue_();
        return true;
    });
    list_->set_on_row_context([this](int index, const std::string& /*item*/, tuinator::Point anchor) {
        const StackFrameRow* frame = frame_at_display_index(index);
        if (frame != nullptr && on_context_ != nullptr) {
            on_context_(*frame, anchor);
        }
    });

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), theme.title_stacks, theme.panel_background,
                                               std::move(scroll_options), true, false);
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> StacksPanel::release_widget() { return pane_->release_widget(); }

void StacksPanel::set_thread_stacks(std::vector<ThreadStackContent> threads) {
    if (list_ == nullptr) {
        return;
    }

    std::sort(threads.begin(), threads.end(), [](const ThreadStackContent& left, const ThreadStackContent& right) {
        if (left.stopped != right.stopped) {
            return left.stopped;
        }
        return left.id < right.id;
    });

    frames_.clear();
    display_to_frame_.clear();
    std::vector<std::string> items;
    std::unordered_set<std::string> stopped_headers;

    if (threads.empty()) {
        items.push_back("No threads");
        list_->set_stopped_thread_headers({});
        list_->assign_items(std::move(items));
        return;
    }

    for (std::size_t thread_index = 0; thread_index < threads.size(); ++thread_index) {
        const ThreadStackContent& thread = threads[thread_index];
        const std::string header = thread.name + ":";
        items.push_back(header);
        display_to_frame_.push_back(-1);
        if (thread.stopped) {
            stopped_headers.insert(header);
        }

        for (std::size_t frame_index = 0; frame_index < thread.frames.size(); ++frame_index) {
            const StackFrameRow& frame = thread.frames[frame_index];
            const bool current_frame = thread.stopped && frame_index == 0;
            const std::string marker = current_frame ? "\u25b6 " : "  ";
            items.push_back(marker + frame.name + " " + frame_location_label(frame));
            frames_.push_back(frame);
            display_to_frame_.push_back(static_cast<int>(frames_.size()) - 1);
        }

        if (thread_index + 1 < threads.size()) {
            items.push_back("");
            display_to_frame_.push_back(-1);
        }
    }

    list_->set_stopped_thread_headers(std::move(stopped_headers));
    if (list_->items() != items) {
        list_->assign_items(std::move(items));
    }
}

void StacksPanel::set_lines(std::vector<std::string> lines) {
    if (list_ != nullptr) {
        list_->set_stopped_thread_headers({});
        list_->assign_items(std::move(lines));
    }
}

void StacksPanel::set_on_activate(ActivateCallback callback) { on_activate_ = std::move(callback); }

void StacksPanel::set_on_continue(std::function<void()> callback) { on_continue_ = std::move(callback); }

void StacksPanel::set_on_context(ContextCallback callback) { on_context_ = std::move(callback); }

const StackFrameRow* StacksPanel::frame_at_display_index(int index) const {
    if (index < 0 || index >= static_cast<int>(display_to_frame_.size())) {
        return nullptr;
    }
    const int frame_index = display_to_frame_[static_cast<std::size_t>(index)];
    if (frame_index < 0 || frame_index >= static_cast<int>(frames_.size())) {
        return nullptr;
    }
    return &frames_[static_cast<std::size_t>(frame_index)];
}

tuinator::Widget* StacksPanel::list_widget() const { return list_; }

tuinator::ScrollView* StacksPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
