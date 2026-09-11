#include "tui_debug_ui/titled_scroll_pane.hpp"

#include "tui_debug_ui/background_widget.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/layout/box.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/widgets/display/label.hpp>
#include <tuinator/widgets/widget.hpp>

#include <utility>

namespace tui_debug_ui {

namespace {

class PanelTitleBar : public tuinator::Widget {
  public:
    PanelTitleBar(std::string title, tuinator::Style title_style, std::string action_symbol, tuinator::Style action_style,
                  std::function<void()> on_action)
        : title_(std::move(title)),
          title_style_(std::move(title_style)),
          action_symbol_(std::move(action_symbol)),
          action_style_(std::move(action_style)),
          on_action_(std::move(on_action)) {}

    void set_title(std::string title) {
        title_ = std::move(title);
        mark_dirty();
    }

    void set_action(std::string symbol, tuinator::Style style, std::function<void()> on_action) {
        action_symbol_ = std::move(symbol);
        action_style_ = std::move(style);
        on_action_ = std::move(on_action);
        mark_dirty();
    }

    tuinator::Size preferred_size() const override {
        const int title_width = tuinator::text_display_width(title_);
        const int action_width = action_symbol_.empty() ? 0 : tuinator::text_display_width(action_symbol_);
        return {title_width + action_width, 1};
    }

    void layout(tuinator::Rect bounds) override { bounds_ = bounds; }

    void paint(tuinator::PaintContext& ctx) const override {
        if (bounds_.width <= 0 || bounds_.height <= 0) {
            return;
        }

        tuinator::Canvas& canvas = ctx.canvas;
        const std::size_t title_bytes =
            tuinator::text_byte_length_for_width(title_, std::max(0, bounds_.width));
        canvas.draw_text({0, 0}, title_.substr(0, title_bytes), title_style_);

        if (!action_symbol_.empty()) {
            const int action_width = tuinator::text_display_width(action_symbol_);
            const int action_x = std::max(0, bounds_.width - action_width);
            canvas.draw_text({action_x, 0}, action_symbol_, action_style_);
        }
    }

    bool handle_event(const tuinator::Event& event) override {
        if (action_symbol_.empty() || !on_action_) {
            return false;
        }

        if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
            if (mouse->action != tuinator::MouseAction::Click && mouse->action != tuinator::MouseAction::Release) {
                return false;
            }
            if (!bounds_.contains(mouse->position)) {
                return false;
            }

            const tuinator::Point local{mouse->position.x - bounds_.x, mouse->position.y - bounds_.y};
            const int action_width = tuinator::text_display_width(action_symbol_);
            const int action_x = std::max(0, bounds_.width - action_width);
            if (local.x >= action_x) {
                on_action_();
                return true;
            }
        }

        return false;
    }

  private:
    std::string title_;
    tuinator::Style title_style_;
    std::string action_symbol_;
    tuinator::Style action_style_;
    std::function<void()> on_action_;
};

}  // namespace

TitledScrollPane::TitledScrollPane(std::string title, std::unique_ptr<tuinator::Widget> content,
                                   tuinator::Style title_style, tuinator::Style background,
                                   tuinator::ScrollViewOptions scroll_options, bool scrollable,
                                   std::unique_ptr<tuinator::Widget> header,
                                   std::unique_ptr<tuinator::Widget> footer) {
    auto column = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});
    column->set_flex(1);

    auto label = std::make_unique<PanelTitleBar>(std::move(title), title_style, "", title_style, nullptr);
    title_label_ = label.get();
    column->add_child(std::move(label));

    if (header != nullptr) {
        header->set_flex(0);
        column->add_child(std::move(header));
    }

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

    if (footer != nullptr) {
        footer->set_flex(0);
        column->add_child(std::move(footer));
    }

    root_ = std::make_unique<BackgroundWidget>(std::move(column), std::move(background));
    root_->set_flex(1);
}

void TitledScrollPane::set_title(std::string title) {
    if (auto* bar = dynamic_cast<PanelTitleBar*>(title_label_)) {
        bar->set_title(std::move(title));
    }
}

void TitledScrollPane::set_title_action(std::string symbol, tuinator::Style style, std::function<void()> callback) {
    if (auto* bar = dynamic_cast<PanelTitleBar*>(title_label_)) {
        bar->set_action(std::move(symbol), std::move(style), std::move(callback));
    }
}

void TitledScrollPane::refresh_scroll_content() {
    if (scroll_view_ != nullptr) {
        scroll_view_->refresh_content();
    }
}

std::unique_ptr<tuinator::Widget> TitledScrollPane::release_widget() { return std::move(root_); }

}  // namespace tui_debug_ui
