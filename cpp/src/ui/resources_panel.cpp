#include "tui_debug_ui/resources_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/widgets/widget.hpp>

#include <algorithm>
#include <cmath>
#include <sstream>
#include <utility>
#include <vector>

namespace tui_debug_ui {
namespace {

constexpr int kHistoryRows = 2;

double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

void paint_bar(tuinator::Canvas& canvas, int x, int y, int width, double fraction, tuinator::Style fill,
               tuinator::Style track) {
    if (width <= 0 || y < 0) {
        return;
    }
    fraction = clamp01(fraction);
    canvas.fill_rect({x, y, width, 1}, ' ', track);
    const int filled = static_cast<int>(std::lround(fraction * static_cast<double>(width)));
    if (filled > 0) {
        canvas.fill_rect({x, y, std::min(filled, width), 1}, ' ', fill);
    }
}

void paint_history_columns(tuinator::Canvas& canvas, int x, int y, int width, int height,
                           const std::deque<double>& history, double scale_max, tuinator::Style fill,
                           tuinator::Style track) {
    if (width <= 0 || height <= 0 || history.empty()) {
        return;
    }
    const double max_value = std::max(1.0, scale_max);
    canvas.fill_rect({x, y, width, height}, ' ', track);
    const int count = static_cast<int>(history.size());
    for (int column = 0; column < width; ++column) {
        const int index =
            count <= width ? column : ((column * (count - 1)) / std::max(1, width - 1));
        const double value =
            history[static_cast<std::size_t>(std::clamp(index, 0, count - 1))];
        const int bar_height =
            std::max(1, static_cast<int>(std::lround(clamp01(value / max_value) * static_cast<double>(height))));
        canvas.fill_rect({x + column, y + height - bar_height, 1, bar_height}, ' ', fill);
    }
}

class ResourcesContentWidget : public tuinator::Widget {
  public:
    explicit ResourcesContentWidget(const DapUiTheme& theme) : theme_(theme) {
        bar_track_ = theme.row_action_muted;
        mem_fill_ = tuinator::style_fg_bg(tuinator::Rgb{120, 190, 255}, theme.background);
        cpu_fill_ = tuinator::style_fg_bg(tuinator::Rgb{120, 220, 140}, theme.background);
        debug_fill_ = tuinator::style_fg_bg(tuinator::Rgb{220, 170, 110}, theme.background);
    }

    void set_state(std::vector<std::uint32_t> pids, ProcessMetricsSample sample,
                   const ProcessMetricsSampler& sampler) {
        pids_ = std::move(pids);
        sample_ = sample;
        sampler_ = &sampler;
        mark_dirty();
    }

    tuinator::Size preferred_size() const override { return {40, 13}; }

    void layout(tuinator::Rect bounds) override { bounds_ = bounds; }

