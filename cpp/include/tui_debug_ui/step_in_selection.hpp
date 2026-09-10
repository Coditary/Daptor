#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct StepInTargetSpan {
    int start_column = 0;
    int end_column = 0;
    std::string label;
    std::int64_t target_id = -1;
};

struct StepInSelectionState {
    int line = 0;
    std::vector<StepInTargetSpan> targets;
    int active_index = 0;

    [[nodiscard]] bool active() const { return !targets.empty(); }

    [[nodiscard]] const StepInTargetSpan* active_target() const {
        if (!active() || active_index < 0 ||
            active_index >= static_cast<int>(targets.size())) {
            return nullptr;
        }
        return &targets[static_cast<std::size_t>(active_index)];
    }
};

/// Find callable `name(` spans on a source line (fallback when DAP has no columns).
std::vector<StepInTargetSpan> find_step_in_targets_on_line(const std::string& line_text);

/// Merge DAP step-in targets with source-line spans for highlighting.
std::vector<StepInTargetSpan> build_step_in_target_spans(const std::string& line_text,
                                                         const std::string& targets_json,
                                                         std::string& error_out);

}  // namespace tui_debug_ui
