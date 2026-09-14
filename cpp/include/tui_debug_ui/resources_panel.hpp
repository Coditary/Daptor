#pragma once

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/process_metrics.hpp"

#include <tuinator/render/style.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class Widget;
}

namespace tui_debug_ui {

class TitledScrollPane;

/// Live CPU/RAM panel for the debuggee (DAP `systemProcessId` PIDs, aggregated).
class ResourcesPanel {
  public:
    ResourcesPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                   const std::string& title = "Resources");

    std::unique_ptr<tuinator::Widget> release_widget();
    void set_title(std::string title);
    void set_tracked_pids(const std::vector<std::uint32_t>& pids);
    void set_metrics(const ProcessMetricsSample& sample, const ProcessMetricsSampler& sampler);

  private:
    tuinator::Style title_style_;
    std::unique_ptr<TitledScrollPane> pane_;
    tuinator::Widget* content_ = nullptr;
    std::vector<std::uint32_t> tracked_pids_;
};

}  // namespace tui_debug_ui