    void paint(tuinator::PaintContext& ctx) const override {
        tuinator::Canvas& canvas = ctx.canvas;
        canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', theme_.panel_background);

        const int width = std::max(12, bounds_.width - 2);
        int row = 0;
        const auto draw_line = [&](std::string_view text, tuinator::Style style) {
            if (row >= bounds_.height) {
                return;
            }
            const std::size_t bytes = tuinator::text_byte_length_for_width(text, width);
            canvas.draw_text({1, row}, text.substr(0, bytes), style);
            ++row;
        };
        const auto draw_bar_row = [&](double fraction, tuinator::Style fill) {
            if (row >= bounds_.height) {
                return;
            }
            paint_bar(canvas, 1, row, width, fraction, fill, bar_track_);
            ++row;
        };
        const auto draw_history_row = [&](const std::deque<double>& history, double scale_max,
                                          tuinator::Style fill) {
            if (row + kHistoryRows > bounds_.height || history.empty()) {
                return;
            }
            paint_history_columns(canvas, 1, row, width, kHistoryRows, history, scale_max, fill, bar_track_);
            row += kHistoryRows;
        };

        draw_line("System", theme_.scope_header);
        if (sample_.system_mem_total_bytes > 0) {
            const double mem_fraction = static_cast<double>(sample_.system_mem_used_bytes) /
                                        static_cast<double>(sample_.system_mem_total_bytes);
            draw_line("RAM  " + format_byte_size(sample_.system_mem_used_bytes) + " / " +
                          format_byte_size(sample_.system_mem_total_bytes) + "  (" +
                          format_percent(mem_fraction * 100.0) + ")",
                      theme_.label);
            draw_bar_row(mem_fraction, mem_fill_);
        } else {
            draw_line("RAM  (unavailable)", theme_.label);
        }
        draw_line("CPU  " + format_percent(sample_.system_cpu_percent), theme_.label);
        draw_bar_row(clamp01(sample_.system_cpu_percent / 100.0), cpu_fill_);
        if (sampler_ != nullptr) {
            draw_history_row(sampler_->system_cpu_history(), 100.0, cpu_fill_);
        }

        draw_line("Program", theme_.scope_globals_header);

        if (pids_.empty()) {
            draw_line("Waiting for DAP process event…", theme_.label);
            return;
        }

        std::ostringstream pid_line;
        pid_line << "PIDs ";
        for (std::size_t i = 0; i < pids_.size(); ++i) {
            if (i > 0) {
                pid_line << ", ";
            }
            pid_line << pids_[i];
        }
        if (pids_.size() > 1) {
            pid_line << " (aggregated)";
        }
        draw_line(pid_line.str(), theme_.label);

        if (sample_.tracked_pids == 0) {
            draw_line("Process exited or unavailable", theme_.console_stderr);
            return;
        }

        if (sample_.system_mem_total_bytes > 0) {
            const double mem_fraction = static_cast<double>(sample_.debuggee_mem_bytes) /
                                        static_cast<double>(sample_.system_mem_total_bytes);
            draw_line("RAM  " + format_byte_size(sample_.debuggee_mem_bytes) + "  (" +
                          format_percent(mem_fraction * 100.0) + " of system)",
                      theme_.label);
            draw_bar_row(mem_fraction, debug_fill_);
        } else {
            draw_line("RAM  " + format_byte_size(sample_.debuggee_mem_bytes), theme_.label);
        }
        draw_line("CPU  " + format_percent(sample_.debuggee_cpu_percent), theme_.label);
        draw_bar_row(clamp01(sample_.debuggee_cpu_percent / 100.0), debug_fill_);
        if (sampler_ != nullptr) {
            draw_history_row(sampler_->debuggee_cpu_history(), 100.0, debug_fill_);
        }
    }

  private:
    const DapUiTheme& theme_;
    tuinator::Style bar_track_;
    tuinator::Style mem_fill_;
    tuinator::Style cpu_fill_;
    tuinator::Style debug_fill_;
    std::vector<std::uint32_t> pids_;
    ProcessMetricsSample sample_{};
    const ProcessMetricsSampler* sampler_ = nullptr;
};

}  // namespace

ResourcesPanel::ResourcesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                               const std::string& title)
    : title_style_(theme.title_watches) {
    auto content = std::make_unique<ResourcesContentWidget>(theme);
    content_ = static_cast<tuinator::Widget*>(content.get());
    pane_ = std::make_unique<TitledScrollPane>(title, std::move(content), title_style_, theme.panel_background,
                                               std::move(scroll_options), false, true);
}

std::unique_ptr<tuinator::Widget> ResourcesPanel::release_widget() { return pane_->release_widget(); }

void ResourcesPanel::set_title(std::string title) { pane_->set_title(std::move(title)); }

void ResourcesPanel::set_tracked_pids(const std::vector<std::uint32_t>& pids) {
    tracked_pids_ = pids;
}

void ResourcesPanel::set_metrics(const ProcessMetricsSample& sample, const ProcessMetricsSampler& sampler) {
    if (auto* widget = dynamic_cast<ResourcesContentWidget*>(content_)) {
        widget->set_state(tracked_pids_, sample, sampler);
    }
}

}  // namespace tui_debug_ui
