#include "tui_debug_ui/titled_scroll_pane.hpp"

#include "tui_debug_ui/background_widget.hpp"

#include <tuinator/layout/box.hpp>
#include <tuinator/widgets/display/label.hpp>

namespace tui_debug_ui {

TitledScrollPane::TitledScrollPane(std::string title, std::unique_ptr<tuinator::Widget> content,
                                   tuinator::Style title_style, tuinator::Style background,
                                   tuinator::ScrollViewOptions scroll_options, bool scrollable) {
    auto column = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});
    column->set_flex(1);

    auto label = std::make_unique<tuinator::Label>(std::move(title), title_style);
    title_label_ = label.get();
    column->add_child(std::move(label));

    if (scrollable) {
        content_widget_ = content.get();
        auto scroll = std::make_unique<tuinator::ScrollView>(std::move(content), std::move(scroll_options));
        scroll_view_ = scroll.get();
        scroll->set_flex(1);
        column->add_child(std::move(scroll));
    } else {
        content_widget_ = content.get();
        content->set_flex(1);
        column->add_child(std::move(content));
    }

    root_ = std::make_unique<BackgroundWidget>(std::move(column), std::move(background));
    root_->set_flex(1);
}

void TitledScrollPane::set_title(std::string title) {
    if (auto* label = dynamic_cast<tuinator::Label*>(title_label_)) {
        label->set_text(std::move(title));
    }
}

void TitledScrollPane::refresh_scroll_content() {
    if (scroll_view_ != nullptr) {
        scroll_view_->refresh_content();
    }
}

std::unique_ptr<tuinator::Widget> TitledScrollPane::release_widget() { return std::move(root_); }

}  // namespace tui_debug_ui
