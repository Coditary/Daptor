#pragma once

#include <tuinator/render/scrollbar.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/widgets/widget.hpp>

#include <memory>
#include <string>

namespace tui_debug_ui {

/// Section header plus scrollable content without a bordered panel frame.
class TitledScrollPane {
  public:
    TitledScrollPane(std::string title, std::unique_ptr<tuinator::Widget> content, tuinator::Style title_style,
                     tuinator::Style background, tuinator::ScrollViewOptions scroll_options, bool scrollable = true);

    void set_title(std::string title);

    tuinator::Widget* content_widget() const { return content_widget_; }
    tuinator::ScrollView* scroll_view() const { return scroll_view_; }

    void refresh_scroll_content();

    std::unique_ptr<tuinator::Widget> release_widget();

  private:
    tuinator::Widget* title_label_ = nullptr;
    tuinator::Widget* content_widget_ = nullptr;
    tuinator::ScrollView* scroll_view_ = nullptr;
    std::unique_ptr<tuinator::Widget> root_;
};

}  // namespace tui_debug_ui
