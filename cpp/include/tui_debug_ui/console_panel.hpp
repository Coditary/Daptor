#pragma once

#include <tuinator/render/style.hpp>
#include <tuinator/widgets/widget.hpp>

#include <functional>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct ConsoleLine;
struct DapUiTheme;

/// Scrollable read-only console output panel (left-aligned lines).
class ConsolePanel : public tuinator::Widget {
  public:
    explicit ConsolePanel(const DapUiTheme& theme);

    const std::vector<std::string>& lines() const { return lines_; }

    void set_text(std::string text);
    void append_lines(std::vector<std::string> lines);

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    bool is_focusable() const override { return true; }
    void collect_focusable(std::vector<tuinator::Widget*>& out) override;
    tuinator::Widget* hit_test(tuinator::Point point) override;
    tuinator::Widget* hit_test_focusable(tuinator::Point point) override;

  private:
    tuinator::Style label_style_;
    tuinator::Style panel_background_;
    tuinator::Style console_stderr_;
    tuinator::Style console_stdout_;
    tuinator::Style console_event_;
    std::vector<std::string> lines_;
};

/// Format stored console entries for display in the panel.
std::vector<std::string> format_console_display_lines(const std::vector<ConsoleLine>& entries);

} // namespace tui_debug_ui
