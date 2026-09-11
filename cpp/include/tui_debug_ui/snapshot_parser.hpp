#pragma once

#include "tui_debug_ui/debug_ui_model.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct BreakpointHitUpdate {
    std::string path;
    int line = 0;
    std::uint64_t hit_count = 0;
};

/// Parse poll/sync JSON envelopes into `model`.
///
/// Accepts `{"type":"stopped","snapshot":{...}}` and `{"type":"snapshot","snapshot":{...}}`.
/// Returns `true` when a snapshot was applied, `false` on parse errors or missing snapshot.
bool apply_poll_json(DebugUiModel& model, const std::string& json);

/// Append console output entries from a JSON array: `[{"category":"stdout","text":"..."}]`.
/// Returns `true` when parsed successfully (including an empty array).
bool apply_console_json(DebugUiModel& model, const std::string& json);

/// Parse a DAP variables array: `[{"name":"x","value":"1","variablesReference":0}]`.
std::vector<VariableInfo> parse_variables_json(const std::string& json);

/// Apply scope-variable batch JSON from the session worker:
/// `{"signature":"...","variables":{"3":[...],"4":[...]}}`.
bool apply_scope_variables_batch(DebugUiModel& model, const std::string& signature, const std::string& json);

/// Parse `breakpoint_hits` from a poll/sync JSON envelope.
std::vector<BreakpointHitUpdate> parse_breakpoint_hits_from_poll_json(const std::string& json);

}  // namespace tui_debug_ui
