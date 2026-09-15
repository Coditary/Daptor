#include "tui_debug_ui/app.hpp"

#include "tui_debug_ui/background_widget.hpp"
#include "tui_debug_ui/console_panel.hpp"
#include "tui_debug_ui/controls_bar.hpp"
#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/layout_config.hpp"
#include "tui_debug_ui/theme_loader.hpp"
#include "tui_debug_ui/highlight_bridge.hpp"
#include "tui_debug_ui/resizable_split_pane.hpp"
#include "tui_debug_ui/panel_slot.hpp"
#include "tui_debug_ui/shared_widget_host.hpp"
#include "tui_debug_ui/stacked_pane.hpp"
#include "tui_debug_ui/scopes_panel.hpp"
#include "tui_debug_ui/snapshot_parser.hpp"
#include "tui_debug_ui/source_panel.hpp"
#include "tui_debug_ui/source_tab_bar.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/breakpoints_panel.hpp"
#include "tui_debug_ui/context_menu.hpp"
#include "tui_debug_ui/file_picker.hpp"
#include "tui_debug_ui/file_tree_panel.hpp"
#include "tui_debug_ui/workspace_files.hpp"
#include "tui_debug_ui/stacks_panel.hpp"
#include "tui_debug_ui/watches_panel.hpp"
#include "tui_debug_ui/memory_panel.hpp"
#include "tui_debug_ui/disassembly_panel.hpp"
#include "tui_debug_ui/runtime_source_panel.hpp"
#include "tui_debug_ui/resources_panel.hpp"
#include "tui_debug_ui/network_mock_data.hpp"
#include "tui_debug_ui/network_panel.hpp"
#include "tui_debug_ui/dap_view_formatters.hpp"
#include "tui_debug_ui/repl_panel.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"
#include "tui_debug_ui/tty_setup.hpp"

#include <tuinator/backend/terminal_backend.hpp>
#include <tuinator/render/canvas.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/tuinator.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define TUI_DEBUG_UI_HAS_NLOHMANN_JSON 1
#endif

namespace tui_debug_ui {

class DebugApp;

} // namespace tui_debug_ui

namespace {

constexpr int kControlsBarRows = 1;
constexpr int kStatusBarRows = 1;
constexpr int kSplitDividerRows = 1;
constexpr int kFullFileSourceLineThreshold = 500;
constexpr int kHighlightLineMargin = 12;
constexpr int kMaxHighlightLinesPerRequest = 128;
constexpr auto kReplCompletionFetchDelay = std::chrono::milliseconds(200);
constexpr auto kReplCompletionShowDelay = std::chrono::milliseconds(500);

bool repl_completion_ident_char(char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) || ch == '_';
}

std::pair<std::size_t, std::size_t> repl_completion_token_range(const std::string& text, std::size_t cursor) {
    const std::size_t bounded_cursor = std::min(cursor, text.size());
    std::size_t start = bounded_cursor;
    while (start > 0 && repl_completion_ident_char(text[start - 1])) {
        --start;
    }
    return {start, bounded_cursor - start};
}

std::string repl_completion_ghost_suffix(const std::string& typed, const std::string& label) {
    if (label.size() <= typed.size()) {
        return {};
    }
    if (label.compare(0, typed.size(), typed) != 0) {
        return {};
    }
    return label.substr(typed.size());
}

std::vector<tui_debug_ui::ReplCompletionCandidate> parse_repl_completions(const std::string& json) {
#if defined(TUI_DEBUG_UI_HAS_NLOHMANN_JSON)
    try {
        const nlohmann::json root = nlohmann::json::parse(json);
        if (!root.is_array() || root.empty()) {
            return {};
        }

        std::vector<tui_debug_ui::ReplCompletionCandidate> items;
        for (const nlohmann::json& entry : root) {
            if (!entry.is_object() || !entry.contains("label") || !entry.at("label").is_string()) {
                continue;
            }
            tui_debug_ui::ReplCompletionCandidate item;
            item.label = entry.at("label").get<std::string>();
            if (item.label.empty()) {
                continue;
            }
            if (entry.contains("sortText") && entry.at("sortText").is_string()) {
                item.sort_text = entry.at("sortText").get<std::string>();
            }
            items.push_back(std::move(item));
        }

        std::sort(items.begin(), items.end(),
                  [](const tui_debug_ui::ReplCompletionCandidate& left, const tui_debug_ui::ReplCompletionCandidate& right) {
            const std::string& left_sort = left.sort_text.empty() ? left.label : left.sort_text;
            const std::string& right_sort = right.sort_text.empty() ? right.label : right.sort_text;
            if (left_sort != right_sort) {
                return left_sort < right_sort;
            }
            if (left.label.size() != right.label.size()) {
                return left.label.size() > right.label.size();
            }
            return left.label < right.label;
        });
        return items;
    } catch (const std::exception&) {
        return {};
    }
#else
    (void)json;
    return {};
#endif
}

std::string humanize_execution_command_error(const std::string& op, const std::string& detail,
                                           const std::string& session_state) {
    const bool is_step = op == "step_over" || op == "next" || op == "step_into" || op == "step_in" ||
                         op == "step_out";
    if (is_step && session_state == "running") {
        return "Program is running — focus Console and type input (don't step again)";
    }
    if (is_step && (detail.find("DAP next failed") != std::string::npos ||
                    detail.find("DAP stepIn failed") != std::string::npos ||
                    detail.find("DAP stepOut failed") != std::string::npos)) {
        return "Program is running — focus Console and type input";
    }
    return detail.empty() ? "Command failed" : detail;
}

bool is_execution_control_command(const char* op) {
    if (op == nullptr) {
        return false;
    }
    return std::strcmp(op, "continue") == 0 || std::strcmp(op, "play_pause") == 0 ||
           std::strcmp(op, "pause") == 0 || std::strcmp(op, "step_over") == 0 ||
           std::strcmp(op, "next") == 0 || std::strcmp(op, "step_into") == 0 ||
           std::strcmp(op, "step_in") == 0 || std::strcmp(op, "step_out") == 0 ||
           std::strcmp(op, "step_back") == 0 || std::strcmp(op, "step_back_into") == 0 ||
           std::strcmp(op, "reverse_continue") == 0;
}

bool command_state_synced_via_snapshot(const char* op) {
    if (op == nullptr) {
        return false;
    }
    return std::strcmp(op, "continue") == 0 || std::strcmp(op, "play_pause") == 0 ||
           std::strcmp(op, "pause") == 0 || std::strcmp(op, "step_over") == 0 ||
           std::strcmp(op, "next") == 0 || std::strcmp(op, "step_into") == 0 ||
           std::strcmp(op, "step_in") == 0 || std::strcmp(op, "step_out") == 0 ||
           std::strcmp(op, "step_back") == 0 || std::strcmp(op, "step_back_into") == 0 ||
           std::strcmp(op, "reverse_continue") == 0;
}

std::unordered_map<int, bool> source_breakpoints_from_info(
    const std::unordered_map<int, tui_debug_ui::BreakpointInfo>& breakpoints) {
    std::unordered_map<int, bool> source_breakpoints;
    for (const auto& [line, info] : breakpoints) {
        source_breakpoints[line] = !info.condition.empty() || !info.hit_condition.empty();
    }
    return source_breakpoints;
}

std::string trim_watch_text(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
        --end;
    }
    return text.substr(begin, end - begin);
}

bool is_simple_watch_identifier(const std::string& expression) {
    if (expression.empty()) {
        return false;
    }
    unsigned char first = static_cast<unsigned char>(expression.front());
    if (first != '_' && !std::isalpha(first)) {
        return false;
    }
    for (std::size_t i = 1; i < expression.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(expression[i]);
        if (ch != '_' && !std::isalnum(ch)) {
            return false;
        }
    }
    return true;
}

std::string normalize_watch_expression(std::string expression) {
    expression = trim_watch_text(std::move(expression));
    const std::size_t eq = expression.find('=');
    if (eq == std::string::npos) {
        return expression;
    }
    if (eq + 1 < expression.size() && expression[eq + 1] == '=') {
        return expression;
    }

    const std::string lhs = trim_watch_text(expression.substr(0, eq));
    if (is_simple_watch_identifier(lhs)) {
        return lhs;
    }
    return expression;
}

std::optional<std::string> try_resolve_watch_from_model(const tui_debug_ui::DebugUiModel& model,
                                                        const std::string& expression) {
    const std::string key = normalize_watch_expression(expression);
    if (!is_simple_watch_identifier(key)) {
        return std::nullopt;
    }

    for (const tui_debug_ui::VariableInfo& variable : model.variables) {
        if (variable.name == key) {
            return variable.value;
        }
    }

    for (const auto& [_, variables] : model.scope_variables) {
        for (const tui_debug_ui::VariableInfo& variable : variables) {
            if (variable.name == key) {
                return variable.value;
            }
        }
    }

    return std::nullopt;
}

std::optional<std::string> variable_name_from_scope_row(const std::string& line) {
    if (const auto parsed = tui_debug_ui::NavigableListView::parse_scope_variable_row(line)) {
        return std::string(parsed->name);
    }
    return std::nullopt;
}

bool scope_variable_value_is_empty(const std::string& value) {
    return std::find_if(value.begin(), value.end(),
                        [](unsigned char ch) { return !std::isspace(ch); }) == value.end();
}

std::string truncate_scope_display_value(const std::string& value) {
    constexpr std::size_t kMaxWidth = 56;
    if (value.size() <= kMaxWidth) {
        return value;
    }
    return value.substr(0, kMaxWidth - 1) + "…";
}

std::string format_scope_leaf_row(const std::string& indent, const std::string& name, const std::string& value) {
    if (scope_variable_value_is_empty(value)) {
        return indent + name;
    }
    return indent + name + " = " + truncate_scope_display_value(value);
}

void patch_scope_row_value(std::vector<std::string>& rows, const std::string& name, const std::string& value) {
    for (std::string& row : rows) {
        const auto parsed = tui_debug_ui::NavigableListView::parse_scope_variable_row(row);
        if (!parsed.has_value() || parsed->name != name) {
            continue;
        }
        const std::string indent(static_cast<std::size_t>(parsed->depth) * 2, ' ');
        if (parsed->expandable) {
            row = indent + (parsed->expanded ? tui_debug_ui::kScopeExpandExpanded
                                             : tui_debug_ui::kScopeExpandCollapsed) +
                  name;
        } else {
            row = format_scope_leaf_row(indent, name, value);
        }
    }
}

std::optional<std::string> try_resolve_watch_from_scope_rows(const std::vector<std::string>& rows,
                                                             const std::string& expression) {
    const std::string key = normalize_watch_expression(expression);
    if (!is_simple_watch_identifier(key)) {
        return std::nullopt;
    }

    for (const std::string& row : rows) {
        const auto parsed = tui_debug_ui::NavigableListView::parse_scope_variable_row(row);
        if (parsed.has_value() && parsed->name == key) {
            return std::string(parsed->value);
        }
    }

    return std::nullopt;
}

std::string escape_json_string(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char ch : value) {
        switch (ch) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped.push_back(ch);
            break;
        }
    }
    return escaped;
}

std::string trim_breakpoint_condition(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
        value.pop_back();
    }
    return value;
}

bool is_locals_scope_name(const std::string& name) {
    return name == "Locals" || (name.size() >= 6 && name.compare(0, 6, "Locals") == 0);
}

bool scope_rows_include_variables(const std::vector<std::string>& rows) {
    for (const std::string& row : rows) {
        if (tui_debug_ui::NavigableListView::parse_scope_variable_row(row).has_value()) {
            return true;
        }
    }
    return false;
}

struct VariableEditTarget {
    std::int64_t variables_reference = 0;
    std::string value;
};

std::optional<VariableEditTarget> find_variable_for_edit(const tui_debug_ui::DebugUiModel& model,
                                                           const std::string& name) {
    for (const auto& [variables_reference, variables] : model.scope_variables) {
        for (const tui_debug_ui::VariableInfo& variable : variables) {
            if (variable.name == name) {
                return VariableEditTarget{variables_reference, variable.value};
            }
        }
    }
    return std::nullopt;
}

constexpr char kScopePathSeparator = '\x1F';

using ExpandedScopePaths = std::unordered_set<std::string>;
using PendingScopePaths = std::unordered_set<std::string>;

std::optional<std::int64_t> resolve_scope_path_reference(const tui_debug_ui::DebugUiModel& model,
                                                         const std::string& path) {
    const std::size_t first_sep = path.find(kScopePathSeparator);
    if (first_sep == std::string::npos) {
        return std::nullopt;
    }

    const std::string scope_name = path.substr(0, first_sep);
    const tui_debug_ui::ScopeInfo* scope = nullptr;
    for (const tui_debug_ui::ScopeInfo& candidate : model.scopes) {
        if (candidate.name == scope_name) {
            scope = &candidate;
            break;
        }
    }
    if (scope == nullptr || scope->variables_reference <= 0) {
        return std::nullopt;
    }

    std::int64_t container_reference = scope->variables_reference;
    std::size_t pos = first_sep + 1;
    while (pos < path.size()) {
        const std::size_t next_sep = path.find(kScopePathSeparator, pos);
        const std::string segment =
            next_sep == std::string::npos ? path.substr(pos) : path.substr(pos, next_sep - pos);

        const auto vars_it = model.scope_variables.find(container_reference);
        if (vars_it == model.scope_variables.end()) {
            return std::nullopt;
        }

        const tui_debug_ui::VariableInfo* variable = nullptr;
        for (const tui_debug_ui::VariableInfo& candidate : vars_it->second) {
            if (candidate.name == segment) {
                variable = &candidate;
                break;
            }
        }
        if (variable == nullptr || variable->variables_reference <= 0) {
            return std::nullopt;
        }
        if (next_sep == std::string::npos) {
            return variable->variables_reference;
        }

        container_reference = variable->variables_reference;
        pos = next_sep + 1;
    }
    return std::nullopt;
}

void append_scope_variables(const tui_debug_ui::DebugUiModel& model, std::int64_t container_reference, int depth,
                            const std::string& path_prefix, const ExpandedScopePaths& expanded,
                            const PendingScopePaths& pending, std::vector<std::string>& rows,
                            std::vector<tui_debug_ui::ScopeVariableRowMeta>& meta) {
    const auto vars_it = model.scope_variables.find(container_reference);
    if (vars_it == model.scope_variables.end()) {
        return;
    }

    const std::string indent(static_cast<std::size_t>(depth) * 2, ' ');
    for (const tui_debug_ui::VariableInfo& variable : vars_it->second) {
        tui_debug_ui::ScopeVariableRowMeta row_meta{};
        row_meta.container_reference = container_reference;
        row_meta.variable_name = variable.name;
        row_meta.show_edit = !variable.has_children();

        if (variable.has_children()) {
            const std::string path = path_prefix + variable.name;
            row_meta.expand_path = path;
            row_meta.expand_reference = variable.variables_reference;
            const bool is_expanded = expanded.count(path) > 0;
            const bool is_pending = pending.count(path) > 0;
            std::string row = indent;
            row += is_expanded ? tui_debug_ui::kScopeExpandExpanded : tui_debug_ui::kScopeExpandCollapsed;
            row += variable.name;
            rows.push_back(std::move(row));
            meta.push_back(row_meta);

            if (is_expanded) {
                if (is_pending) {
                    rows.push_back(std::string(static_cast<std::size_t>(depth + 1) * 2, ' ') + "…");
                    meta.push_back({});
                } else {
                    append_scope_variables(model, variable.variables_reference, depth + 1, path, expanded, pending,
                                           rows, meta);
                }
            }
            continue;
        }

        rows.push_back(format_scope_leaf_row(indent, variable.name, variable.value));
        meta.push_back(row_meta);
    }
}

std::vector<std::string> build_scope_rows(const tui_debug_ui::DebugUiModel& model, const ExpandedScopePaths& expanded,
                                          const PendingScopePaths& pending,
                                          const std::unordered_set<std::string>& collapsed_sections,
                                          std::vector<tui_debug_ui::ScopeVariableRowMeta>& meta,
                                          const std::optional<std::string>& scope_filter = std::nullopt) {
    std::vector<std::string> scope_rows;
    meta.clear();
    for (const tui_debug_ui::ScopeInfo& scope : model.scopes) {
        if (scope_filter.has_value() && scope.name != *scope_filter) {
            continue;
        }
        const bool scoped_panel = scope_filter.has_value();
        if (!scoped_panel) {
            const bool section_expanded = collapsed_sections.count(scope.name) == 0;
            std::string header = (section_expanded ? tui_debug_ui::kScopeExpandExpanded
                                                   : tui_debug_ui::kScopeExpandCollapsed) +
                                 scope.name + ":";
            scope_rows.push_back(std::move(header));
            tui_debug_ui::ScopeVariableRowMeta header_meta{};
            header_meta.is_scope_section = true;
            header_meta.scope_section_name = scope.name;
            meta.push_back(header_meta);
            if (!section_expanded) {
                continue;
            }
        }
        if (scope.variables_reference > 0) {
            const std::string path_prefix = scope.name + std::string(1, kScopePathSeparator);
            const int depth = scoped_panel ? 0 : 1;
            append_scope_variables(model, scope.variables_reference, depth, path_prefix, expanded, pending, scope_rows,
                                   meta);
        }
    }
    return scope_rows;
}

bool is_wheel_action(tuinator::MouseAction action) {
    return action == tuinator::MouseAction::WheelUp || action == tuinator::MouseAction::WheelDown ||
           action == tuinator::MouseAction::WheelLeft || action == tuinator::MouseAction::WheelRight;
}

bool is_mouse_position_tracking_action(tuinator::MouseAction action) {
    return action == tuinator::MouseAction::Move || action == tuinator::MouseAction::Press ||
           action == tuinator::MouseAction::Release || action == tuinator::MouseAction::Click;
}

bool is_plausible_mouse_position(tuinator::Point point, const tuinator::Rect& bounds) {
    return bounds.contains(point) && !(point.x == 0 && point.y == 0);
}

tuinator::Point wheel_routing_position(const tuinator::MouseEvent& mouse,
                                       const std::optional<tuinator::Point>& last_mouse_position,
                                       const tuinator::Rect& root_bounds) {
    if (is_plausible_mouse_position(mouse.position, root_bounds)) {
        return mouse.position;
    }
    if (last_mouse_position.has_value() && is_plausible_mouse_position(*last_mouse_position, root_bounds)) {
        return *last_mouse_position;
    }
    return mouse.position;
}

/// Root shell with fixed chrome rows: controls (top), content (middle), status (bottom).
class DebugChromeRoot : public tuinator::Widget {
  public:
    DebugChromeRoot(tuinator::Application* app, tui_debug_ui::DebugApp* debug_app,
                    std::unique_ptr<tuinator::Widget> controls, std::unique_ptr<tuinator::Widget> content,
                    std::unique_ptr<tuinator::Widget> status, tuinator::Style chrome_background)
        : app_(app), debug_app_(debug_app), controls_(std::move(controls)), content_(std::move(content)),
          status_(std::move(status)), chrome_background_(chrome_background) {}

    bool wants_full_screen() const override { return true; }
    bool needs_periodic_idle() const override { return true; }

    void on_idle() override {
        if (debug_app_ != nullptr) {
            debug_app_->poll_session();
            debug_app_->maybe_refresh_source_highlight_for_scroll();
        }
    }

    tuinator::Size preferred_size() const override {
        const tuinator::Size content_size = content_ ? content_->preferred_size() : tuinator::Size{};
        return {content_size.width, content_size.height + kControlsBarRows + kStatusBarRows};
    }

    void layout(tuinator::Rect bounds) override {
        bounds_ = bounds;
        if (bounds.height <= 0 || bounds.width <= 0) {
            return;
        }

        const int status_y = bounds.y + bounds.height - kStatusBarRows;
        const int content_y = bounds.y + kControlsBarRows;
        const int content_h = std::max(0, bounds.height - kControlsBarRows - kStatusBarRows);

        if (controls_ != nullptr) {
            controls_->layout({bounds.x, bounds.y, bounds.width, kControlsBarRows});
        }
        if (content_ != nullptr) {
            content_->layout({bounds.x, content_y, bounds.width, content_h});
        }
        if (status_ != nullptr) {
            status_->layout({bounds.x, status_y, bounds.width, kStatusBarRows});
        }
    }

    void paint(tuinator::PaintContext& ctx) const override {
        if (bounds_.width > 0 && bounds_.height > 0) {
            ctx.canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', chrome_background_);
        }

        auto paint_child = [&](const tuinator::Widget* child) {
            if (child == nullptr) {
                return;
            }
            const tuinator::Rect local{child->bounds().x - bounds_.x, child->bounds().y - bounds_.y,
                                       child->bounds().width, child->bounds().height};
            ctx.with_clip(local, [&](tuinator::PaintContext& child_ctx) { child->paint(child_ctx); });
        };

        paint_child(controls_.get());
        paint_child(content_.get());
        paint_child(status_.get());

        if (debug_app_ != nullptr) {
            debug_app_->paint_overlay(ctx);
            debug_app_->finalize_text_cursor(ctx);
        }
    }

    bool handle_event(const tuinator::Event& event) override {
        if (debug_app_ != nullptr && debug_app_->overlay_intercepts_events() &&
            debug_app_->handle_overlay_event(event)) {
            return true;
        }

        if (std::holds_alternative<tuinator::Resize>(event)) {
            if (debug_app_ != nullptr) {
                debug_app_->on_terminal_resize();
            }
            return true;
        }

        if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
            if (debug_app_ != nullptr && debug_app_->handle_stacked_pane_rename_key(event)) {
                return true;
            }
            if (key->ctrl && key->character == 'c') {
                if (app_ != nullptr) {
                    app_->quit();
                }
                return true;
            }
            const bool block_quit =
                debug_app_ != nullptr && debug_app_->should_block_app_quit_key(*key);
            if (!block_quit &&
                (key->key == tuinator::Key::Escape || key->character == 'q' || key->character == 'Q')) {
                if (app_ != nullptr) {
                    app_->quit();
                }
                return true;
            }
            if (block_quit && key->key == tuinator::Key::Escape && debug_app_ != nullptr) {
                if (debug_app_->is_watch_input_focused()) {
                    debug_app_->blur_watch_input();
                    return true;
                }
                if (debug_app_->is_repl_input_focused()) {
                    debug_app_->blur_repl_input();
                    return true;
                }
                if (debug_app_->is_scope_input_focused()) {
                    debug_app_->blur_scope_input();
                    return true;
                }
                if (debug_app_->is_memory_input_focused()) {
                    debug_app_->blur_active_memory_input();
                    return true;
                }
                if (debug_app_->is_breakpoint_input_focused()) {
                    debug_app_->blur_breakpoint_input();
                    return true;
                }
            }
            if (debug_app_ != nullptr && debug_app_->handle_global_key(*key)) {
                return true;
            }
            if (debug_app_ != nullptr && debug_app_->handle_repl_input_key(event)) {
                return true;
            }
            if (debug_app_ != nullptr && debug_app_->handle_breakpoint_input_key(event)) {
                return true;
            }
            if (debug_app_ != nullptr && debug_app_->handle_scope_input_key(event)) {
                return true;
            }
            if (debug_app_ != nullptr && debug_app_->handle_memory_toolbar_input_key(event)) {
                return true;
            }
            if (debug_app_ != nullptr && debug_app_->handle_memory_input_key(event)) {
                return true;
            }
            if (debug_app_ != nullptr && debug_app_->handle_watch_input_key(event)) {
                return true;
            }
        }

        if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
            if (is_mouse_position_tracking_action(mouse->action) && bounds_.contains(mouse->position)) {
                last_mouse_position_ = mouse->position;
            }
            if (debug_app_ != nullptr && mouse->action == tuinator::MouseAction::Move) {
                debug_app_->sync_controls_hover(mouse->position);
            }

            if (is_wheel_action(mouse->action)) {
                tuinator::MouseEvent routed = *mouse;
                routed.position = wheel_routing_position(*mouse, last_mouse_position_, bounds_);

                for (tuinator::Widget* child : {controls_.get(), status_.get(), content_.get()}) {
                    if (child != nullptr && child->bounds().contains(routed.position)) {
                        return child->handle_event(routed);
                    }
                }
                return false;
            }

            if (debug_app_ != nullptr && debug_app_->handle_layout_drag_mouse(*mouse)) {
                return true;
            }

            const bool pointer_pick = mouse->action == tuinator::MouseAction::Click ||
                                      mouse->action == tuinator::MouseAction::Release ||
                                      mouse->action == tuinator::MouseAction::Press;
            if (pointer_pick && debug_app_ != nullptr) {
                debug_app_->handle_pointer_pick(*mouse);
            }

            // Status row before content so the bottom chrome row is not shadowed by content hit tests.
            bool handled = false;
            for (tuinator::Widget* child : {controls_.get(), status_.get(), content_.get()}) {
                if (child != nullptr && child->bounds().contains(mouse->position) && child->handle_event(event)) {
                    handled = true;
                }
            }
            if (pointer_pick && debug_app_ != nullptr) {
                debug_app_->sync_focus_from_ui();
            }
            return handled;
        }

        for (tuinator::Widget* child : {content_.get(), controls_.get(), status_.get()}) {
            if (child != nullptr && child->handle_event(event)) {
                return true;
            }
        }
        return false;
    }

    tuinator::Widget* hit_test(tuinator::Point point) override {
        if (!bounds_.contains(point)) {
            return nullptr;
        }

        for (tuinator::Widget* child : {controls_.get(), status_.get(), content_.get()}) {
            if (child != nullptr) {
                if (tuinator::Widget* hit = child->hit_test(point)) {
                    return hit;
                }
            }
        }
        return this;
    }

    bool has_focused_descendant() const override {
        return (controls_ && controls_->has_focused_descendant()) || (content_ && content_->has_focused_descendant()) ||
               (status_ && status_->has_focused_descendant());
    }

    void collect_focusable(std::vector<tuinator::Widget*>& out) override {
        for (tuinator::Widget* child : {controls_.get(), content_.get(), status_.get()}) {
            if (child != nullptr) {
                child->collect_focusable(out);
            }
        }
    }

    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override {
        for (tuinator::Widget* child : {controls_.get(), content_.get(), status_.get()}) {
            if (child != nullptr) {
                visitor(child);
            }
        }
    }

    void replace_content(std::unique_ptr<tuinator::Widget> content) {
        content_ = std::move(content);
        if (content_ != nullptr) {
            content_->set_flex(1);
            if (on_dirty_) {
                content_->set_on_dirty(on_dirty_);
                if (auto* split = dynamic_cast<tui_debug_ui::ResizableSplitPane*>(content_.get())) {
                    split->propagate_on_dirty(on_dirty_);
                }
            }
        }
        if (bounds_.width > 0 && bounds_.height > 0 && content_ != nullptr) {
            const int content_y = bounds_.y + kControlsBarRows;
            const int content_h = std::max(0, bounds_.height - kControlsBarRows - kStatusBarRows);
            content_->layout({bounds_.x, content_y, bounds_.width, content_h});
        }
        mark_dirty();
    }

    void set_on_dirty(std::function<void(tuinator::Rect)> callback) override {
        // Chrome rows repaint via their own widgets' dirty rects; expanding every
        // child rect to include them would turn each partial redraw into a
        // full-height repaint.
        Widget::set_on_dirty(std::move(callback));
        if (controls_ != nullptr) {
            controls_->set_on_dirty(on_dirty_);
        }
        if (content_ != nullptr) {
            content_->set_on_dirty(on_dirty_);
        }
        if (status_ != nullptr) {
            status_->set_on_dirty(on_dirty_);
        }
    }

  private:
    tuinator::Application* app_ = nullptr;
    tui_debug_ui::DebugApp* debug_app_ = nullptr;
    std::unique_ptr<tuinator::Widget> controls_;
    std::unique_ptr<tuinator::Widget> content_;
    std::unique_ptr<tuinator::Widget> status_;
    tuinator::Style chrome_background_;
    std::optional<tuinator::Point> last_mouse_position_;
};

DebugChromeRoot* g_chrome_root = nullptr;

void set_debug_chrome_root(DebugChromeRoot* root) { g_chrome_root = root; }

bool debug_chrome_root_ready() { return g_chrome_root != nullptr; }

void replace_debug_chrome_content(std::unique_ptr<tuinator::Widget> content) {
    if (g_chrome_root != nullptr) {
        g_chrome_root->replace_content(std::move(content));
    }
}

int sidebar_first_size(int terminal_width, std::uint16_t sidebar_pct) {
    const int pct = static_cast<int>(sidebar_pct);
    return std::max(24, terminal_width * pct / 100);
}

int bottom_tray_height(int terminal_height, std::uint16_t bottom_pct) {
    const int pct = static_cast<int>(bottom_pct);
    return std::max(8, terminal_height * pct / 100);
}

int content_area_height(int terminal_height) {
    return std::max(12, terminal_height - kControlsBarRows - kStatusBarRows);
}

int main_area_height(int terminal_height, int bottom_tray_height) {
    return std::max(8, content_area_height(terminal_height) - bottom_tray_height - kSplitDividerRows);
}

std::string source_cache_key(const std::string& path, std::int64_t source_reference) {
    if (!path.empty()) {
        if (path.rfind("dap:source:", 0) == 0) {
            return path;
        }
        try {
            std::filesystem::path resolved(path);
            if (resolved.is_relative()) {
                resolved = std::filesystem::absolute(resolved);
            }
            return std::filesystem::weakly_canonical(resolved).string();
        } catch (...) {
            return path;
        }
    }
    if (source_reference > 0) {
        return "dap:source:" + std::to_string(source_reference);
    }
    return {};
}

std::string panel_title_from_path(const std::string& path) {
    if (path.empty()) {
        return "Source";
    }
    constexpr std::string_view kAdapterPrefix = "dap:source:";
    if (path.rfind(kAdapterPrefix, 0) == 0) {
        return "adapter source #" + path.substr(kAdapterPrefix.size());
    }
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

bool viewing_same_source(const std::string& left_path, std::int64_t left_reference,
                         const std::string& right_path, std::int64_t right_reference) {
    return source_cache_key(left_path, left_reference) == source_cache_key(right_path, right_reference);
}

std::string read_file_or_empty(const std::string& path) {
    std::ifstream input(path);
    if (!input.is_open()) {
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

bool looks_like_elf_executable(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return false;
    }
    char magic[4] = {};
    input.read(magic, 4);
    return input.gcount() == 4 && magic[0] == '\x7f' && magic[1] == 'E' && magic[2] == 'L' && magic[3] == 'F';
}

bool source_file_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    std::ifstream input(path);
    return input.good();
}

std::string sibling_source_for_executable(const std::string& program_path) {
    if (!looks_like_elf_executable(program_path)) {
        return {};
    }

    const std::filesystem::path executable(program_path);
    const std::string stem = executable.filename().string();
    const std::filesystem::path parent = executable.parent_path();

    for (const char* extension : {".c", ".cpp", ".cc", ".cxx", ".rs"}) {
        const std::filesystem::path candidate = parent / (stem + extension);
        if (source_file_exists(candidate.string())) {
            return candidate.string();
        }
    }
    return {};
}

std::string resolve_debugger_source_path(const std::string& path, const std::string& program_path) {
    if (path.empty() || path.rfind("dap:source:", 0) == 0) {
        return path;
    }

    std::string normalized = path;
    if (const std::size_t tick = normalized.find('`'); tick != std::string::npos) {
        normalized.resize(tick);
    }

    if (source_file_exists(normalized)) {
        return normalized;
    }

    try {
        const std::filesystem::path reported(normalized);
        if (!program_path.empty()) {
            const std::filesystem::path program(program_path);
            const std::filesystem::path joined = program.parent_path() / reported;
            if (source_file_exists(joined.string())) {
                return joined.string();
            }
            if (!reported.filename().empty()) {
                const std::filesystem::path sibling = program.parent_path() / reported.filename();
                if (source_file_exists(sibling.string())) {
                    return sibling.string();
                }
            }
        }

        if (reported.is_relative()) {
            const std::filesystem::path absolute = std::filesystem::absolute(reported);
            if (source_file_exists(absolute.string())) {
                return absolute.string();
            }
        }
    } catch (...) {
    }

    if (looks_like_elf_executable(normalized)) {
        return sibling_source_for_executable(normalized);
    }

    return normalized;
}

bool should_open_program_path_as_source(const std::string& program_path) {
    if (program_path.empty() || looks_like_elf_executable(program_path)) {
        return false;
    }
    return source_file_exists(program_path);
}

std::string preferred_program_source_path(const std::string& program_path) {
    if (should_open_program_path_as_source(program_path)) {
        return program_path;
    }
    return sibling_source_for_executable(program_path);
}

int count_file_lines(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    int lines = 1;
    for (char ch : text) {
        if (ch == '\n') {
            ++lines;
        }
    }
    return lines;
}

std::string line_text_at(const std::string& text, int line_number) {
    if (text.empty() || line_number <= 0) {
        return {};
    }

    int current = 1;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index == text.size() || text[index] == '\n') {
            if (current == line_number) {
                return text.substr(start, index - start);
            }
            ++current;
            start = index + 1;
        }
    }
    return {};
}

std::string trimmed_line_text_at(const std::string& text, int line_number) {
    std::string line = line_text_at(text, line_number);
    const std::size_t start = line.find_first_not_of(" \t\r");
    if (start == std::string::npos) {
        return {};
    }
    const std::size_t end = line.find_last_not_of(" \t\r");
    return line.substr(start, end - start + 1);
}

bool is_blank_source_line(const std::string& line) {
    for (unsigned char ch : line) {
        if (!std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

std::string highlighted_line_text(const tui_debug_ui::SourcePanel* panel, int line_number) {
    if (panel == nullptr) {
        return {};
    }

    for (const tui_debug_ui::HighlightedLine& line : panel->lines()) {
        if (line.line_number != line_number) {
            continue;
        }
        std::string merged;
        for (const tui_debug_ui::HighlightSpan& span : line.spans) {
            merged += span.text;
        }
        return merged;
    }
    return {};
}

bool is_breakpointable_line(const tui_debug_ui::SourcePanel* panel, const std::string& file_text, int line_number) {
    const std::string line_text =
        file_text.empty() ? highlighted_line_text(panel, line_number) : line_text_at(file_text, line_number);
    return !is_blank_source_line(line_text);
}

std::string lowercase_copy(std::string value) {
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

bool exception_filter_is_objective_c(const tui_debug_ui::DebugUiModel::ExceptionBreakpointFilterInfo& filter) {
    const std::string haystack = lowercase_copy(filter.label + ' ' + filter.filter);
    return haystack.find("objective-c") != std::string::npos || haystack.find("objc") != std::string::npos;
}

bool exception_filter_relevant_for_source(const tui_debug_ui::DebugUiModel::ExceptionBreakpointFilterInfo& filter,
                                          const std::string& source_path) {
    if (!exception_filter_is_objective_c(filter)) {
        return true;
    }
    const std::string path = lowercase_copy(source_path);
    return path.size() >= 2 && (path.compare(path.size() - 2, 2, ".m") == 0 ||
                                (path.size() >= 3 && path.compare(path.size() - 3, 3, ".mm") == 0));
}

}  // namespace

namespace tui_debug_ui {

DebugApp::DebugApp(const std::string& program_path, SessionMode mode, LaunchUiSettings launch_ui,
                   std::vector<std::string> program_args, AppConfig app_config,
                   std::optional<std::string> resolved_launch_json,
                   std::optional<std::filesystem::path> workspace_override,
                   std::optional<std::string> display_source_override)
    : mode_(mode),
      launch_ui_(std::move(launch_ui)),
      program_path_(program_path),
      program_args_(std::move(program_args)),
      resolved_launch_json_(std::move(resolved_launch_json)),
      display_source_override_(std::move(display_source_override)),
      session_io_(std::make_unique<SessionIoThread>(mode)),
      workspace_root_(workspace_override.has_value() ? *workspace_override
                                                       : workspace_root_for_program(program_path_)),
      app_config_(std::move(app_config)),
      app_(std::make_unique<tuinator::Application>()) {
    const LoadedTheme loaded_theme = load_application_theme(app_config_);
    dap_theme_ = loaded_theme.ui;
    syntax_theme_ = loaded_theme.syntax;

    if (app_config_.layout.sidebar_pct.has_value()) {
        model_.layout.sidebar_pct = *app_config_.layout.sidebar_pct;
    }
    if (app_config_.layout.bottom_pct.has_value()) {
        model_.layout.bottom_pct = *app_config_.layout.bottom_pct;
    }

    tuinator::Theme theme = app_->theme();
    dap_theme_.apply_to(theme);
    app_->set_theme(theme);

    if (mode_ == SessionMode::Mock) {
        model_.status_message = "Mock UI mode (no Rust backend)";
    } else if (launch_ui_.is_rr_backend) {
        model_.status_message = "Recording with rr…";
    } else if (!launch_ui_.adapter_label.empty()) {
        model_.status_message = "Connecting to " + launch_ui_.adapter_label + "…";
    } else {
        model_.status_message = "Connecting to debug adapter…";
    }

}

DebugApp::~DebugApp() = default;

int DebugApp::run() {
    sync_terminal_size_from_tty();
    set_terminal_theme_background(dap_theme_.background);
    terminal_ready_for_session_ = true;
    // Initialize ncurses and build the widget tree before the first event-loop frame.
    app_->present();
    // Re-apply after ncurses init so UTF-8 locale detection picks Unicode box glyphs.
    {
        tuinator::Theme theme = app_->theme();
        dap_theme_.apply_to(theme);
        app_->set_theme(theme);
    }
    ensure_ui_built();
    return app_->run();
}

void DebugApp::ensure_ui_built() {
    if (ui_built_) {
        return;
    }
    apply_ncurses_winsize();
    build_ui();
    ui_built_ = true;
}

void DebugApp::on_terminal_resize() {
    apply_ncurses_winsize();
    build_ui();
    ui_built_ = true;
}

void DebugApp::build_ui() {
    capture_watch_input_state();
    capture_scope_input_state();
    capture_breakpoint_input_state();

    cached_status_bar_text_.clear();
    ensure_layout_tree_initialized();
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        for (SidebarSlot& slot : leaf_slots(leaf_id)) {
            slot.cached_scope_rows.clear();
            slot.cached_scope_row_meta.clear();
        }
    }
    cached_stack_lines_.clear();
    cached_highlight_first_line_ = -1;
    cached_highlight_line_count_ = -1;

    const tuinator::Size term_size = app_->terminal_size();
    const int tray_body = bottom_tray_height(term_size.height, model_.layout.bottom_pct);
    const int main_h = main_area_height(term_size.height, tray_body);

    auto controls = std::make_unique<ControlsBar>(dap_theme_);
    controls_bar_ = controls.get();
    controls->set_on_action([this](const std::string& op) { send_command(op.c_str()); });
    controls->set_on_hover_changed([this]() { request_repaint(); });

    const auto scroll_options = dap_theme_.scroll_view_options();
    scopes_panel_ = nullptr;
    watches_panel_ = nullptr;
    stacks_panel_ = nullptr;
    breakpoints_panel_ = nullptr;

    auto source_panel = std::make_unique<SourcePanel>(syntax_theme_);
    source_panel_ = source_panel.get();
    context_menu_ = std::make_unique<ContextMenu>(dap_theme_.panel_background, dap_theme_.label, dap_theme_.selection,
                                                  dap_theme_.border_focused);
    file_picker_ = std::make_unique<FilePicker>(
        dap_theme_.panel_background, dap_theme_.border_focused, dap_theme_.title_source, dap_theme_.label,
        dap_theme_.selection, dap_theme_.variable_name, dap_theme_.frame_current, dap_theme_.label,
        dap_theme_.divider);
    file_picker_->set_on_select([this](const std::filesystem::path& path) {
        open_source_file(path.string(), 1, false);
        model_.status_message = "Opened " + relative_display(workspace_root_, path);
        sync_status_bar();
        request_repaint();
    });

    source_panel_->set_on_toggle_breakpoint([this](int line) { toggle_breakpoint_at_line(line); });
    source_panel_->set_on_breakpoint_context([this](int line, int code_column, tuinator::Point anchor) {
        const std::string path = effective_source_path();
        std::optional<SourceContextIdentifier> source_identifier;
        if (!path.empty()) {
            if (cached_source_path_ != path) {
                cached_source_path_ = path;
                cached_source_text_ = read_file_or_empty(path);
            }
            source_identifier = resolve_source_identifier(path, cached_source_text_, line, code_column);
        }
        begin_source_context_menu(goto_probe_source_path(), line, code_column, anchor, source_identifier);
    });
    source_panel_->set_on_request_viewport([this](int /*center_line*/) {
        if (divider_drag_active_) {
            return;
        }
        cached_highlight_first_line_ = -1;
        cached_highlight_line_count_ = -1;
        highlight_request_first_line_ = -1;
        highlight_request_line_count_ = -1;
        maybe_request_source_highlight();
    });
    source_panel_->set_on_step_in_target_click([this](int target_index) {
        if (!step_in_selection_active()) {
            return;
        }
        if (target_index < 0 || target_index >= static_cast<int>(step_in_selection_->targets.size())) {
            return;
        }
        step_in_selection_->active_index = target_index;
        confirm_step_in_selection();
    });
    if (!program_path_.empty()) {
        const std::string preview_path = preferred_program_source_path(program_path_);
        if (!preview_path.empty()) {
            source_panel_->set_file_line_count(std::max(1, count_file_lines(read_file_or_empty(preview_path))));
        }
    }

    auto source_tab_bar = std::make_unique<SourceTabBar>(dap_theme_.panel_background, dap_theme_.label,
                                                         dap_theme_.title_source, dap_theme_.row_action_muted,
                                                         dap_theme_.breakpoint_hit_count, dap_theme_.divider);
    source_tab_bar_ = source_tab_bar.get();
    source_tab_bar_->set_on_select([this](int index) { switch_source_file_tab(index); });
    source_tab_bar_->set_on_close([this](int index) { close_source_file_tab(index); });

    source_section_ = std::make_unique<TitledScrollPane>(
        panel_type_label(SidebarPanelType::Source), std::move(source_panel), dap_theme_.title_source,
        dap_theme_.panel_background, scroll_options, true, false, std::move(source_tab_bar));
    source_scroll_view_ = source_section_->scroll_view();
    if (source_panel_ != nullptr && source_scroll_view_ != nullptr) {
        source_panel_->set_scroll_parent(source_scroll_view_);
    }
    source_content_shell_ = source_section_->release_widget();

    reset_layout_slot_widgets();

    repl_panel_ = std::make_unique<ReplPanel>(dap_theme_, scroll_options);
    repl_panel_->set_on_submit([this](const std::string& expression) { submit_repl_expression(expression); });
    repl_panel_->set_on_change([this](const std::string& expression) {
        repl_input_draft_ = expression;
        repl_input_focused_ = true;
        model_.focus = Focus::Repl;
        repl_last_edit_time_ = std::chrono::steady_clock::now();
        clear_repl_completion_state();
    });
    repl_panel_->set_on_completion_request([this]() { request_repl_completion(); });
    repl_panel_->set_on_completion_cycle([this](int delta) { return cycle_repl_completion(delta); });
    repl_panel_->set_on_completion_cancel([this]() {
        repl_completion_results_ready_ = false;
        repl_completion_candidates_.clear();
        repl_completion_matches_.clear();
        repl_completion_selected_index_ = 0;
    });
    repl_panel_->set_on_activate([this]() {
        model_.focus = Focus::Repl;
        repl_input_focused_ = true;
        if (repl_panel_ != nullptr) {
            if (!repl_input_draft_.empty()) {
                repl_panel_->set_input_value(repl_input_draft_);
            }
            repl_panel_->focus_input();
        }
        apply_focus();
    });
    if (!repl_input_draft_.empty()) {
        repl_panel_->set_input_value(repl_input_draft_);
    }
    sync_repl_panel();
    repl_shell_ = repl_panel_->release_widget();
    repl_shell_->set_flex(1);

    auto console_panel = std::make_unique<ConsolePanel>(dap_theme_);
    console_panel_ = console_panel.get();

    console_panel_->set_line_buffered_input(launch_ui_.line_buffered_console);
    console_panel_->set_on_activate([this]() {
        model_.focus = Focus::Console;
        apply_focus();
    });
    console_panel_->set_on_input([this](const std::string& bytes) {
        if (session_io_ == nullptr) {
            return;
        }
        session_io_->post_terminal_input(bytes);
        if (launch_ui_.line_buffered_console && is_session_stopped() && model_.session_state != "running" &&
            !bytes.empty() && bytes.back() == '\n') {
            session_io_->post_command("continue");
        }
    });

    auto console_section = std::make_unique<TitledScrollPane>("Console", std::move(console_panel),
                                                              dap_theme_.title_console, dap_theme_.panel_background,
                                                              scroll_options, false, false);
    console_scroll_view_ = console_section->scroll_view();
    console_shell_ = console_section->release_widget();
    console_shell_->set_flex(1);

    network_panel_ = nullptr;
    auto network_panel = std::make_unique<NetworkPanel>(dap_theme_, scroll_options);
    network_panel_ = network_panel.get();
    network_panel_->set_workspace_root(workspace_root_);
    wire_network_panel(*network_panel_);
    network_shell_ = std::move(network_panel);
    network_shell_->set_flex(1);
    if (network_panel_ != nullptr) {
        network_panel_->refresh_scroll_content();
    }

    auto content_split = build_layout_content_widget();

    update_active_panel_pointers();
    if (!watch_input_draft_.empty() && watches_panel_ != nullptr) {
        watches_panel_->set_inline_edit(-1, watch_input_draft_, "?");
        sync_watches_panel();
    }
    refresh_all_scope_slots();
    sync_file_tree_slots();
    sync_source_stack_title();

    auto status = std::make_unique<tuinator::StatusBar>(format_status_bar_text(), dap_theme_.status_bar);
    status_bar_ = status.get();

    auto root = std::make_unique<DebugChromeRoot>(app_.get(), this, std::move(controls), std::move(content_split),
                                                  std::move(status), dap_theme_.panel_background);
    set_debug_chrome_root(root.get());

    app_->set_root(std::move(root));
    sync_overlay_mouse_tracking();
    sync_ui_from_model();
    sync_controls_bar();
    sync_breakpoints_list_panel();
    if (!source_file_tabs_.empty() && active_source_file_tab_ >= 0 &&
        active_source_file_tab_ < static_cast<int>(source_file_tabs_.size())) {
        sync_source_file_tab_bar();
        activate_source_file_tab(active_source_file_tab_, 0);
    } else if (!model_.source_path.empty()) {
        const int line =
            model_.execution_line > 0 ? static_cast<int>(model_.execution_line) : 1;
        open_source_file(model_.source_path, line, false, model_.source_reference);
    } else if (const std::string preview_path = preferred_program_source_path(program_path_);
               !preview_path.empty()) {
        open_source_file(preview_path, 1, false);
        if (!launch_posted_) {
            prefetch_program_source_highlight();
        }
    } else {
        sync_source_file_tab_bar();
    }
    refresh_scroll_views();
    restore_watch_input_state();
    restore_breakpoint_input_state();
    restore_scope_input_state();
    request_full_screen_refresh();
}

void DebugApp::refresh_scroll_views(bool include_source) {
    auto refresh = [](tuinator::ScrollView* scroll) {
        if (scroll != nullptr) {
            scroll->refresh_content();
        }
    };

    if (scopes_panel_ != nullptr) {
        refresh(scopes_panel_->scroll_view());
    }
    if (stacks_panel_ != nullptr) {
        refresh(stacks_panel_->scroll_view());
    }
    if (breakpoints_panel_ != nullptr) {
        refresh(breakpoints_panel_->scroll_view());
    }
    if (watches_panel_ != nullptr) {
        refresh(watches_panel_->scroll_view());
    }
    if (include_source) {
        refresh(source_scroll_view_);
    }
    refresh(console_scroll_view_);
    if (network_panel_ != nullptr) {
        network_panel_->refresh_scroll_content();
    }
}

void DebugApp::maybe_start_launch() {
    if (!terminal_ready_for_session_ || launch_complete_handled_ || program_path_.empty()) {
        return;
    }
    if (launch_posted_) {
        return;
    }
    launch_posted_ = true;
    cached_thread_stack_contents_.clear();
    prefetch_program_source_highlight();
    if (resolved_launch_json_.has_value() && !resolved_launch_json_->empty()) {
        session_io_->start_launch(*resolved_launch_json_);
    } else if (mode_ == SessionMode::Mock) {
        session_io_->start_launch("{\"program\":\"" + program_path_ + "\"}");
    }
}

void DebugApp::prefetch_program_source_highlight() {
    const std::string source_path = preferred_program_source_path(program_path_);
    if (source_path.empty() || session_io_ == nullptr) {
        return;
    }

    const std::string text = read_file_or_empty(source_path);
    if (text.empty()) {
        return;
    }

    if (cached_source_text_.empty()) {
        cached_source_path_ = source_path;
        cached_source_text_ = text;
        if (source_panel_ != nullptr) {
            source_panel_->set_file_line_count(std::max(1, count_file_lines(text)));
            ensure_source_plain_lines();
        }
    }

    const int line_count = std::min(kMaxHighlightLinesPerRequest, kHighlightLineMargin + 24);
    session_io_->request_highlight(language_from_path(source_path), text, 1, line_count);
}

bool DebugApp::update_connecting_spinner() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_spinner_update_ < std::chrono::milliseconds(200)) {
        return false;
    }
    last_spinner_update_ = now;

    static constexpr char kSpinner[] = "|/-\\";
    const char* spinner_adapter_label = "debug adapter";
    if (mode_ == SessionMode::Mock) {
        spinner_adapter_label = "mock session";
    } else if (launch_ui_.is_rr_backend) {
        spinner_adapter_label = "rr replay";
    } else if (!launch_ui_.adapter_label.empty()) {
        spinner_adapter_label = launch_ui_.adapter_label.c_str();
    }
    const std::string message = std::string("Connecting to ") + spinner_adapter_label + "… "
                              + kSpinner[static_cast<std::size_t>(spinner_frame_++ % 4)];
    if (message == model_.status_message) {
        return false;
    }
    model_.status_message = message;
    return true;
}

void DebugApp::handle_launch_complete() {
    launch_complete_handled_ = true;

    if (session_io_->is_active()) {
        model_.connection_state = ConnectionState::Connected;
        if (model_.connection_state == ConnectionState::Connected && model_.session_state.empty()) {
            model_.status_message =
                mode_ == SessionMode::Mock ? "Mock session ready" : "Connected";
        }
        if (mode_ == SessionMode::Rust && !model_.network_capture_live) {
            model_.network_show_mock_fallback = true;
            if (model_.status_message == "Connected") {
                model_.status_message = "Connected — network capture unavailable, showing demo traffic";
            }
        }
        refresh_network_panel();
    } else {
        model_.connection_state = ConnectionState::Failed;
        model_.status_message = launch_error_.empty() ? "Failed to launch debug session" : launch_error_;
    }

    reclaim_terminal_for_ui();
    sync_ui_from_model();

    if (session_io_->is_active()) {
        if (const std::string preview_path = preferred_program_source_path(program_path_);
            model_.source_path.empty() && !preview_path.empty() && model_.execution_path.empty()) {
            open_source_file(preview_path, 1, false);
        }
        maybe_follow_execution();
        maybe_request_scope_variables();
        maybe_request_dap_panel_data();
        maybe_request_source_highlight();
        normalize_breakpoint_path_keys();
        apply_exception_filter_defaults();
        if (is_session_stopped()) {
            flush_breakpoints_to_session();
            breakpoints_flushed_after_launch_ = true;
            maybe_request_scope_variables();
            maybe_request_dap_panel_data();
            resolve_watches_from_locals();
        } else if (model_.session_state == "exited" || model_.session_state == "Exited") {
            model_.status_message =
                "Program exited before stopping — press Restart, or rebuild: "
                "examples/native/build.sh";
        }
        sync_breakpoints_list_panel();
        sync_breakpoints_to_panel();
        if (breakpoints_flushed_after_launch_) {
            restore_watch_input_state();
        }
        request_full_screen_refresh();
    }
}

void DebugApp::apply_snapshot_json_payload(const std::string& json) {
    const std::string previous_state = model_.session_state;
    if (step_in_selection_active()) {
        cancel_step_in_selection();
    }
    step_in_targets_pending_ = false;
    model_.apply_snapshot_json(json);
    if (model_.debug_process_ids.empty() && model_.session_state == "exited") {
        process_metrics_sampler_.reset();
    }
    remember_scope_names_from_model();
    remember_threads_from_model();
    if (launch_ui_.line_buffered_console) {
        model_.supports_function_breakpoints = true;
    }
    apply_exception_filter_defaults();
    if (is_session_stopped()) {
        pending_scope_paths_.clear();
        scope_variables_fetch_pending_ = false;
    }
    if (!model_.execution_path.empty() && model_.execution_path.rfind("dap:source:", 0) != 0) {
        const std::string resolved = resolve_debugger_source_path(model_.execution_path, program_path_);
        if (!resolved.empty()) {
            model_.execution_path = resolved;
        }
    }
    ++snapshot_generation_;
    scope_variables_fetch_signature_.clear();
    scope_variables_signature_.clear();
    if (const std::string preview_path = preferred_program_source_path(program_path_);
        model_.source_path.empty() && !preview_path.empty() && model_.execution_path.empty()) {
        open_source_file(preview_path, 1, false);
    }
    apply_breakpoint_hits_from_snapshot_json(json);
    if (is_session_stopped()) {
        if (model_.stop_reason == "exception" && previous_state == "running") {
            navigate_to_user_stop_frame();
        }
        handle_exception_info_from_snapshot();
        sync_execution_location_ui();
        record_breakpoint_hit();
        maybe_finish_ephemeral_catch_skip();
        refresh_breakpoint_hit_counts_from_session();
    } else {
        last_logged_exception_key_.clear();
    }
    sync_breakpoints_list_panel();
    mark_all_panels_dirty();
    request_full_screen_refresh();
    if (previous_state == "disconnected" && is_session_stopped()) {
        reclaim_terminal_for_ui();
    }
    if (is_session_stopped() && restart_pending_) {
        normalize_breakpoint_path_keys();
        flush_breakpoints_to_session();
        breakpoints_flushed_after_launch_ = true;
        restart_pending_ = false;
    } else if (is_session_stopped() && launch_complete_handled_ && !breakpoints_flushed_after_launch_) {
        normalize_breakpoint_path_keys();
        flush_breakpoints_to_session();
        breakpoints_flushed_after_launch_ = true;
    }
}

void DebugApp::apply_network_json_payload(const std::string& json) {
    if (compose_send_pending_) {
        NetworkExchange compose_result;
        if (apply_compose_send_json(compose_result, json)) {
            compose_send_pending_ = false;
            model_.network_session.exchanges.push_back(compose_result);
            model_.network_capture_live = true;
            model_.network_show_mock_fallback = false;
            if (network_panel_ != nullptr) {
                network_panel_->set_compose_pending(false);
                network_panel_->set_compose_result(compose_result);
                network_panel_->set_view(NetworkPanelView::Traffic);
            }
            if (compose_result.state == NetworkExchangeState::Dropped) {
                model_.status_message = "Request failed: " + compose_result.response_body;
            } else {
                model_.status_message = "Sent " + compose_result.method + " " + compose_result.path;
                if (compose_result.status_code > 0) {
                    model_.status_message += " -> " + std::to_string(compose_result.status_code);
                }
            }
            sync_status_bar();
            refresh_network_panel();
            if (network_panel_ != nullptr) {
                network_panel_->select_last_exchange();
            }
            request_repaint();
            return;
        }
        compose_send_pending_ = false;
        if (network_panel_ != nullptr) {
            network_panel_->set_compose_pending(false);
        }
    }

    const bool changed = apply_network_json(model_, json);
    if (!changed) {
        return;
    }
    refresh_network_panel();
    request_repaint();
}

void DebugApp::refresh_network_panel() {
    if (network_panel_ == nullptr) {
        return;
    }

    auto live_traffic_session = [](const NetworkMockSession& session) {
        NetworkMockSession filtered = session;
        std::erase_if(filtered.exchanges, [](const NetworkExchange& exchange) {
            return exchange.origin == NetworkExchangeOrigin::Demo;
        });
        return filtered;
    };

    const char* force_mock = std::getenv("TUI_DEBUG_NETWORK_MOCK");
    if ((mode_ == SessionMode::Mock ||
         (force_mock != nullptr && force_mock[0] != '\0' && std::strcmp(force_mock, "0") != 0)) &&
        !model_.network_capture_live) {
        network_panel_->set_session(make_demo_network_session());
        if (network_select_last_on_refresh_) {
            network_panel_->select_last_exchange();
            network_select_last_on_refresh_ = false;
        }
        return;
    }

    if (model_.network_capture_live && !model_.network_show_mock_fallback) {
        network_panel_->set_session(live_traffic_session(model_.network_session));
        if (network_select_last_on_refresh_) {
            network_panel_->select_last_exchange();
            network_select_last_on_refresh_ = false;
        }
        return;
    }

    if (model_.network_show_mock_fallback) {
        network_panel_->set_session(make_demo_network_session());
        if (network_select_last_on_refresh_) {
            network_panel_->select_last_exchange();
            network_select_last_on_refresh_ = false;
        }
        return;
    }

    network_panel_->set_session(live_traffic_session(model_.network_session));
    if (network_select_last_on_refresh_) {
        network_panel_->select_last_exchange();
        network_select_last_on_refresh_ = false;
    }
}

void DebugApp::apply_console_json_payload(const std::string& json) {
    const std::size_t before = model_.console_lines.size();
    if (!apply_console_json(model_, json)) {
        return;
    }

    if (console_panel_ != nullptr && model_.console_lines.size() > before) {
        std::string output;
        for (std::size_t i = before; i < model_.console_lines.size(); ++i) {
            output += model_.console_lines[i].text;
        }
        console_panel_->feed_output(output);
        console_synced_line_count_ = model_.console_lines.size();

        if (!output.empty() && (output.find(": ") != std::string::npos || output.back() == ':')) {
            model_.focus = Focus::Console;
            apply_focus();
        }
    }
}

void DebugApp::refresh_scope_rows() {
    refresh_all_scope_slots();
}

void DebugApp::restore_expanded_scope_children() {
    if (session_io_ == nullptr) {
        return;
    }

    for (const std::string& path : expanded_scope_paths_) {
        if (pending_scope_paths_.count(path) > 0) {
            continue;
        }

        const std::optional<std::int64_t> variables_reference = resolve_scope_path_reference(model_, path);
        if (!variables_reference.has_value() || *variables_reference <= 0) {
            continue;
        }

        const auto cached = model_.scope_variables.find(*variables_reference);
        if (cached != model_.scope_variables.end() && !cached->second.empty()) {
            continue;
        }

        pending_scope_paths_.insert(path);
        session_io_->request_variable_children(*variables_reference, path);
    }
}

void DebugApp::apply_variable_children_payload(std::int64_t variables_reference, const std::string& path,
                                               const std::string& json, bool success) {
    pending_scope_paths_.erase(path);
    if (success) {
        model_.scope_variables[variables_reference] = parse_variables_json(json);
        apply_scope_value_overrides();
    }
    refresh_scope_rows();
    restore_expanded_scope_children();
    request_repaint();
}

void DebugApp::toggle_scope_row_expand(std::uint64_t slot_id, int row_index) {
    SidebarSlot* slot = slot_by_id(slot_id);
    if (slot == nullptr || slot->scopes == nullptr || slot->scopes->has_active_inline_edit()) {
        return;
    }
    if (row_index < 0 || row_index >= static_cast<int>(slot->cached_scope_row_meta.size())) {
        return;
    }

    const ScopeVariableRowMeta& row_meta = slot->cached_scope_row_meta[static_cast<std::size_t>(row_index)];
    if (row_meta.is_scope_section) {
        if (collapsed_scope_sections_.count(row_meta.scope_section_name) > 0) {
            collapsed_scope_sections_.erase(row_meta.scope_section_name);
        } else {
            collapsed_scope_sections_.insert(row_meta.scope_section_name);
        }
        refresh_scope_rows();
        request_repaint();
        return;
    }
    if (row_meta.expand_path.empty() || row_meta.expand_reference <= 0) {
        return;
    }

    const std::string& path = row_meta.expand_path;
    if (expanded_scope_paths_.count(path) > 0) {
        expanded_scope_paths_.erase(path);
        refresh_scope_rows();
        request_repaint();
        return;
    }

    expanded_scope_paths_.insert(path);
    const std::int64_t ref = row_meta.expand_reference;
    const auto cached = model_.scope_variables.find(ref);
    if (cached == model_.scope_variables.end() || cached->second.empty()) {
        pending_scope_paths_.insert(path);
        if (session_io_ != nullptr) {
            session_io_->request_variable_children(ref, path);
        }
    }
    refresh_scope_rows();
    request_repaint();
}

void DebugApp::apply_scope_variables_payload(const std::string& signature, const std::string& json) {
    model_.scope_variables.clear();
    if (!apply_scope_variables_batch(model_, signature, json)) {
        scope_variables_fetch_pending_ = false;
        return;
    }
    scope_variables_signature_ = signature;
    scope_variables_fetch_signature_ = signature;
    scope_variables_fetch_pending_ = false;
    if (variable_set_in_flight_) {
        apply_scope_value_overrides();
        return;
    }
    refresh_scope_rows();
    restore_expanded_scope_children();
    resolve_watches_from_locals();
    request_repaint();
}

void DebugApp::handle_session_event(const SessionIoEvent& event) {
    switch (event.kind) {
    case SessionIoEventKind::LaunchFinished:
        if (!event.success) {
            launch_error_ = event.payload.empty() ? "Failed to launch debug session" : event.payload;
        }
        break;
    case SessionIoEventKind::SnapshotJson:
    case SessionIoEventKind::PollJson:
        if (event.success) {
            apply_snapshot_json_payload(event.payload);
        } else if (event.kind == SessionIoEventKind::PollJson &&
                   model_.session_state != "disconnected") {
            model_.connection_state = ConnectionState::Failed;
            model_.status_message = event.detail.empty() ? "Session poll failed" : event.detail;
        }
        break;
    case SessionIoEventKind::ConsoleJson:
        if (event.success) {
            apply_console_json_payload(event.payload);
        }
        break;
    case SessionIoEventKind::NetworkJson:
        if (event.success) {
            apply_network_json_payload(event.payload);
        } else {
            compose_send_pending_ = false;
            if (network_panel_ != nullptr) {
                network_panel_->set_compose_pending(false);
            }
            if (!event.payload.empty()) {
                model_.status_message = "Request failed: " + event.payload;
                sync_status_bar();
            }
        }
        break;
    case SessionIoEventKind::ScopeVariablesReady:
        if (event.success) {
            apply_scope_variables_payload(event.detail, event.payload);
        } else {
            scope_variables_fetch_pending_ = false;
        }
        break;
    case SessionIoEventKind::VariableChildrenReady:
        apply_variable_children_payload(event.scope_ref, event.detail, event.payload, event.success);
        break;
    case SessionIoEventKind::MemoryReady: {
        SidebarSlot* slot = slot_by_id(event.slot_id);
        if (slot != nullptr && event.success) {
#if TUI_DEBUG_UI_HAS_NLOHMANN_JSON
            const auto parsed = nlohmann::json::parse(event.payload, nullptr, false);
            if (parsed.is_object()) {
                const std::string address = parsed.value("address", std::string{});
                const std::string data = parsed.value("data", std::string{});
                slot->cached_memory_reference = event.detail;
                slot->cached_memory_read_offset = event.memory_offset;
                slot->cached_memory_hex_data = data;
                slot->cached_memory_response_address = address;
                slot->cached_memory_lines = format_memory_hex_dump(address, data);
                if (slot->memory != nullptr && !address.empty()) {
                    slot->memory->set_address_value(address);
                }
                sync_memory_slot(*slot);
            }
#endif
        } else if (slot != nullptr) {
            slot->cached_memory_lines = {"(failed to read memory)"};
            sync_memory_slot(*slot);
        }
        request_repaint();
        break;
    }
    case SessionIoEventKind::DisassemblyReady: {
        SidebarSlot* slot = slot_by_id(event.slot_id);
        if (slot != nullptr && event.success) {
            const DisassemblyLineStyle style =
                slot->config.type == SidebarPanelType::DisassemblyBytes ? DisassemblyLineStyle::Bytes
                                                                        : DisassemblyLineStyle::Asm;
            slot->cached_memory_reference = event.detail;
            slot->cached_disassembly_lines = format_disassembly_lines(event.payload, style);
            sync_disassembly_slot(*slot);
        } else if (slot != nullptr) {
            slot->cached_disassembly_lines = {"(failed to disassemble)"};
            sync_disassembly_slot(*slot);
        }
        request_repaint();
        break;
    }
    case SessionIoEventKind::RuntimeSourceReady: {
        SidebarSlot* slot = slot_by_id(event.slot_id);
        if (slot != nullptr && event.success) {
            slot->cached_runtime_source_reference = event.source_reference;
            slot->cached_runtime_source_lines = format_runtime_source_lines(event.payload);
            sync_runtime_source_slot(*slot);
        } else if (slot != nullptr) {
            slot->cached_runtime_source_lines = {"(failed to fetch runtime source)"};
            sync_runtime_source_slot(*slot);
        }
        request_repaint();
        break;
    }
    case SessionIoEventKind::WriteMemoryFinished:
        if (event.success) {
            model_.status_message = "Memory written at offset " + std::to_string(event.memory_offset);
            maybe_request_dap_panel_data();
        } else if (!event.payload.empty()) {
            model_.status_message = "Memory write failed: " + event.payload;
        } else {
            model_.status_message =
                "Failed to write memory — region may be read-only; try Addr: &g_buffer or a data address";
        }
        memory_write_active_ = false;
        sync_status_bar();
        break;
    case SessionIoEventKind::SourceReady:
        if (event.success && event.detail == pending_source_fetch_key_) {
            cached_source_text_ = event.payload;
            pending_source_fetch_key_.clear();
            if (active_source_file_tab_ >= 0 &&
                active_source_file_tab_ < static_cast<int>(source_file_tabs_.size())) {
                source_file_tabs_[static_cast<std::size_t>(active_source_file_tab_)].cached_text = event.payload;
            }
            if (source_panel_ != nullptr) {
                source_panel_->set_file_line_count(std::max(1, count_file_lines(cached_source_text_)));
                const int line = source_panel_->cursor_line() > 0 ? source_panel_->cursor_line() : 1;
                if (source_panel_->lines().empty()) {
                    const int line_count =
                        uses_full_file_source() ? source_panel_->file_line_count()
                                                : std::max(1, source_viewport_height());
                    const int first_line = uses_full_file_source() ? 1 : std::max(1, line - line_count / 2);
                    apply_instant_source_viewport(first_line, line_count);
                }
                if (launch_complete_handled_) {
                    maybe_request_source_highlight();
                }
                source_panel_->mark_dirty();
            }
            if (source_scroll_view_ != nullptr) {
                source_scroll_view_->refresh_content();
                source_scroll_view_->mark_dirty();
            }
            sync_status_bar();
        } else if (!event.success) {
            pending_source_fetch_key_.clear();
            model_.status_message = "Failed to load adapter source";
            sync_status_bar();
        }
        break;
    case SessionIoEventKind::HighlightReady:
        if (event.success) {
            apply_highlight_payload(event.highlight_first_line, event.highlight_line_count, event.payload);
        }
        highlight_request_first_line_ = -1;
        highlight_request_line_count_ = -1;
        break;
    case SessionIoEventKind::CommandFinished:
        if (!event.success) {
            model_.status_message =
                humanize_execution_command_error(event.payload, event.detail, model_.session_state);
            if (is_execution_control_command(event.payload.c_str()) && model_.session_state == "running") {
                model_.session_state = "stopped";
            }
            if (std::strcmp(event.payload.c_str(), "restart") == 0) {
                restart_pending_ = false;
            }
        } else if (std::strcmp(event.payload.c_str(), "restart") == 0) {
            reclaim_terminal_for_ui();
            restart_pending_ = false;
            if (is_session_stopped()) {
                model_.status_message = model_.stop_reason.empty()
                                            ? "Stopped"
                                            : "Stopped (" + model_.stop_reason + ")";
            } else if (model_.session_state == "running") {
                model_.status_message = "Running";
            } else {
                model_.status_message = "Restarting…";
            }
        } else if (std::strcmp(event.payload.c_str(), "disconnect") == 0) {
            reclaim_terminal_for_ui();
        } else if (command_state_synced_via_snapshot(event.payload.c_str())) {
            // SnapshotJson already refreshed session_state/status_message.
        } else {
            const std::string status = command_status_message(event.payload.c_str());
            apply_execution_command_started(event.payload.c_str());
            model_.status_message = status;
            if ((event.payload == "step_over" || event.payload == "next") && model_.session_state == "running") {
                model_.focus = Focus::Console;
                apply_focus();
                model_.status_message = "Running — type in Console";
            }
        }
        break;
    case SessionIoEventKind::CompletionsFinished:
        handle_repl_completions_event(event);
        break;
    case SessionIoEventKind::EvaluateFinished:
        if (memory_address_eval_slot_id_ != 0) {
            const std::uint64_t slot_id = memory_address_eval_slot_id_;
            memory_address_eval_slot_id_ = 0;
            if (event.success) {
                if (const std::optional<std::string> address = extract_address_from_eval_result(event.payload)) {
                    navigate_memory_view(slot_id, *address, 0);
                } else {
                    model_.status_message = "Could not parse address from: " + event.payload;
                    sync_status_bar();
                }
            } else {
                model_.status_message =
                    event.payload.empty() ? "Address evaluation failed" : "Address error: " + event.payload;
                sync_status_bar();
            }
        } else if (!event.detail.empty()) {
            std::string entry = "> " + event.detail + "\n= ";
            if (event.success) {
                entry += event.payload;
                model_.status_message = "REPL: " + event.detail;
            } else {
                entry += "error: " + event.payload;
                model_.status_message =
                    event.payload.empty() ? "REPL evaluation failed" : "REPL error: " + event.payload;
            }
            model_.repl_history.push_back(std::move(entry));
            sync_repl_panel();
        }
        if (is_session_stopped()) {
            resolve_watches_from_locals();
        } else {
            sync_watches_panel();
        }
        break;
    case SessionIoEventKind::SetVariableFinished:
        variable_set_in_flight_ = false;
        if (event.success) {
            std::string updated_value = pending_variable_value_;
            if (const std::optional<std::string> parsed = parse_set_variable_result_value(event.payload)) {
                updated_value = *parsed;
            }
            if (!event.detail.empty() && !updated_value.empty()) {
                scope_value_overrides_[event.detail] = updated_value;
                patch_local_variable_value(event.detail, updated_value);
            }
            resolve_watches_from_locals();
            model_.status_message = event.detail.empty() ? "Variable updated" : "Updated " + event.detail;
        } else {
            model_.status_message =
                event.payload.empty() ? "Failed to set variable" : "Set variable failed: " + event.payload;
        }
        editing_variable_name_.clear();
        editing_variables_reference_ = 0;
        pending_variable_value_.clear();
        scope_input_draft_.clear();
        scope_input_focused_ = false;
        if (scopes_panel_ != nullptr) {
            scopes_panel_->clear_inline_edit();
            sync_scopes_list_panel();
            if (scopes_panel_->list_widget() != nullptr) {
                scopes_panel_->list_widget()->set_focused(true);
            }
        }
        model_.focus = Focus::Scopes;
        apply_focus();
        sync_status_bar();
        request_repaint();
        break;
    case SessionIoEventKind::BreakpointsFinished:
        model_.status_message =
            event.success ? "Breakpoints updated" : (event.detail.empty() ? "Breakpoint update failed" : event.detail);
        if (event.success && !event.payload.empty() && !event.detail.empty()) {
            apply_breakpoint_hit_counts(event.detail, event.payload);
            sync_breakpoints_list_panel();
            mark_all_panels_dirty();
        }
        if (source_panel_ != nullptr) {
            source_panel_->mark_dirty();
        }
        break;
    case SessionIoEventKind::DataBreakpointInfoReady:
        handle_data_breakpoint_info_payload(event);
        break;
    case SessionIoEventKind::DataBreakpointsFinished:
        model_.status_message = event.success
                                    ? "Data breakpoints updated"
                                    : (event.detail.empty() ? "Data breakpoint update failed" : event.detail);
        if (event.success) {
            sync_breakpoints_list_panel();
            mark_all_panels_dirty();
        }
        sync_status_bar();
        break;
    case SessionIoEventKind::FunctionBreakpointsFinished:
        model_.status_message = event.success
                                    ? "Function breakpoints updated"
                                    : (event.detail.empty() ? "Function breakpoint update failed" : event.detail);
        if (event.success) {
            sync_breakpoints_list_panel();
            mark_all_panels_dirty();
        }
        sync_status_bar();
        break;
    case SessionIoEventKind::ExceptionBreakpointsFinished:
        model_.status_message = event.success
                                    ? "Exception breakpoints updated"
                                    : (event.detail.empty() ? "Exception breakpoint update failed" : event.detail);
        if (event.success) {
            sync_breakpoints_list_panel();
            mark_all_panels_dirty();
        }
        sync_status_bar();
        break;
    case SessionIoEventKind::StepInTargetsReady:
        handle_step_in_targets_payload(event);
        break;
    case SessionIoEventKind::GotoTargetsReady:
        handle_goto_targets_payload(event);
        break;
    }
}

void DebugApp::poll_session() {
    ensure_ui_built();
    tick_process_metrics();
    if (controls_bar_ != nullptr) {
        controls_bar_->tick_hover();
    }
    sync_overlay_mouse_tracking();
    process_pending_file_tree_open();

    if (consume_sigint_quit_request()) {
        if (app_ != nullptr) {
            app_->quit();
        }
        return;
    }

    maybe_start_launch();

    bool needs_sync = false;
    SessionIoEvent event;
    while (session_io_->try_pop_event(event)) {
        handle_session_event(event);
        needs_sync = true;
    }

    if (launch_complete_handled_) {
        tick_repl_completion();
    }

    if (!launch_complete_handled_) {
        if (session_io_->launch_finished()) {
            handle_launch_complete();
            needs_sync = true;
        } else if (model_.connection_state == ConnectionState::Connecting && update_connecting_spinner()) {
            needs_sync = true;
        }
        if (needs_sync) {
            sync_ui_from_model();
            if (watch_input_focused_) {
                restore_watch_input_state();
            }
        }
        return;
    }

    if (!needs_sync) {
        return;
    }

    if (model_.connection_state == ConnectionState::Connected && !variable_set_in_flight_) {
        maybe_request_scope_variables();
        maybe_request_dap_panel_data();
    }

    sync_ui_from_model();
    if (watch_input_focused_) {
        restore_watch_input_state();
    }
}

void DebugApp::sync_ui_from_model() {
    sync_controls_bar();
    maybe_apply_reverse_continue_hint();
    sync_status_bar();
    if (console_panel_ != nullptr) {
        console_panel_->set_input_active(console_input_active());
    }
    if (!variable_set_in_flight_) {
        for_each_sidebar_slot([&](SidebarSlot& slot) {
            if (slot.config.type != SidebarPanelType::Variables) {
                return;
            }
            std::vector<ScopeVariableRowMeta> meta;
            std::vector<std::string> scope_rows =
                build_scope_rows(model_, expanded_scope_paths_, pending_scope_paths_, collapsed_scope_sections_, meta,
                                 slot.config.scope_filter);
            const bool next_has_values = scope_rows_include_variables(scope_rows);
            const bool cached_has_values = scope_rows_include_variables(slot.cached_scope_rows);
            const bool keep_stale_values = cached_has_values && !next_has_values &&
                                           (scope_variables_fetch_pending_ || !is_session_stopped() ||
                                            !model_.scope_variables.empty());

            if (!keep_stale_values && scope_rows != slot.cached_scope_rows) {
                slot.cached_scope_rows = std::move(scope_rows);
                slot.cached_scope_row_meta = std::move(meta);
            }
            sync_scope_slot(slot);
        });
        apply_scope_value_overrides();
    }
    sync_threads_list_panel();
    refresh_all_dap_panel_slots();
    if (is_session_stopped()) {
        resolve_watches_from_locals();
    } else {
        sync_watches_panel();
    }
    sync_source_stack_title();
    sync_breakpoints_to_panel();
    if (source_panel_ != nullptr) {
        const bool viewing_execution =
            !model_.execution_path.empty() &&
            viewing_same_source(model_.source_path, model_.source_reference, model_.execution_path,
                                model_.execution_source_reference);
        const int next_execution_line =
            viewing_execution && model_.execution_line > 0 ? static_cast<int>(model_.execution_line) : 0;
        if (source_panel_->execution_line() != next_execution_line) {
            source_panel_->set_execution_line(next_execution_line);
        }
        if (viewing_execution && model_.execution_line > 0 &&
            model_.execution_line != cached_follow_line_) {
            cached_follow_line_ = model_.execution_line;
            if (follow_execution_) {
                scroll_source_to_line(static_cast<int>(model_.execution_line));
                cached_highlight_first_line_ = -1;
                highlight_request_first_line_ = -1;
                maybe_request_source_highlight();
            } else {
                source_panel_->mark_dirty();
            }
        }
    }
    if (console_panel_ != nullptr && model_.console_lines.size() > console_synced_line_count_) {
        std::string output;
        for (std::size_t i = console_synced_line_count_; i < model_.console_lines.size(); ++i) {
            output += model_.console_lines[i].text;
        }
        console_panel_->feed_output(std::move(output));
        console_synced_line_count_ = model_.console_lines.size();
    }
}

void DebugApp::sync_status_bar() {
    if (status_bar_ == nullptr) {
        return;
    }
    const std::string status_text = format_status_bar_text();
    if (status_text != cached_status_bar_text_) {
        cached_status_bar_text_ = status_text;
        status_bar_->set_text(status_text);
    }
}

int DebugApp::source_viewport_height() const {
    if (source_scroll_view_ != nullptr && source_scroll_view_->bounds().height > 0) {
        return source_scroll_view_->bounds().height;
    }
    if (source_panel_ != nullptr) {
        const int height = source_panel_->viewport_height();
        if (height > 0 && (source_scroll_view_ == nullptr || source_scroll_view_->bounds().height > 0)) {
            return height;
        }
    }
    if (cached_source_viewport_height_ > 0) {
        return cached_source_viewport_height_;
    }
    return 24;
}

bool DebugApp::uses_full_file_source() const {
    return source_panel_ != nullptr && source_panel_->file_line_count() > 0 &&
           source_panel_->file_line_count() <= kFullFileSourceLineThreshold;
}

void DebugApp::apply_instant_source_viewport(int first_line, int line_count) {
    if (source_panel_ == nullptr || cached_source_text_.empty() || line_count <= 0 || first_line < 1) {
        return;
    }

    const auto lines = build_plain_viewport_lines(cached_source_text_, first_line, line_count);
    if (lines.empty()) {
        return;
    }

    source_panel_->set_lines(lines);
    if (!uses_full_file_source()) {
        source_panel_->set_scroll_offset(0);
    } else if (source_scroll_view_ != nullptr) {
        source_scroll_view_->refresh_content();
    }
}

void DebugApp::invalidate_scope_variables() {
    scope_variables_signature_.clear();
    scope_variables_fetch_signature_.clear();
    scope_variables_fetch_pending_ = false;
    model_.scope_variables.clear();
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        slot.cached_scope_rows.clear();
        slot.cached_scope_row_meta.clear();
    });
    expanded_scope_paths_.clear();
    collapsed_scope_sections_.clear();
    known_scope_names_.clear();
    known_threads_.clear();
    pending_scope_paths_.clear();
}

void DebugApp::remember_threads_from_model() {
    for (const ThreadInfo& thread : model_.threads) {
        const auto existing = std::find_if(known_threads_.begin(), known_threads_.end(),
                                           [&](const ThreadInfo& known) { return known.id == thread.id; });
        if (existing == known_threads_.end()) {
            known_threads_.push_back(thread);
        } else if (!thread.name.empty()) {
            existing->name = thread.name;
        }
    }
}

std::vector<ThreadInfo> DebugApp::available_thread_menu_threads() const {
    std::vector<ThreadInfo> threads;
    std::unordered_set<std::int64_t> seen;
    auto add_thread = [&](const ThreadInfo& thread) {
        if (thread.id <= 0 || seen.count(thread.id) > 0) {
            return;
        }
        seen.insert(thread.id);
        threads.push_back(thread);
    };

    for (const ThreadInfo& thread : model_.threads) {
        add_thread(thread);
    }
    for (const ThreadInfo& thread : known_threads_) {
        add_thread(thread);
    }
    for (const ThreadStackContent& thread : cached_thread_stack_contents_) {
        ThreadInfo info{};
        info.id = thread.id;
        info.name = thread.name;
        add_thread(info);
    }
    return threads;
}

void DebugApp::remember_scope_names_from_model() {
    for (const ScopeInfo& scope : model_.scopes) {
        if (scope.name.empty()) {
            continue;
        }
        if (std::find(known_scope_names_.begin(), known_scope_names_.end(), scope.name) == known_scope_names_.end()) {
            known_scope_names_.push_back(scope.name);
        }
    }
}

std::vector<std::string> DebugApp::available_scope_names() const {
    std::vector<std::string> names;
    std::unordered_set<std::string> seen;
    auto add_name = [&](const std::string& name) {
        if (name.empty() || seen.count(name) > 0) {
            return;
        }
        seen.insert(name);
        names.push_back(name);
    };

    for (const ScopeInfo& scope : model_.scopes) {
        add_name(scope.name);
    }
    for (const std::string& name : known_scope_names_) {
        add_name(name);
    }
    for (const std::string& path : expanded_scope_paths_) {
        const std::size_t separator = path.find(kScopePathSeparator);
        add_name(separator == std::string::npos ? path : path.substr(0, separator));
    }
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        for (const SidebarSlot& slot : layout_tree_.node(leaf_id).leaf.slots) {
            for (const ScopeVariableRowMeta& row : slot.cached_scope_row_meta) {
                if (row.is_scope_section) {
                    add_name(row.scope_section_name);
                }
            }
        }
    }
    return names;
}

void DebugApp::request_scope_variables_refresh() {
    scope_variables_signature_.clear();
    scope_variables_fetch_signature_.clear();
    scope_variables_fetch_pending_ = false;
    maybe_request_scope_variables();
    maybe_request_dap_panel_data();
}

void DebugApp::patch_local_variable_value(const std::string& name, const std::string& value) {
    for (auto& [_, variables] : model_.scope_variables) {
        for (VariableInfo& variable : variables) {
            if (variable.name == name) {
                variable.value = value;
            }
        }
    }
    for (VariableInfo& variable : model_.variables) {
        if (variable.name == name) {
            variable.value = value;
        }
    }

    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type == SidebarPanelType::Variables) {
            patch_scope_row_value(slot.cached_scope_rows, name, value);
        }
    });
    refresh_all_scope_slots();
}

void DebugApp::apply_scope_value_overrides() {
    if (scope_value_overrides_.empty()) {
        return;
    }

    for (auto& [_, variables] : model_.scope_variables) {
        for (VariableInfo& variable : variables) {
            if (const auto it = scope_value_overrides_.find(variable.name); it != scope_value_overrides_.end()) {
                variable.value = it->second;
            }
        }
    }
    for (VariableInfo& variable : model_.variables) {
        if (const auto it = scope_value_overrides_.find(variable.name); it != scope_value_overrides_.end()) {
            variable.value = it->second;
        }
    }
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type != SidebarPanelType::Variables) {
            return;
        }
        for (const auto& [name, value] : scope_value_overrides_) {
            patch_scope_row_value(slot.cached_scope_rows, name, value);
        }
    });
}

void DebugApp::clear_scope_value_overrides() { scope_value_overrides_.clear(); }

std::string DebugApp::build_scope_variables_signature() const {
    std::string signature = std::to_string(snapshot_generation_);
    signature += '|';
    signature += model_.session_state;
    signature += '|';
    signature += std::to_string(model_.current_line);
    for (const ScopeInfo& scope : model_.scopes) {
        signature += '|';
        signature += scope.name;
        signature += ':';
        signature += std::to_string(scope.variables_reference);
    }
    return signature;
}

void DebugApp::maybe_request_scope_variables() {
    if (!has_active_session() || !is_session_stopped()) {
        return;
    }

    const std::string signature = build_scope_variables_signature();
    if (signature == scope_variables_fetch_signature_ || scope_variables_fetch_pending_) {
        return;
    }

    std::vector<std::pair<std::int64_t, std::string>> scopes;
    scopes.reserve(model_.scopes.size());
    for (const ScopeInfo& scope : model_.scopes) {
        if (scope.variables_reference <= 0) {
            continue;
        }
        scopes.emplace_back(scope.variables_reference, scope.name);
    }
    if (scopes.empty()) {
        return;
    }

    scope_variables_fetch_pending_ = true;
    session_io_->request_scope_variables(signature, scopes);
}

namespace {

std::optional<StackFrameInfo> active_stack_frame(const DebugUiModel& model) {
    if (!model.stack_frames.empty()) {
        return model.stack_frames.front();
    }
    for (const ThreadStackInfo& stack : model.thread_stacks) {
        if (model.stopped_thread_id > 0 && stack.thread_id != model.stopped_thread_id) {
            continue;
        }
        if (!stack.frames.empty()) {
            return stack.frames.front();
        }
    }
    return std::nullopt;
}

std::string memory_reference_for_frame(const StackFrameInfo& frame) {
    if (!frame.instruction_pointer_reference.empty()) {
        return frame.instruction_pointer_reference;
    }
    return {};
}

std::string trim_whitespace(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

}  // namespace

void DebugApp::maybe_request_dap_panel_data() {
    if (!has_active_session() || !is_session_stopped() || session_io_ == nullptr) {
        return;
    }

    const std::optional<StackFrameInfo> frame = active_stack_frame(model_);
    const std::string memory_reference = frame.has_value() ? memory_reference_for_frame(*frame) : std::string{};
    const std::int64_t source_reference =
        frame.has_value() && frame->source_reference > 0
            ? frame->source_reference
            : (model_.execution_source_reference > 0 ? model_.execution_source_reference : 0);

    for_each_sidebar_slot([&](SidebarSlot& slot) {
        switch (slot.config.type) {
        case SidebarPanelType::Memory:
            if (model_.supports_read_memory_request) {
                const std::string& view_reference =
                    !slot.memory_view_reference.empty() ? slot.memory_view_reference : memory_reference;
                if (!view_reference.empty()) {
                    request_memory_fetch_for_slot(slot, view_reference, slot.cached_memory_read_offset);
                }
            }
            break;
        case SidebarPanelType::DisassemblyAsm:
        case SidebarPanelType::DisassemblyBytes:
            if (model_.supports_disassemble_request && !memory_reference.empty()) {
                session_io_->request_disassembly_fetch(memory_reference, 0, 0, 64, slot.config.id);
            }
            break;
        case SidebarPanelType::RuntimeSource:
            if (source_reference > 0) {
                session_io_->request_runtime_source_fetch(source_reference, slot.config.id);
            }
            break;
        default:
            break;
        }
    });
}

void DebugApp::sync_memory_slot(SidebarSlot& slot) {
    if (slot.memory == nullptr || slot.config.type != SidebarPanelType::Memory) {
        return;
    }
    std::string title = panel_type_label(SidebarPanelType::Memory);
    if (!slot.cached_memory_reference.empty()) {
        title += " @ " + slot.cached_memory_reference;
    }
    if (model_.supports_write_memory_request) {
        title += " (rw)";
    }
    slot.memory->set_writable(model_.supports_write_memory_request && is_session_stopped());
    slot.memory->set_title(std::move(title));
    if (slot.cached_memory_lines.empty()) {
        if (!is_session_stopped()) {
            slot.memory->set_lines({"(program running — pause to inspect memory)"});
        } else if (!model_.supports_read_memory_request) {
            slot.memory->set_lines({"(adapter does not support readMemory)"});
        } else if (slot.cached_memory_reference.empty()) {
            slot.memory->set_lines({"(no instruction pointer — try lldb-dap)"});
        } else if (model_.supports_write_memory_request) {
            slot.memory->set_lines(
                {"(loading memory…) — g: address, /: search, Enter/e: edit row"});
        } else {
            slot.memory->set_lines({"(loading memory…) — g: address, /: search"});
        }
    } else {
        slot.memory->set_lines(slot.cached_memory_lines);
    }
}

void DebugApp::sync_disassembly_slot(SidebarSlot& slot) {
    if (slot.disassembly == nullptr ||
        (slot.config.type != SidebarPanelType::DisassemblyAsm &&
         slot.config.type != SidebarPanelType::DisassemblyBytes)) {
        return;
    }
    slot.disassembly->set_title(panel_type_label(slot.config.type));
    if (slot.cached_disassembly_lines.empty()) {
        if (!is_session_stopped()) {
            slot.disassembly->set_lines({"(program running — pause to disassemble)"});
        } else if (!model_.supports_disassemble_request) {
            slot.disassembly->set_lines({"(adapter does not support disassemble)"});
        } else if (slot.cached_memory_reference.empty()) {
            slot.disassembly->set_lines({"(no instruction pointer — try lldb-dap)"});
        } else {
            slot.disassembly->set_lines({"(loading disassembly…)"});
        }
    } else {
        slot.disassembly->set_lines(slot.cached_disassembly_lines);
    }
}

void DebugApp::sync_runtime_source_slot(SidebarSlot& slot) {
    if (slot.runtime_source == nullptr || slot.config.type != SidebarPanelType::RuntimeSource) {
        return;
    }
    std::string title = panel_type_label(SidebarPanelType::RuntimeSource);
    if (slot.cached_runtime_source_reference > 0) {
        title += " #" + std::to_string(slot.cached_runtime_source_reference);
    }
    slot.runtime_source->set_title(std::move(title));
    if (slot.cached_runtime_source_lines.empty()) {
        if (!is_session_stopped()) {
            slot.runtime_source->set_lines({"(program running — pause to fetch source)"});
        } else if (slot.cached_runtime_source_reference <= 0) {
            slot.runtime_source->set_lines(
                {"(no runtime sourceReference — open a frame with adapter-provided source)"});
        } else {
            slot.runtime_source->set_lines({"(loading runtime source…)"});
        }
    } else {
        slot.runtime_source->set_lines(slot.cached_runtime_source_lines);
    }
}

void DebugApp::refresh_all_dap_panel_slots() {
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        switch (slot.config.type) {
        case SidebarPanelType::Memory:
            sync_memory_slot(slot);
            break;
        case SidebarPanelType::DisassemblyAsm:
        case SidebarPanelType::DisassemblyBytes:
            sync_disassembly_slot(slot);
            break;
        case SidebarPanelType::RuntimeSource:
            sync_runtime_source_slot(slot);
            break;
        case SidebarPanelType::FileTree:
            sync_file_tree_slot(slot);
            break;
        case SidebarPanelType::Resources:
            sync_resources_slot(slot);
            break;
        default:
            break;
        }
    });
}

void DebugApp::sync_resources_slot(SidebarSlot& slot) {
    if (slot.resources == nullptr || slot.config.type != SidebarPanelType::Resources) {
        return;
    }
    slot.resources->set_title(panel_type_label(SidebarPanelType::Resources));
    slot.resources->set_tracked_pids(model_.debug_process_ids);
    slot.resources->set_metrics(process_metrics_sampler_.latest(), process_metrics_sampler_);
}

void DebugApp::sync_network_slot(SidebarSlot& slot) {
    static_cast<void>(slot);
    refresh_network_panel();
}

void DebugApp::wire_network_panel(NetworkPanel& panel) {
    panel.set_on_message([this](const std::string& message) {
        model_.status_message = message;
        sync_status_bar();
    });
    panel.set_on_compose_send([this](const NetworkComposeTemplate& item) {
        if (session_io_ == nullptr) {
            model_.status_message = "No active session for Compose send";
            sync_status_bar();
            return false;
        }
        compose_send_pending_ = true;
        network_panel_->set_compose_pending(true);
        session_io_->post_network_compose_send(item.method, item.url, item.headers, item.body, item.timeout_ms);
        model_.status_message = "Waiting for " + item.method + " " + item.url + " (" +
                                std::to_string(item.timeout_ms / 1000) + "s timeout)…";
        sync_status_bar();
        return true;
    });
    panel.set_on_action([this](const std::string& action, const NetworkExchange& exchange) {
        if (action == "forward") {
            model_.status_message = "Forwarded " + exchange.method + " " + exchange.path;
        } else if (action == "drop") {
            model_.status_message = "Dropped " + exchange.method + " " + exchange.path;
        } else if (action == "edit") {
            model_.status_message = "Edit request: " + exchange.method + " " + exchange.path + " (mock)";
        } else if (action == "replay") {
            model_.status_message = "Replay " + exchange.method + " " + exchange.path + " (mock)";
        }
        sync_status_bar();
    });
}

void DebugApp::tick_process_metrics() {
    if (!launch_complete_handled_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (last_process_metrics_tick_ != std::chrono::steady_clock::time_point{} &&
        now - last_process_metrics_tick_ < std::chrono::milliseconds(500)) {
        return;
    }
    last_process_metrics_tick_ = now;

    const bool changed = process_metrics_sampler_.tick(model_.debug_process_ids);
    if (!changed) {
        return;
    }

    bool updated = false;
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type != SidebarPanelType::Resources || slot.resources == nullptr) {
            return;
        }
        sync_resources_slot(slot);
        updated = true;
    });
    if (updated) {
        request_repaint();
    }
}

void DebugApp::request_memory_fetch_for_slot(SidebarSlot& slot, const std::string& reference, std::int64_t offset) {
    if (session_io_ == nullptr || reference.empty()) {
        return;
    }
    session_io_->request_memory_fetch(reference, offset, 256, slot.config.id);
}

void DebugApp::navigate_memory_view(std::uint64_t slot_id, const std::string& reference, std::int64_t offset) {
    SidebarSlot* slot = slot_by_id(slot_id);
    if (slot == nullptr || reference.empty()) {
        return;
    }

    slot->memory_view_reference = reference;
    slot->cached_memory_read_offset = offset;
    slot->memory_search_matches.clear();
    slot->memory_search_match_index = -1;
    if (slot->memory != nullptr) {
        slot->memory->set_address_value(reference);
        slot->memory->blur_toolbar_inputs();
    }
    request_memory_fetch_for_slot(*slot, reference, offset);
    model_.status_message = "Reading memory at " + reference;
    sync_status_bar();
}

void DebugApp::submit_memory_address(std::uint64_t slot_id, const std::string& input) {
    memory_toolbar_focused_ = false;
    const std::string trimmed = trim_whitespace(input);
    if (trimmed.empty()) {
        return;
    }
    if (!is_session_stopped()) {
        model_.status_message = "Pause the program before navigating memory";
        sync_status_bar();
        return;
    }

    if (const std::optional<std::string> reference = parse_memory_address_reference(trimmed)) {
        navigate_memory_view(slot_id, *reference, 0);
        return;
    }

    if (session_io_ == nullptr) {
        return;
    }
    const std::optional<StackFrameInfo> frame = active_stack_frame(model_);
    if (!frame.has_value()) {
        model_.status_message = "No stack frame for address evaluation";
        sync_status_bar();
        return;
    }

    memory_address_eval_slot_id_ = slot_id;
    session_io_->post_evaluate(trimmed, frame->id, "repl");
    model_.status_message = "Evaluating address: " + trimmed;
    sync_status_bar();
}

void DebugApp::submit_memory_search(std::uint64_t slot_id, const std::string& query, bool forward) {
    SidebarSlot* slot = slot_by_id(slot_id);
    if (slot == nullptr || slot->memory == nullptr) {
        return;
    }

    const std::string trimmed = trim_whitespace(query);
    if (trimmed.empty()) {
        return;
    }

    if (trimmed != slot->memory_search_query) {
        slot->memory_search_match_index = -1;
    }
    slot->memory_search_query = trimmed;
    slot->memory_search_matches = find_memory_search_matches(slot->cached_memory_hex_data, trimmed);
    if (slot->memory_search_matches.empty()) {
        slot->memory_search_match_index = -1;
        model_.status_message = "No matches for \"" + trimmed + "\" in loaded memory";
        sync_status_bar();
        request_repaint();
        return;
    }

    if (forward) {
        slot->memory_search_match_index =
            (slot->memory_search_match_index + 1) % static_cast<int>(slot->memory_search_matches.size());
    } else {
        slot->memory_search_match_index =
            slot->memory_search_match_index <= 0
                ? static_cast<int>(slot->memory_search_matches.size()) - 1
                : slot->memory_search_match_index - 1;
    }

    const std::size_t byte_offset =
        slot->memory_search_matches[static_cast<std::size_t>(slot->memory_search_match_index)];
    const int row = static_cast<int>(byte_offset / 16);

    memory_focus_slot_id_ = slot_id;
    memory_toolbar_focused_ = false;
    slot->memory->blur_toolbar_inputs();
    model_.focus = Focus::Memory;
    apply_focus();
    slot->memory->set_selected_row(row);
    model_.status_message = "Match " + std::to_string(slot->memory_search_match_index + 1) + "/" +
                            std::to_string(slot->memory_search_matches.size()) + " at byte " +
                            std::to_string(byte_offset);
    sync_status_bar();
    request_repaint();
}

void DebugApp::wire_memory_panel(MemoryPanel& panel, SidebarSlot& slot) {
    panel.set_on_refresh([this, slot_id = slot.config.id]() {
        if (SidebarSlot* target = slot_by_id(slot_id); target != nullptr) {
            const std::string& reference = !target->memory_view_reference.empty()
                                               ? target->memory_view_reference
                                               : target->cached_memory_reference;
            if (!reference.empty() && session_io_ != nullptr) {
                request_memory_fetch_for_slot(*target, reference, target->cached_memory_read_offset);
            } else {
                maybe_request_dap_panel_data();
            }
        }
    });
    panel.set_on_address_submit([this, slot_id = slot.config.id](const std::string& value) {
        submit_memory_address(slot_id, value);
    });
    panel.set_on_search_submit([this, slot_id = slot.config.id](const std::string& value) {
        submit_memory_search(slot_id, value, true);
    });
    panel.set_on_activate([this, slot_id = slot.config.id](int row) { begin_memory_row_edit(slot_id, row); });
    panel.set_on_submit([this, slot_id = slot.config.id](int row, const std::string& hex) {
        submit_memory_write(slot_id, row, hex);
    });
    panel.set_on_inline_edit_cancel([this]() {
        memory_write_active_ = false;
        memory_write_row_ = -1;
        model_.status_message = "Memory edit cancelled";
        sync_status_bar();
    });
    panel.set_on_toolbar_interact([this, slot_id = slot.config.id]() { activate_memory_toolbar(slot_id); });
}

void DebugApp::begin_memory_row_edit(std::uint64_t slot_id, int row_index) {
    if (!model_.supports_write_memory_request) {
        model_.status_message = "Adapter does not support writeMemory";
        sync_status_bar();
        return;
    }
    if (!is_session_stopped()) {
        model_.status_message = "Pause the program before editing memory";
        sync_status_bar();
        return;
    }

    SidebarSlot* slot = slot_by_id(slot_id);
    if (slot == nullptr || slot->memory == nullptr || slot->cached_memory_reference.empty()) {
        return;
    }
    if (row_index < 0 || row_index >= static_cast<int>(slot->cached_memory_lines.size())) {
        return;
    }

    const std::optional<std::string> hex = extract_memory_row_hex(slot->cached_memory_lines[row_index]);
    if (!hex.has_value()) {
        model_.status_message = "Select a memory row to edit";
        sync_status_bar();
        return;
    }

    memory_focus_slot_id_ = slot_id;
    memory_toolbar_focused_ = false;
    memory_write_slot_id_ = slot_id;
    memory_write_row_ = row_index;
    memory_write_active_ = true;
    slot->memory->begin_row_edit(row_index, format_hex_for_edit(*hex));
    model_.focus = Focus::Memory;
    model_.status_message = "Edit hex bytes — Enter to write, Esc to cancel";
    apply_focus();
    sync_status_bar();
    request_repaint();
}

void DebugApp::submit_memory_write(std::uint64_t slot_id, int row_index, const std::string& hex_input) {
    SidebarSlot* slot = slot_by_id(slot_id);
    if (slot == nullptr || session_io_ == nullptr || slot->cached_memory_reference.empty()) {
        return;
    }

    const std::optional<std::string> hex = normalize_hex_input(hex_input);
    if (!hex.has_value()) {
        model_.status_message = "Invalid hex — use pairs like de ad be ef";
        sync_status_bar();
        if (slot->memory != nullptr) {
            slot->memory->begin_row_edit(row_index, hex_input);
        }
        return;
    }

    const std::int64_t offset = slot->cached_memory_read_offset + static_cast<std::int64_t>(row_index) * 16;
    memory_write_slot_id_ = slot_id;
    memory_write_row_ = row_index;
    memory_write_offset_ = offset;
    memory_write_active_ = true;
    if (slot->memory != nullptr) {
        slot->memory->clear_inline_edit();
    }
    session_io_->post_write_memory(slot->cached_memory_reference, offset, *hex, slot_id);
    model_.status_message = "Writing memory at offset " + std::to_string(offset) + "…";
    sync_status_bar();
}

void DebugApp::wire_disassembly_panel(DisassemblyPanel& panel, SidebarSlot& slot) {
    panel.set_on_refresh([this, slot_id = slot.config.id]() {
        if (SidebarSlot* target = slot_by_id(slot_id); target != nullptr) {
            if (!target->cached_memory_reference.empty() && session_io_ != nullptr) {
                session_io_->request_disassembly_fetch(target->cached_memory_reference, 0, 0, 64, slot_id);
            } else {
                maybe_request_dap_panel_data();
            }
        }
    });
}

void DebugApp::wire_runtime_source_panel(RuntimeSourcePanel& panel, SidebarSlot& slot) {
    panel.set_on_refresh([this, slot_id = slot.config.id]() {
        if (SidebarSlot* target = slot_by_id(slot_id); target != nullptr) {
            if (target->cached_runtime_source_reference > 0 && session_io_ != nullptr) {
                session_io_->request_runtime_source_fetch(target->cached_runtime_source_reference, slot_id);
            } else {
                maybe_request_dap_panel_data();
            }
        }
    });
}

void DebugApp::wire_file_tree_panel(FileTreePanel& panel) {
    panel.set_on_open_file([this](const std::filesystem::path& path) {
        pending_file_tree_open_ = path.string();
    });
}

void DebugApp::process_pending_file_tree_open() {
    if (!pending_file_tree_open_.has_value()) {
        return;
    }

    const std::string path = *pending_file_tree_open_;
    pending_file_tree_open_.reset();
    open_source_file(path, 1, false);
    model_.status_message = "Opened " + relative_display(workspace_root_, path);
    sync_status_bar();
    request_repaint();
}

void DebugApp::sync_file_tree_slot(SidebarSlot& slot) {
    if (slot.file_tree == nullptr || slot.config.type != SidebarPanelType::FileTree) {
        return;
    }
    workspace_root_ = workspace_root_for_program(program_path_);
    workspace_files_ = list_source_files(workspace_root_);
    slot.file_tree->set_workspace(workspace_root_, workspace_files_);
}

void DebugApp::sync_file_tree_slots() {
    for_each_sidebar_slot([&](SidebarSlot& slot) { sync_file_tree_slot(slot); });
}

int DebugApp::highlight_line_count() const {
    const int viewport = std::max(1, source_viewport_height());
    return std::min(kMaxHighlightLinesPerRequest, viewport + kHighlightLineMargin);
}

void DebugApp::apply_highlight_payload(int first_line, int line_count, const std::string& json) {
    if (source_panel_ == nullptr || json.empty()) {
        return;
    }

    try {
        if (uses_full_file_source() && source_scroll_view_ != nullptr && highlight_request_scroll_y_ >= 0 &&
            source_scroll_view_->scroll_y() != highlight_request_scroll_y_) {
            const int requested_first = highlight_request_scroll_y_ + 1;
            const int current_first = source_scroll_view_->scroll_y() + 1;
            if (requested_first != current_first) {
                highlight_request_first_line_ = -1;
                cached_highlight_first_line_ = -1;
                maybe_request_source_highlight();
                return;
            }
        }

        const auto lines = parse_highlight_json(json);
        if (lines.empty()) {
            return;
        }

        if (uses_full_file_source()) {
            ensure_source_plain_lines();
            source_panel_->merge_highlighted_lines(lines);
        } else {
            source_panel_->set_lines(lines);
            source_panel_->set_scroll_offset(0);
            if (source_scroll_view_ != nullptr) {
                source_scroll_view_->refresh_content();
            }
        }

        source_panel_->mark_dirty();
        if (source_scroll_view_ != nullptr) {
            source_scroll_view_->mark_dirty();
            cached_highlight_scroll_y_ = source_scroll_view_->scroll_y();
        }
        cached_highlight_first_line_ = first_line;
        cached_highlight_line_count_ = line_count;
        if (source_scroll_view_ != nullptr && source_scroll_view_->bounds().height > 0) {
            cached_source_viewport_height_ = source_scroll_view_->bounds().height;
        }
        if (!divider_drag_active_) {
            mark_source_view_dirty();
        }
    } catch (const std::exception&) {
    }
}

void DebugApp::scroll_source_to_line(int line) {
    if (source_panel_ == nullptr || line <= 0) {
        return;
    }

    source_panel_->set_cursor_line(line);
    if (source_scroll_view_ != nullptr) {
        const int viewport = std::max(1, source_viewport_height());
        const int current_scroll = source_scroll_view_->scroll_y();
        // Keep the viewport put while the line is comfortably visible and only
        // re-center once it leaves a small margin. Unconditional centering
        // scrolled on every debugger step, which repainted every source row.
        constexpr int kMargin = 2;
        const int first_visible = current_scroll + 1;
        const int last_visible = current_scroll + viewport;
        const bool visible = line >= first_visible + kMargin && line <= last_visible - kMargin;
        if (!visible) {
            const int scroll_y = std::max(0, line - viewport / 2 - 1);
            source_scroll_view_->scroll_to(0, scroll_y);
            source_scroll_view_->refresh_content();
            cached_highlight_scroll_y_ = scroll_y;
        }
    } else {
        source_panel_->ensure_cursor_visible();
    }
}

void DebugApp::maybe_refresh_source_highlight_for_scroll() {
    if (divider_drag_active_ || source_layout_settling_ || source_scroll_view_ == nullptr ||
        !uses_full_file_source()) {
        return;
    }
    if (highlight_request_first_line_ >= 0) {
        return;
    }
    if (source_scroll_view_->bounds().height <= 0) {
        return;
    }

    const int scroll_y = source_scroll_view_->scroll_y();
    const int viewport_height = source_scroll_view_->bounds().height;
    if (scroll_y == cached_highlight_scroll_y_ && viewport_height == cached_source_viewport_height_) {
        return;
    }

    cached_highlight_scroll_y_ = scroll_y;
    cached_source_viewport_height_ = viewport_height;
    cached_highlight_first_line_ = -1;
    highlight_request_first_line_ = -1;
    highlight_request_scroll_y_ = -1;

    maybe_request_source_highlight();
}

int DebugApp::highlight_first_line() const {
    const int line_budget = highlight_line_count();
    if (source_panel_ == nullptr) {
        return 1;
    }

    if (uses_full_file_source()) {
        if (source_scroll_view_ != nullptr) {
            return std::max(1, source_scroll_view_->scroll_y() + 1);
        }
        const int cursor = std::max(1, source_panel_->cursor_line());
        return std::max(1, cursor - line_budget / 2);
    }

    int first_line = std::max(1, source_panel_->cursor_line() - line_budget / 2);
    const auto& visible_lines = source_panel_->lines();
    if (!visible_lines.empty()) {
        const int scroll = source_panel_->scroll_offset();
        if (scroll >= 0 && scroll < static_cast<int>(visible_lines.size())) {
            first_line = visible_lines[static_cast<std::size_t>(scroll)].line_number;
        }
    }
    const int cursor_line = source_panel_->cursor_line();
    if (cursor_line < first_line || cursor_line >= first_line + line_budget) {
        first_line = std::max(1, cursor_line - line_budget / 2);
    }
    return first_line;
}

void DebugApp::ensure_source_plain_lines() {
    if (source_panel_ == nullptr || cached_source_text_.empty() || !uses_full_file_source()) {
        return;
    }

    const int file_lines = source_panel_->file_line_count();
    if (static_cast<int>(source_panel_->lines().size()) >= file_lines) {
        return;
    }

    const auto cached = source_plain_lines_cache_.find(cached_source_path_);
    if (cached != source_plain_lines_cache_.end() &&
        static_cast<int>(cached->second.size()) >= file_lines) {
        source_panel_->set_lines(cached->second);
        return;
    }

    const auto lines = build_plain_viewport_lines(cached_source_text_, 1, file_lines);
    if (lines.empty()) {
        return;
    }

    source_plain_lines_cache_[cached_source_path_] = lines;
    source_panel_->set_lines(lines);
}

void DebugApp::maybe_request_source_highlight() {
    if (divider_drag_active_ || source_layout_settling_ || source_panel_ == nullptr) {
        return;
    }
    if (source_scroll_view_ != nullptr && source_scroll_view_->bounds().height <= 0) {
        return;
    }

    const std::string source_key = source_cache_key(model_.source_path, model_.source_reference);
    if (source_key.empty()) {
        return;
    }

    if (source_key != cached_source_path_ || cached_source_reference_ != model_.source_reference) {
        return;
    }

    if (cached_source_text_.empty()) {
        return;
    }

    ensure_source_plain_lines();

    const int line_count = highlight_line_count();
    const int first_line = highlight_first_line();
    const bool full_file = uses_full_file_source();

    if (first_line == cached_highlight_first_line_ && line_count == cached_highlight_line_count_ &&
        cached_source_path_ == source_key) {
        return;
    }
    if (first_line == highlight_request_first_line_ && line_count == highlight_request_line_count_ &&
        cached_source_path_ == source_key) {
        return;
    }

    highlight_request_first_line_ = first_line;
    highlight_request_line_count_ = line_count;
    highlight_request_scroll_y_ =
        source_scroll_view_ != nullptr ? source_scroll_view_->scroll_y() : -1;

    if (source_panel_->lines().empty()) {
        if (!full_file) {
            apply_instant_source_viewport(first_line, line_count);
        } else {
            ensure_source_plain_lines();
        }
    }

    session_io_->request_highlight(language_from_path(cached_source_path_), cached_source_text_, first_line,
                                    line_count);
}

bool DebugApp::is_session_stopped() const {
    return model_.session_state.find("Stopped") != std::string::npos ||
           model_.session_state.find("stopped") != std::string::npos;
}

bool DebugApp::console_input_active() const {
    if (!has_active_session()) {
        return false;
    }
    if (model_.session_state == "exited" || model_.session_state == "Exited" ||
        model_.session_state == "disconnected") {
        return false;
    }
    return model_.session_state == "running" || is_session_stopped();
}

void DebugApp::finalize_text_cursor(tuinator::PaintContext& ctx) const {
    if (console_panel_ == nullptr) {
        ctx.canvas.set_text_cursor(std::nullopt);
        return;
    }
    console_panel_->paint_text_cursor(ctx);
}

void DebugApp::maybe_apply_reverse_continue_hint() {
    if (reverse_continue_hint_shown_ || mode_ == SessionMode::Mock ||
        !launch_ui_.show_reverse_continue_hint ||
        model_.supports_step_back || !is_session_stopped()) {
        return;
    }

    reverse_continue_hint_shown_ = true;
    model_.status_message =
        "Reverse continue unavailable on this system (LLDB trace). Step Over/Into/Out still work.";
}

void DebugApp::sync_controls_hover(tuinator::Point position) {
    if (controls_bar_ == nullptr) {
        return;
    }
    if (!controls_bar_->bounds().contains(position)) {
        controls_bar_->clear_hover();
    }
}

void DebugApp::sync_controls_bar() {
    if (controls_bar_ == nullptr) {
        return;
    }
    const bool active = has_active_session();
    const bool stopped = is_session_stopped();
    const bool ended = model_.session_state == "exited" || model_.session_state == "disconnected";
    if (controls_bar_->session_active() == active && controls_bar_->stopped() == stopped &&
        controls_bar_->session_ended() == ended) {
        // fall through — capabilities may still need syncing
    } else {
        controls_bar_->set_session_active(active);
        controls_bar_->set_stopped(stopped);
        controls_bar_->set_session_ended(ended);
    }
    controls_bar_->set_supports_step_back(mode_ == SessionMode::Mock ? true : model_.supports_step_back);
}

std::string DebugApp::format_status_bar_text() const {
    const int max_cols = app_ != nullptr ? std::max(20, app_->terminal_size().width) : 80;
    std::string suffix = " | follow: " + std::string(follow_execution_ ? "on" : "off");
    suffix += " | " + model_.connection_label() + " | focus: " + model_.focus_label();
    const int suffix_width = tuinator::text_display_width(suffix);

    std::string prefix = model_.status_message;
    if (!model_.source_path.empty()) {
        prefix += " | ";
        prefix += panel_title_from_path(model_.source_path);
    }
    if (source_panel_ != nullptr && source_panel_->cursor_line() > 0) {
        prefix += " | ln " + std::to_string(source_panel_->cursor_line());
    }
    if (is_session_stopped() && !model_.execution_path.empty() &&
        model_.execution_path != model_.source_path) {
        prefix += " | stopped @ " + panel_title_from_path(model_.execution_path) + ":" +
                  std::to_string(model_.execution_line);
    }

    if (tuinator::text_display_width(prefix) + suffix_width <= max_cols) {
        return prefix + suffix;
    }

    const int prefix_budget = std::max(0, max_cols - suffix_width - 3);
    if (prefix_budget <= 0) {
        return suffix.substr(0, static_cast<std::size_t>(max_cols));
    }

    const std::size_t prefix_bytes = tuinator::text_byte_length_for_width(prefix, prefix_budget);
    std::string clipped = prefix.substr(0, prefix_bytes);
    if (prefix_bytes < prefix.size()) {
        clipped += "...";
    }
    return clipped + suffix;
}

bool DebugApp::has_active_session() const {
    return model_.connection_state == ConnectionState::Connected && session_io_ != nullptr &&
           session_io_->is_active();
}

void DebugApp::bind_split_pane(ResizableSplitPane* split) {
    if (split == nullptr) {
        return;
    }

    split->set_on_drag_state_changed([this](bool dragging) {
        divider_drag_active_ = dragging;
        if (!dragging) {
            on_split_drag_ended();
        }
    });
}

void DebugApp::request_full_screen_refresh() {
    if (!ui_built_ || app_ == nullptr) {
        return;
    }

    // Repaint everything without clearing the terminal: repaint_all() goes
    // through the partial-render path, where the backend's cell diff reduces
    // output to the cells that actually changed. present() would force a
    // \033[2J clear first, which visibly flashed the whole screen on every
    // debugger step.
    app_->repaint_all();
}

void DebugApp::request_repaint() {
    if (!ui_built_) {
        return;
    }

    mark_all_panels_dirty();
}

void DebugApp::mark_source_view_dirty() {
    if (source_panel_ != nullptr) {
        source_panel_->mark_dirty();
    }
    if (source_scroll_view_ != nullptr) {
        source_scroll_view_->mark_dirty();
    }
}

void DebugApp::refresh_source_highlight_if_needed() {
    if (source_scroll_view_ != nullptr) {
        source_scroll_view_->refresh_content();
        if (source_scroll_view_->bounds().height > 0) {
            cached_source_viewport_height_ = source_scroll_view_->bounds().height;
            cached_highlight_scroll_y_ = source_scroll_view_->scroll_y();
        }
    }

    const int next_first_line = highlight_first_line();
    const int next_line_count = highlight_line_count();
    const bool highlight_window_changed = next_first_line != cached_highlight_first_line_ ||
                                          next_line_count != cached_highlight_line_count_;

    if (highlight_window_changed) {
        cached_highlight_first_line_ = -1;
        cached_highlight_line_count_ = -1;
        highlight_request_first_line_ = -1;
        highlight_request_line_count_ = -1;
        maybe_request_source_highlight();
    }

    mark_source_view_dirty();
}

void DebugApp::on_split_drag_ended() {
    apply_focus();
    refresh_scroll_views();

    source_layout_settling_ = true;
    refresh_source_highlight_if_needed();
    source_layout_settling_ = false;

    if (status_bar_ == nullptr) {
        sync_ui_from_model();
        return;
    }

    const std::string status_text = format_status_bar_text();
    if (status_text != cached_status_bar_text_) {
        cached_status_bar_text_ = status_text;
        status_bar_->set_text(status_text);
    }
    sync_ui_from_model();
}

namespace {

bool is_left_center_split(const LayoutTree& tree, LayoutNodeId split_id) {
    if (tree.empty()) {
        return false;
    }
    const LayoutNode& node = tree.node(split_id);
    if (node.kind != LayoutNode::Kind::Container ||
        node.container.orientation != tuinator::SplitOrientation::Horizontal) {
        return false;
    }
    const LayoutNodeId first = node.container.first;
    const LayoutNodeId second = node.container.second;
    return tree.subtree_contains_dock(first, PanelDock::Left) &&
           tree.subtree_contains_dock(second, PanelDock::Center);
}

bool is_default_bottom_root_split(const LayoutTree& tree, LayoutNodeId split_id) {
    if (split_id != tree.root()) {
        return false;
    }
    const LayoutNode& node = tree.node(split_id);
    if (node.kind != LayoutNode::Kind::Container ||
        node.container.orientation != tuinator::SplitOrientation::Vertical) {
        return false;
    }
    const LayoutNodeId first = node.container.first;
    const LayoutNodeId second = node.container.second;
    const bool first_main_area = (tree.subtree_contains_dock(first, PanelDock::Left) ||
                                tree.subtree_contains_dock(first, PanelDock::Center)) &&
                               !tree.subtree_contains_dock(first, PanelDock::Bottom);
    return first_main_area && tree.subtree_contains_dock(second, PanelDock::Bottom);
}

}  // namespace

void DebugApp::ensure_layout_tree_initialized() {
    if (layout_tree_.empty()) {
        layout_tree_.init_default_three_pane(model_.layout.sidebar_pct, model_.layout.bottom_pct);
    }
}

namespace {

std::optional<LayoutDockSpec> dock_spec_for(const AppConfig& config, PanelDock dock) {
    switch (dock) {
    case PanelDock::Left:
        return config.layout.left;
    case PanelDock::Center:
        return config.layout.center;
    case PanelDock::Bottom:
        return config.layout.bottom;
    }
    return std::nullopt;
}

}  // namespace

void DebugApp::ensure_default_leaf_slots(LayoutNodeId leaf_id) {
    std::vector<SidebarSlot>& slots = leaf_slots(leaf_id);
    if (!slots.empty()) {
        return;
    }
    const std::optional<PanelDock> dock = layout_tree_.node(leaf_id).leaf.dock;
    if (!dock.has_value()) {
        return;
    }
    if (layout_tree_.find_leaf_for_dock(*dock) != leaf_id) {
        return;
    }
    if (const std::optional<LayoutDockSpec> dock_spec = dock_spec_for(app_config_, *dock);
        dock_spec.has_value() && !dock_spec->panels.empty()) {
        init_slots_from_layout_spec(slots, *dock_spec, next_slot_id_);
        return;
    }

    switch (*dock) {
    case PanelDock::Left:
        init_default_sidebar_slots(slots);
        break;
    case PanelDock::Center:
        if (!has_source_panel()) {
            init_default_source_slots(slots);
        }
        break;
    case PanelDock::Bottom:
        init_default_bottom_slots(slots);
        break;
    }
}

LayoutNodeId DebugApp::dock_leaf_id(PanelDock dock) const {
    if (const std::optional<LayoutNodeId> leaf_id = layout_tree_.find_leaf_for_dock(dock); leaf_id.has_value()) {
        return *leaf_id;
    }
    const std::vector<LayoutNodeId> leaves = layout_tree_.leaf_ids();
    return leaves.empty() ? 0 : leaves.front();
}

std::vector<SidebarSlot>& DebugApp::leaf_slots(LayoutNodeId leaf_id) {
    return layout_tree_.node(leaf_id).leaf.slots;
}

StackedPane* DebugApp::layout_stack(LayoutNodeId leaf_id) {
    const auto it = layout_stacks_.find(leaf_id);
    if (it == layout_stacks_.end()) {
        return nullptr;
    }
    return it->second;
}

const StackedPane* DebugApp::layout_stack(LayoutNodeId leaf_id) const {
    const auto it = layout_stacks_.find(leaf_id);
    if (it == layout_stacks_.end()) {
        return nullptr;
    }
    return it->second;
}

int& DebugApp::leaf_stack_index(LayoutNodeId leaf_id) {
    return layout_tree_.node(leaf_id).leaf.active_index;
}

std::unique_ptr<StackedPane> DebugApp::build_stacked_pane_for_leaf(LayoutNodeId leaf_id) {
    const auto scroll_options = dap_theme_.scroll_view_options();
    ensure_default_leaf_slots(leaf_id);
    std::vector<SidebarSlot>& slots = leaf_slots(leaf_id);

    std::vector<StackedPane::Entry> entries;
    entries.reserve(slots.size());
    for (SidebarSlot& slot : slots) {
        ensure_sidebar_slot_panels(slot, scroll_options);
        auto widget = release_sidebar_slot_widget(slot);
        widget->set_flex(1);
        entries.push_back({slot.config.tab_label, std::move(widget)});
    }

    auto stack = std::make_unique<StackedPane>(std::move(entries), dap_theme_.panel_background, dap_theme_.label,
                                               dap_theme_.title_focused, dap_theme_.divider,
                                               dap_theme_.breakpoint_line_number);
    StackedPane* stack_ptr = stack.get();
    layout_stacks_[leaf_id] = stack_ptr;

    int& active_index = leaf_stack_index(leaf_id);
    active_index = std::clamp(active_index, 0, std::max(0, stack_ptr->count() - 1));
    stack_ptr->set_active_index(active_index);

    stack_ptr->set_add_action([this, leaf_id]() {
        if (StackedPane* pane = layout_stack(leaf_id); pane != nullptr) {
            const tuinator::Rect bounds = pane->bounds();
            show_add_panel_menu({bounds.x + std::max(0, bounds.width - 2), bounds.y + 1}, leaf_id);
        }
    });
    stack_ptr->set_rename_action([this, leaf_id](int index, const std::string& label) {
        rename_leaf_panel(leaf_id, index, label);
    });
    stack_ptr->set_pane_menu_action([this, leaf_id](tuinator::Point anchor) {
        show_pane_layout_menu(leaf_id, anchor);
    });
    stack_ptr->set_layout_drag_press_handler(
        [this, leaf_id](LayoutDragSourceKind kind, int tab_index, tuinator::Point position) {
            on_layout_drag_press(leaf_id, kind, tab_index, position);
        });
    stack_ptr->set_on_active_changed([this, leaf_id](int index) {
        leaf_stack_index(leaf_id) = index;
        if (SidebarSlot* slot = leaf_slot_at(leaf_id, index)) {
            model_.focus = focus_for_panel_type(slot->config.type);
        } else if (const std::optional<PanelDock> dock = layout_tree_.node(leaf_id).leaf.dock) {
            if (*dock == PanelDock::Center) {
                model_.focus = Focus::Source;
            } else if (*dock == PanelDock::Bottom) {
                model_.focus = Focus::Repl;
            }
        }
        if (model_.focus == Focus::Repl) {
            repl_input_focused_ = false;
        }
        if (SidebarSlot* slot = leaf_slot_at(leaf_id, index);
            slot != nullptr && slot->config.type == SidebarPanelType::Network && network_panel_ != nullptr) {
            network_panel_->refresh_scroll_content();
            network_panel_->mark_dirty();
        }
        update_active_panel_pointers();
        apply_focus();
        if (StackedPane* pane = layout_stack(leaf_id); pane != nullptr) {
            const std::optional<PanelDock> dock = layout_tree_.node(leaf_id).leaf.dock;
            if (dock == PanelDock::Left) {
                model_.status_message = "Left: " + pane->active_label() + " (" + std::to_string(index + 1) + "/" +
                                        std::to_string(pane->count()) + ") — ◄ ► or [ ]";
                sync_status_bar();
            } else if (dock == PanelDock::Bottom) {
                model_.status_message = "Bottom: " + pane->active_label() + " (" + std::to_string(index + 1) + "/" +
                                        std::to_string(pane->count()) + ") — ◄ ► or [ ]";
                sync_status_bar();
            }
        }
    });

    stack->set_flex(1);
    return stack;
}

std::unique_ptr<tuinator::Widget> DebugApp::build_layout_widget(LayoutNodeId node_id) {
    LayoutNode& node = layout_tree_.node(node_id);
    if (node.kind == LayoutNode::Kind::Leaf) {
        return build_stacked_pane_for_leaf(node_id);
    }

    const tuinator::Size term_size = app_->terminal_size();
    const int tray_body = bottom_tray_height(term_size.height, model_.layout.bottom_pct);
    const int main_h = main_area_height(term_size.height, tray_body);

    auto first = build_layout_widget(node.container.first);
    auto second = build_layout_widget(node.container.second);

    int first_size = 0;
    bool use_proportional = true;
    if (node.container.orientation == tuinator::SplitOrientation::Horizontal) {
        if (is_left_center_split(layout_tree_, node_id)) {
            first_size = sidebar_first_size(term_size.width, model_.layout.sidebar_pct);
            use_proportional = false;
        }
    } else if (is_default_bottom_root_split(layout_tree_, node_id)) {
        first_size = main_h;
        use_proportional = false;
    }

    auto split = std::make_unique<ResizableSplitPane>(
        std::move(first), std::move(second),
        tuinator::SplitPaneOptions{
            .orientation = node.container.orientation,
            .first_size = first_size,
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);

    ResizableSplitPane* split_ptr = split.get();
    if (use_proportional) {
        split_ptr->set_proportional_first_size(node.container.first_pct);
    }
    layout_splits_[node_id] = split_ptr;
    bind_split_pane(split_ptr);

    const LayoutNodeId split_id = node_id;
    split_ptr->set_on_first_size_changed([this, split_id](int /*first*/) {
        LayoutNode& split_node = layout_tree_.node(split_id);
        if (split_node.kind != LayoutNode::Kind::Container) {
            return;
        }
        const auto it = layout_splits_.find(split_id);
        if (it == layout_splits_.end() || it->second == nullptr) {
            return;
        }
        const tuinator::Rect bounds = it->second->bounds();
        const bool horizontal = split_node.container.orientation == tuinator::SplitOrientation::Horizontal;
        const int total = horizontal ? bounds.width : bounds.height;
        if (total <= 0) {
            return;
        }
        split_node.container.first_pct =
            static_cast<std::uint16_t>(std::clamp(it->second->first_size() * 100 / total, 1, 99));

        if (divider_drag_active_) {
            refresh_scroll_views();
        }

        if (is_left_center_split(layout_tree_, split_id)) {
            model_.layout.sidebar_pct = split_node.container.first_pct;
            if (!divider_drag_active_) {
                model_.status_message = "Left " + std::to_string(model_.layout.sidebar_pct) + "%";
                if (status_bar_ != nullptr) {
                    status_bar_->set_text(format_status_bar_text());
                }
            }
        } else if (is_default_bottom_root_split(layout_tree_, split_id)) {
            persist_split_size_as_pct(it->second, model_.layout.bottom_pct, false, true);
            if (!divider_drag_active_) {
                model_.status_message = "Bottom " + std::to_string(model_.layout.bottom_pct) + "%";
                if (status_bar_ != nullptr) {
                    status_bar_->set_text(format_status_bar_text());
                }
            }
        }
    });

    if (is_default_bottom_root_split(layout_tree_, split_id)) {
        content_split_ = split_ptr;
    }
    if (is_left_center_split(layout_tree_, split_id)) {
        main_row_split_ = split_ptr;
    }

    split->set_flex(1);
    return split;
}

void DebugApp::reset_layout_slot_widgets() {
    layout_stacks_.clear();
    layout_splits_.clear();
    main_row_split_ = nullptr;
    content_split_ = nullptr;

    scopes_panel_ = nullptr;
    watches_panel_ = nullptr;
    stacks_panel_ = nullptr;
    breakpoints_panel_ = nullptr;

    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        for (SidebarSlot& slot : leaf_slots(leaf_id)) {
            slot.scopes.reset();
            slot.watches.reset();
            slot.stacks.reset();
            slot.breakpoints.reset();
            slot.memory.reset();
            slot.disassembly.reset();
            slot.runtime_source.reset();
            slot.file_tree.reset();
            slot.resources.reset();
            if (slot.config.type == SidebarPanelType::Source || slot.config.type == SidebarPanelType::Repl ||
                slot.config.type == SidebarPanelType::Console || slot.config.type == SidebarPanelType::Network) {
                slot.shared_host.reset();
            }
        }
        ensure_default_leaf_slots(leaf_id);
    }
}

std::unique_ptr<tuinator::Widget> DebugApp::build_layout_content_widget() {
    auto content = build_layout_widget(layout_tree_.root());
    content->set_flex(1);
    return content;
}

void DebugApp::rebuild_layout_ui() {
    if (!ui_built_ || !debug_chrome_root_ready()) {
        build_ui();
        return;
    }

    capture_watch_input_state();
    capture_scope_input_state();
    capture_breakpoint_input_state();

    reset_layout_slot_widgets();
    replace_debug_chrome_content(build_layout_content_widget());

    update_active_panel_pointers();
    if (!watch_input_draft_.empty() && watches_panel_ != nullptr) {
        watches_panel_->set_inline_edit(-1, watch_input_draft_, "?");
        sync_watches_panel();
    }
    refresh_all_scope_slots();
    sync_source_stack_title();
    sync_ui_from_model();
    sync_controls_bar();
    sync_breakpoints_list_panel();
    refresh_scroll_views();
    restore_watch_input_state();
    restore_breakpoint_input_state();
    restore_scope_input_state();
    apply_focus();
    request_full_screen_refresh();
}

void DebugApp::clear_layout_menu_preview() {
    if (!layout_menu_preview_.has_value()) {
        return;
    }
    layout_menu_preview_.reset();
    request_repaint();
}

void DebugApp::set_layout_menu_pane_preview(LayoutNodeId source_leaf, LayoutNodeId target_leaf) {
    const tuinator::Rect bounds = layout_node_bounds(target_leaf);
    layout_menu_preview_ = LayoutMenuPreview{
        .source_leaf = source_leaf,
        .target =
            LayoutDropTarget{
                .anchor = target_leaf,
                .hover_leaf = target_leaf,
                .zone = LayoutDropZone::Center,
                .highlight = bounds,
                .spans_siblings = false,
            },
        .mode = LayoutMenuPreview::Mode::PaneHighlight,
        .swap = false,
    };
    request_repaint();
}

void DebugApp::set_layout_menu_placement_preview(LayoutNodeId source_leaf, const LayoutPlacementOption& option) {
    layout_menu_preview_ = LayoutMenuPreview{
        .source_leaf = source_leaf,
        .target = option.target,
        .mode = LayoutMenuPreview::Mode::Placement,
        .swap = option.swap,
    };
    request_repaint();
}

std::vector<LayoutNodeId> DebugApp::leaves_in_subtree(LayoutNodeId node_id) const {
    if (!layout_tree_.has_node(node_id)) {
        return {};
    }

    const LayoutNode& node = layout_tree_.node(node_id);
    if (node.kind == LayoutNode::Kind::Leaf) {
        return {node_id};
    }

    std::vector<LayoutNodeId> leaves = leaves_in_subtree(node.container.first);
    const std::vector<LayoutNodeId> right = leaves_in_subtree(node.container.second);
    leaves.insert(leaves.end(), right.begin(), right.end());
    std::sort(leaves.begin(), leaves.end());
    return leaves;
}

std::string DebugApp::layout_group_label(LayoutNodeId node_id) const {
    const std::vector<LayoutNodeId> leaves = leaves_in_subtree(node_id);
    if (leaves.empty()) {
        return "group";
    }
    if (leaves.size() == 1) {
        return layout_leaf_label(leaves.front());
    }

    std::string label;
    for (std::size_t i = 0; i < leaves.size(); ++i) {
        if (i > 0) {
            label += '+';
        }
        label += layout_leaf_label(leaves[i]);
    }
    return label;
}

LayoutDropTarget DebugApp::make_placement_drop_target(LayoutNodeId anchor, LayoutNodeId reference_leaf,
                                                      LayoutDropZone zone, bool spans_siblings) const {
    LayoutDropTarget target{
        .anchor = anchor,
        .hover_leaf = reference_leaf,
        .zone = zone,
        .spans_siblings = spans_siblings,
    };

    const tuinator::Rect content = layout_content_bounds();
    const int shell_x = std::max(4, std::min(10, content.width / 10));
    const int shell_y = std::max(4, std::min(10, content.height / 10));

    if (spans_siblings && anchor == layout_tree_.root() &&
        (zone == LayoutDropZone::Above || zone == LayoutDropZone::Below || zone == LayoutDropZone::Left ||
         zone == LayoutDropZone::Right)) {
        target.highlight = layout_drop_outer_shell_rect(content, zone, shell_x, shell_y);
        return target;
    }

    const tuinator::Rect anchor_bounds = layout_node_bounds(anchor);
    if (spans_siblings) {
        target.highlight = layout_drop_span_rect(anchor_bounds, zone);
        return target;
    }

    const tuinator::Rect reference_bounds = layout_node_bounds(reference_leaf);
    target.anchor = reference_leaf;
    target.highlight = zone == LayoutDropZone::Center ? reference_bounds : layout_drop_zone_rect(reference_bounds, zone);
    return target;
}

std::vector<LayoutPlacementOption> DebugApp::collect_move_placement_options(LayoutNodeId from_leaf,
                                                                            LayoutNodeId reference_leaf) const {
    struct PlacementKey {
        LayoutNodeId anchor = 0;
        LayoutDropZone zone = LayoutDropZone::Center;
        bool spans_siblings = false;
        bool swap = false;

        bool operator<(const PlacementKey& other) const {
            if (anchor != other.anchor) {
                return anchor < other.anchor;
            }
            if (zone != other.zone) {
                return zone < other.zone;
            }
            if (spans_siblings != other.spans_siblings) {
                return spans_siblings < other.spans_siblings;
            }
            return swap < other.swap;
        }
    };

    auto zone_prefix = [](LayoutDropZone zone) -> std::string {
        switch (zone) {
        case LayoutDropZone::Above:
            return "Above ";
        case LayoutDropZone::Below:
            return "Below ";
        case LayoutDropZone::Left:
            return "Left ";
        case LayoutDropZone::Right:
            return "Right ";
        case LayoutDropZone::Center:
            return "Into ";
        }
        return "";
    };

    auto can_place = [&](const LayoutDropTarget& target, bool swap) -> bool {
        if (swap) {
            return from_leaf != target.hover_leaf;
        }
        if (!swap && !target.spans_siblings && from_leaf == target.anchor) {
            return false;
        }
        return compute_insert_anchor(from_leaf, target.anchor, target.zone) != 0;
    };

    std::vector<LayoutPlacementOption> options;
    std::set<PlacementKey> seen;

    auto add_option = [&](const std::string& label, LayoutNodeId anchor, LayoutDropZone zone, bool spans_siblings,
                          bool swap) {
        const LayoutDropTarget target = make_placement_drop_target(anchor, reference_leaf, zone, spans_siblings);
        const PlacementKey key{target.anchor, target.zone, target.spans_siblings, swap};
        if (seen.count(key) > 0 || !can_place(target, swap)) {
            return;
        }
        seen.insert(key);
        options.push_back(LayoutPlacementOption{label, target, swap});
    };

    const std::string reference_label = layout_leaf_label(reference_leaf);
    add_option(zone_prefix(LayoutDropZone::Above) + reference_label, reference_leaf, LayoutDropZone::Above, false,
               false);
    add_option(zone_prefix(LayoutDropZone::Below) + reference_label, reference_leaf, LayoutDropZone::Below, false,
               false);
    add_option(zone_prefix(LayoutDropZone::Left) + reference_label, reference_leaf, LayoutDropZone::Left, false,
               false);
    add_option(zone_prefix(LayoutDropZone::Right) + reference_label, reference_leaf, LayoutDropZone::Right, false,
               false);
    if (from_leaf != reference_leaf) {
        add_option(zone_prefix(LayoutDropZone::Center) + reference_label, reference_leaf, LayoutDropZone::Center, false,
                   true);
    }

    const LayoutNodeId root = layout_tree_.root();
    for (std::optional<LayoutNodeId> parent = layout_tree_.parent_of(reference_leaf); parent.has_value();
         parent = layout_tree_.parent_of(*parent)) {
        if (*parent == root) {
            break;
        }
        const LayoutNode& parent_node = layout_tree_.node(*parent);
        if (parent_node.kind != LayoutNode::Kind::Container) {
            break;
        }

        const std::string group_label = layout_group_label(*parent);
        const bool horizontal = parent_node.container.orientation == tuinator::SplitOrientation::Horizontal;
        if (horizontal) {
            add_option(zone_prefix(LayoutDropZone::Above) + group_label, *parent, LayoutDropZone::Above, true, false);
            add_option(zone_prefix(LayoutDropZone::Below) + group_label, *parent, LayoutDropZone::Below, true, false);
        } else {
            add_option(zone_prefix(LayoutDropZone::Left) + group_label, *parent, LayoutDropZone::Left, true, false);
            add_option(zone_prefix(LayoutDropZone::Right) + group_label, *parent, LayoutDropZone::Right, true, false);
        }
    }

    const tuinator::Rect reference_bounds = layout_node_bounds(reference_leaf);
    const tuinator::Rect content = layout_content_bounds();
    if (layout_tree_.leaf_count() > 1 && reference_bounds.width > 0 && content.width > 0) {
        if (reference_bounds.y <= content.y) {
            add_option(zone_prefix(LayoutDropZone::Above) + "all", root, LayoutDropZone::Above, true, false);
        }
        if (reference_bounds.y + reference_bounds.height >= content.y + content.height - 1) {
            add_option(zone_prefix(LayoutDropZone::Below) + "all", root, LayoutDropZone::Below, true, false);
        }
        if (reference_bounds.x <= content.x) {
            add_option(zone_prefix(LayoutDropZone::Left) + "all", root, LayoutDropZone::Left, true, false);
        }
        if (reference_bounds.x + reference_bounds.width >= content.x + content.width - 1) {
            add_option(zone_prefix(LayoutDropZone::Right) + "all", root, LayoutDropZone::Right, true, false);
        }
    }

    return options;
}

void DebugApp::open_layout_context_menu(tuinator::Point anchor, std::vector<ContextMenu::Item> items,
                                        const std::function<void(int index)>& on_hover) {
    if (context_menu_ == nullptr) {
        return;
    }

    clear_layout_menu_preview();
    context_menu_->set_on_selection_changed([this, on_hover](int index) {
        if (index < 0) {
            clear_layout_menu_preview();
            return;
        }
        if (on_hover) {
            on_hover(index);
        }
    });
    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_pane_layout_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "Rename",
        [this, leaf_id]() {
            if (StackedPane* stack = layout_stack(leaf_id); stack != nullptr) {
                stack->begin_rename_active_tab();
            }
        },
    });
    if (layout_tree_.leaf_count() > 1) {
        items.push_back(ContextMenu::Item{
            "Delete",
            [this, leaf_id]() { delete_layout_pane(leaf_id); },
        });
        items.push_back(ContextMenu::Item{
            "Swap with ›",
            [this, leaf_id, anchor]() { show_swap_pane_menu(leaf_id, anchor); },
        });
        items.push_back(ContextMenu::Item{
            "Move ›",
            [this, leaf_id, anchor]() { show_move_pane_menu(leaf_id, anchor); },
        });
    }

    open_layout_context_menu(anchor, std::move(items), nullptr);
}

std::string DebugApp::layout_leaf_label(LayoutNodeId leaf_id) const {
    if (const auto it = layout_stacks_.find(leaf_id); it != layout_stacks_.end()) {
        if (StackedPane* stack = it->second; stack != nullptr && stack->count() > 0) {
            const std::string& active = stack->active_label();
            if (!active.empty()) {
                return active;
            }
        }
    }

    int pane_number = 1;
    for (LayoutNodeId id : layout_tree_.leaf_ids()) {
        if (id == leaf_id) {
            break;
        }
        ++pane_number;
    }
    return "Pane " + std::to_string(pane_number);
}

std::string DebugApp::layout_leaf_move_label(LayoutNodeId from_leaf, LayoutNodeId to_leaf) const {
    const std::optional<PanelDock> from_dock = layout_tree_.node(from_leaf).leaf.dock;
    const std::optional<PanelDock> to_dock = layout_tree_.node(to_leaf).leaf.dock;
    if (from_dock.has_value() && to_dock.has_value()) {
        if (*from_dock == PanelDock::Left) {
            if (*to_dock == PanelDock::Center) {
                return "Right";
            }
            if (*to_dock == PanelDock::Bottom) {
                return "Below";
            }
        }
        if (*from_dock == PanelDock::Center) {
            if (*to_dock == PanelDock::Left) {
                return "Left";
            }
            if (*to_dock == PanelDock::Bottom) {
                return "Below";
            }
        }
        if (*from_dock == PanelDock::Bottom) {
            if (*to_dock == PanelDock::Left) {
                return "Above (left)";
            }
            if (*to_dock == PanelDock::Center) {
                return "Above";
            }
        }
    }

    const std::optional<LayoutNodeId> shared_parent = layout_tree_.parent_of(from_leaf);
    if (shared_parent.has_value() && layout_tree_.parent_of(to_leaf) == shared_parent) {
        const LayoutNode& parent = layout_tree_.node(*shared_parent);
        if (parent.kind == LayoutNode::Kind::Container) {
            const bool horizontal = parent.container.orientation == tuinator::SplitOrientation::Horizontal;
            if (parent.container.first == from_leaf && parent.container.second == to_leaf) {
                return horizontal ? "Right" : "Below";
            }
            if (parent.container.first == to_leaf && parent.container.second == from_leaf) {
                return horizontal ? "Left" : "Above";
            }
        }
    }

    return layout_leaf_label(to_leaf);
}

void DebugApp::show_move_tab_menu(LayoutNodeId from_leaf, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    for (LayoutNodeId target_leaf : layout_tree_.leaf_ids()) {
        if (target_leaf == from_leaf) {
            continue;
        }
        items.push_back(ContextMenu::Item{
            layout_leaf_label(target_leaf),
            [this, from_leaf, target_leaf]() { move_active_panel_to_leaf(from_leaf, target_leaf); },
        });
    }
    if (items.empty()) {
        return;
    }

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_swap_pane_menu(LayoutNodeId from_leaf, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<LayoutNodeId> targets;
    std::vector<ContextMenu::Item> items;
    for (LayoutNodeId target_leaf : layout_tree_.leaf_ids()) {
        if (target_leaf == from_leaf) {
            continue;
        }
        targets.push_back(target_leaf);
        items.push_back(ContextMenu::Item{
            layout_leaf_label(target_leaf),
            [this, from_leaf, target_leaf]() { swap_layout_panes(from_leaf, target_leaf); },
        });
    }
    if (items.empty()) {
        return;
    }

    open_layout_context_menu(
        anchor, std::move(items),
        [this, from_leaf, targets](int index) {
            if (index < 0 || index >= static_cast<int>(targets.size())) {
                return;
            }
            set_layout_menu_pane_preview(from_leaf, targets[static_cast<std::size_t>(index)]);
        });
}

void DebugApp::show_move_pane_direction_menu(LayoutNodeId from_leaf, LayoutNodeId target_leaf,
                                             tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    const std::vector<LayoutPlacementOption> placements = collect_move_placement_options(from_leaf, target_leaf);
    if (placements.empty()) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.reserve(placements.size());
    for (const LayoutPlacementOption& placement : placements) {
        items.push_back(ContextMenu::Item{
            placement.label,
            [this, from_leaf, placement]() {
                if (placement.swap) {
                    swap_layout_panes(from_leaf, placement.target.hover_leaf);
                    return;
                }
                move_layout_pane_adjacent(from_leaf, placement.target.anchor, placement.target.zone);
            },
        });
    }

    open_layout_context_menu(
        anchor, std::move(items),
        [this, from_leaf, placements](int index) {
            if (index < 0 || index >= static_cast<int>(placements.size())) {
                return;
            }
            set_layout_menu_placement_preview(from_leaf, placements[static_cast<std::size_t>(index)]);
        });
}

void DebugApp::show_move_pane_menu(LayoutNodeId from_leaf, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<LayoutNodeId> targets;
    std::vector<ContextMenu::Item> items;
    for (LayoutNodeId target_leaf : layout_tree_.leaf_ids()) {
        if (target_leaf == from_leaf) {
            continue;
        }
        targets.push_back(target_leaf);
        items.push_back(ContextMenu::Item{
            layout_leaf_label(target_leaf),
            [this, from_leaf, target_leaf, anchor]() {
                show_move_pane_direction_menu(from_leaf, target_leaf, anchor);
            },
        });
    }
    if (items.empty()) {
        return;
    }

    open_layout_context_menu(
        anchor, std::move(items),
        [this, from_leaf, targets](int index) {
            if (index < 0 || index >= static_cast<int>(targets.size())) {
                return;
            }
            set_layout_menu_pane_preview(from_leaf, targets[static_cast<std::size_t>(index)]);
        });
}

void DebugApp::collapse_empty_leaf_if_needed(LayoutNodeId leaf_id) {
    if (leaf_slots(leaf_id).empty() && layout_tree_.leaf_count() > 1) {
        layout_tree_.delete_leaf(leaf_id);
    }
}

void DebugApp::move_active_panel_to_leaf(LayoutNodeId from_leaf, LayoutNodeId to_leaf) {
    if (from_leaf == to_leaf) {
        return;
    }
    if (context_menu_ != nullptr) {
        context_menu_->close();
    }

    std::vector<SidebarSlot>& from_slots = leaf_slots(from_leaf);
    if (from_slots.empty()) {
        return;
    }

    int index = leaf_stack_index(from_leaf);
    if (StackedPane* from_stack = layout_stack(from_leaf); from_stack != nullptr) {
        index = from_stack->active_index();
    }
    index = std::clamp(index, 0, static_cast<int>(from_slots.size()) - 1);

    if (from_slots[static_cast<std::size_t>(index)].config.type == SidebarPanelType::Source) {
        move_source_panel_to_leaf(to_leaf);
        return;
    }

    SidebarSlot slot = std::move(from_slots[static_cast<std::size_t>(index)]);
    from_slots.erase(from_slots.begin() + static_cast<std::ptrdiff_t>(index));

    int& from_active = leaf_stack_index(from_leaf);
    if (from_slots.empty()) {
        from_active = 0;
    } else {
        if (from_active > index) {
            --from_active;
        } else if (from_active == index) {
            from_active = std::min(index, static_cast<int>(from_slots.size()) - 1);
        }
        from_active = std::clamp(from_active, 0, static_cast<int>(from_slots.size()) - 1);
    }

    if (slot.config.type == SidebarPanelType::Repl || slot.config.type == SidebarPanelType::Console) {
        slot.shared_host.reset();
    }

    std::vector<SidebarSlot>& to_slots = leaf_slots(to_leaf);
    to_slots.push_back(std::move(slot));
    const std::string moved_label = to_slots.back().config.tab_label;
    leaf_stack_index(to_leaf) = static_cast<int>(to_slots.size()) - 1;

    model_.focus = focus_for_panel_type(to_slots.back().config.type);
    if (model_.focus == Focus::Repl) {
        repl_input_focused_ = false;
    }

    collapse_empty_leaf_if_needed(from_leaf);
    rebuild_layout_ui();
    model_.status_message = "Moved " + moved_label + " to " + layout_leaf_label(to_leaf);
    sync_status_bar();
}

void DebugApp::move_tab_to_leaf(LayoutNodeId from_leaf, int tab_index, LayoutNodeId to_leaf) {
    if (from_leaf == to_leaf) {
        return;
    }
    if (context_menu_ != nullptr) {
        context_menu_->close();
    }

    std::vector<SidebarSlot>& from_slots = leaf_slots(from_leaf);
    if (from_slots.empty()) {
        return;
    }
    tab_index = std::clamp(tab_index, 0, static_cast<int>(from_slots.size()) - 1);

    if (from_slots[static_cast<std::size_t>(tab_index)].config.type == SidebarPanelType::Source) {
        move_source_panel_to_leaf(to_leaf);
        return;
    }

    SidebarSlot slot = std::move(from_slots[static_cast<std::size_t>(tab_index)]);
    from_slots.erase(from_slots.begin() + static_cast<std::ptrdiff_t>(tab_index));

    int& from_active = leaf_stack_index(from_leaf);
    if (from_slots.empty()) {
        from_active = 0;
    } else {
        if (from_active > tab_index) {
            --from_active;
        } else if (from_active == tab_index) {
            from_active = std::min(tab_index, static_cast<int>(from_slots.size()) - 1);
        }
        from_active = std::clamp(from_active, 0, static_cast<int>(from_slots.size()) - 1);
    }

    if (slot.config.type == SidebarPanelType::Repl || slot.config.type == SidebarPanelType::Console) {
        slot.shared_host.reset();
    }

    std::vector<SidebarSlot>& to_slots = leaf_slots(to_leaf);
    to_slots.push_back(std::move(slot));
    const std::string moved_label = to_slots.back().config.tab_label;
    leaf_stack_index(to_leaf) = static_cast<int>(to_slots.size()) - 1;

    model_.focus = focus_for_panel_type(to_slots.back().config.type);
    if (model_.focus == Focus::Repl) {
        repl_input_focused_ = false;
    }

    collapse_empty_leaf_if_needed(from_leaf);
    rebuild_layout_ui();
    model_.status_message = "Added " + moved_label + " to " + layout_leaf_label(to_leaf);
    sync_status_bar();
}

void DebugApp::normalize_split_after_swap(LayoutNodeId a, LayoutNodeId b) {
    const std::optional<LayoutNodeId> parent = layout_tree_.parent_of(a);
    if (!parent.has_value() || layout_tree_.parent_of(b) != parent) {
        return;
    }

    LayoutNode& parent_node = layout_tree_.node(*parent);
    if (parent_node.kind != LayoutNode::Kind::Container || is_left_center_split(layout_tree_, *parent)) {
        return;
    }

    if (parent_node.container.orientation == tuinator::SplitOrientation::Horizontal) {
        const LayoutNodeId first = parent_node.container.first;
        const LayoutNodeId second = parent_node.container.second;
        if (layout_tree_.subtree_contains_dock(first, PanelDock::Center) &&
            layout_tree_.subtree_contains_dock(second, PanelDock::Left)) {
            parent_node.container.first_pct = static_cast<std::uint16_t>(
                std::clamp(100 - static_cast<int>(model_.layout.sidebar_pct), 1, 99));
        } else {
            parent_node.container.first_pct = 50;
        }
        return;
    }

    parent_node.container.first_pct = 50;
}

void DebugApp::swap_layout_panes(LayoutNodeId a, LayoutNodeId b) {
    if (a == b) {
        return;
    }
    if (context_menu_ != nullptr) {
        context_menu_->close();
    }

    const std::string a_label = layout_leaf_label(a);
    const std::string b_label = layout_leaf_label(b);
    layout_tree_.swap_leaf_contents(a, b);
    normalize_split_after_swap(a, b);
    rebuild_layout_ui();
    model_.status_message = "Swapped " + a_label + " with " + b_label;
    sync_status_bar();
}

LayoutNodeId DebugApp::compute_insert_anchor(LayoutNodeId from_leaf, LayoutNodeId anchor, LayoutDropZone zone) const {
    if (!layout_tree_.has_node(anchor)) {
        return 0;
    }

    if (!layout_tree_.subtree_contains(anchor, from_leaf)) {
        return anchor;
    }

    const bool vertical_insert = zone == LayoutDropZone::Above || zone == LayoutDropZone::Below;
    const bool horizontal_insert = zone == LayoutDropZone::Left || zone == LayoutDropZone::Right;

    if (const std::optional<LayoutNodeId> from_parent = layout_tree_.parent_of(from_leaf);
        from_parent.has_value() && *from_parent == anchor) {
        const LayoutNode& anchor_node = layout_tree_.node(anchor);
        const LayoutNodeId sibling =
            anchor_node.container.first == from_leaf ? anchor_node.container.second : anchor_node.container.first;
        if (vertical_insert) {
            if (const std::optional<LayoutNodeId> grandparent = layout_tree_.parent_of(anchor);
                grandparent.has_value()) {
                return *grandparent;
            }
            return sibling;
        }
        if (horizontal_insert) {
            return anchor;
        }
    }

    return anchor;
}

void DebugApp::move_layout_pane_adjacent(LayoutNodeId from_leaf, LayoutNodeId anchor, LayoutDropZone zone) {
    if (from_leaf == anchor || zone == LayoutDropZone::Center) {
        return;
    }
    if (context_menu_ != nullptr) {
        context_menu_->close();
    }

    const LayoutNodeId insert_anchor = compute_insert_anchor(from_leaf, anchor, zone);
    if (insert_anchor == 0) {
        model_.status_message = "Could not move pane";
        sync_status_bar();
        return;
    }

    std::optional<LayoutLeafData> extracted = layout_tree_.extract_leaf(from_leaf);
    if (!extracted.has_value()) {
        model_.status_message = "Cannot move the last pane";
        sync_status_bar();
        return;
    }

    LayoutLeafData leaf_data = std::move(*extracted);
    LayoutNodeId resolved_anchor = insert_anchor;
    if (!layout_tree_.has_node(resolved_anchor)) {
        resolved_anchor = layout_tree_.root();
    }

    const PaneSplitDirection direction = layout_drop_zone_to_split(zone);
    bool inserted = layout_tree_.has_node(resolved_anchor) &&
                    layout_tree_.insert_adjacent(resolved_anchor, direction, leaf_data);
    if (!inserted && layout_tree_.has_node(layout_tree_.root())) {
        inserted = layout_tree_.insert_adjacent(layout_tree_.root(), PaneSplitDirection::Right, leaf_data);
    }

    if (!inserted && !leaf_data.slots.empty() && layout_tree_.has_node(layout_tree_.root())) {
        const LayoutNodeId restored = layout_tree_.split_leaf(layout_tree_.root(), PaneSplitDirection::Right);
        layout_tree_.node(restored).leaf = std::move(leaf_data);
        inserted = true;
    }

    rebuild_layout_ui();

    if (!inserted) {
        model_.status_message = "Could not move pane";
    } else {
        model_.status_message = "Moved pane";
    }
    sync_status_bar();
}

void DebugApp::move_tab_to_new_pane_adjacent(LayoutNodeId from_leaf, int tab_index, LayoutNodeId to_leaf,
                                             LayoutDropZone zone) {
    if (zone == LayoutDropZone::Center) {
        move_tab_to_leaf(from_leaf, tab_index, to_leaf);
        return;
    }
    if (context_menu_ != nullptr) {
        context_menu_->close();
    }

    std::vector<SidebarSlot>& from_slots = leaf_slots(from_leaf);
    if (from_slots.empty()) {
        return;
    }
    tab_index = std::clamp(tab_index, 0, static_cast<int>(from_slots.size()) - 1);

    if (from_slots[static_cast<std::size_t>(tab_index)].config.type == SidebarPanelType::Source) {
        move_layout_pane_adjacent(from_leaf, to_leaf, zone);
        return;
    }

    SidebarSlot slot = std::move(from_slots[static_cast<std::size_t>(tab_index)]);
    from_slots.erase(from_slots.begin() + static_cast<std::ptrdiff_t>(tab_index));

    int& from_active = leaf_stack_index(from_leaf);
    if (from_slots.empty()) {
        from_active = 0;
    } else {
        if (from_active > tab_index) {
            --from_active;
        } else if (from_active == tab_index) {
            from_active = std::min(tab_index, static_cast<int>(from_slots.size()) - 1);
        }
        from_active = std::clamp(from_active, 0, static_cast<int>(from_slots.size()) - 1);
    }

    if (slot.config.type == SidebarPanelType::Repl || slot.config.type == SidebarPanelType::Console) {
        slot.shared_host.reset();
    }

    LayoutLeafData new_leaf;
    new_leaf.slots.push_back(std::move(slot));
    new_leaf.active_index = 0;

    LayoutNodeId insert_anchor = compute_insert_anchor(from_leaf, to_leaf, zone);
    if (!layout_tree_.has_node(insert_anchor)) {
        insert_anchor = layout_tree_.root();
    }

    bool inserted = layout_tree_.has_node(insert_anchor) &&
                    layout_tree_.insert_adjacent(insert_anchor, layout_drop_zone_to_split(zone), new_leaf);
    if (!inserted && layout_tree_.has_node(layout_tree_.root())) {
        inserted = layout_tree_.insert_adjacent(layout_tree_.root(), PaneSplitDirection::Right, new_leaf);
    }

    collapse_empty_leaf_if_needed(from_leaf);
    rebuild_layout_ui();

    if (!inserted) {
        model_.status_message = "Could not move tab";
        sync_status_bar();
        return;
    }
    model_.status_message = "Moved tab next to " + layout_leaf_label(to_leaf);
    sync_status_bar();
}

namespace {

int layout_shell_thickness_y(const tuinator::Rect& content) {
    return std::max(4, std::min(10, content.height / 10));
}

int layout_shell_thickness_x(const tuinator::Rect& content) {
    return std::max(4, std::min(10, content.width / 10));
}

bool leaf_touches_content_bottom(const tuinator::Rect& leaf, const tuinator::Rect& content) {
    return leaf.y + leaf.height >= content.y + content.height - 1;
}

bool leaf_touches_content_right(const tuinator::Rect& leaf, const tuinator::Rect& content) {
    return leaf.x + leaf.width >= content.x + content.width - 1;
}

bool leaf_touches_content_top(const tuinator::Rect& leaf, const tuinator::Rect& content) {
    return leaf.y <= content.y;
}

bool leaf_touches_content_left(const tuinator::Rect& leaf, const tuinator::Rect& content) {
    return leaf.x <= content.x;
}

LayoutDropTarget make_layout_shell_target(LayoutNodeId root, LayoutNodeId hover_leaf, LayoutDropZone zone,
                                          const tuinator::Rect& content, int shell_x, int shell_y) {
    return LayoutDropTarget{
        .anchor = root,
        .hover_leaf = hover_leaf,
        .zone = zone,
        .highlight = layout_drop_outer_shell_rect(content, zone, shell_x, shell_y),
        .spans_siblings = true,
    };
}

std::optional<LayoutNodeId> horizontal_parent_of(const LayoutTree& tree, LayoutNodeId leaf) {
    const std::optional<LayoutNodeId> parent = tree.parent_of(leaf);
    if (!parent.has_value()) {
        return std::nullopt;
    }
    const LayoutNode& parent_node = tree.node(*parent);
    if (parent_node.kind != LayoutNode::Kind::Container ||
        parent_node.container.orientation != tuinator::SplitOrientation::Horizontal) {
        return std::nullopt;
    }
    return *parent;
}

bool near_horizontal_seam(LayoutNodeId leaf, tuinator::Point position, const tuinator::Rect& leaf_bounds,
                          const LayoutTree& tree) {
    const std::optional<LayoutNodeId> parent = horizontal_parent_of(tree, leaf);
    if (!parent.has_value()) {
        return false;
    }
    const LayoutNode& parent_node = tree.node(*parent);
    const bool is_first = parent_node.container.first == leaf;
    const int seam_x = is_first ? leaf_bounds.x + leaf_bounds.width : leaf_bounds.x;
    const int seam_band = std::max(3, leaf_bounds.width / 3);
    return std::abs(position.x - seam_x) <= seam_band;
}

std::optional<LayoutNodeId> vertical_parent_of(const LayoutTree& tree, LayoutNodeId leaf) {
    const std::optional<LayoutNodeId> parent = tree.parent_of(leaf);
    if (!parent.has_value()) {
        return std::nullopt;
    }
    const LayoutNode& parent_node = tree.node(*parent);
    if (parent_node.kind != LayoutNode::Kind::Container ||
        parent_node.container.orientation != tuinator::SplitOrientation::Vertical) {
        return std::nullopt;
    }
    return *parent;
}

bool near_vertical_seam(LayoutNodeId leaf, tuinator::Point position, const tuinator::Rect& leaf_bounds,
                        const LayoutTree& tree) {
    const std::optional<LayoutNodeId> parent = vertical_parent_of(tree, leaf);
    if (!parent.has_value()) {
        return false;
    }
    const LayoutNode& parent_node = tree.node(*parent);
    const bool is_first = parent_node.container.first == leaf;
    const int seam_y = is_first ? leaf_bounds.y + leaf_bounds.height : leaf_bounds.y;
    const int seam_band = std::max(2, leaf_bounds.height / 3);
    return std::abs(position.y - seam_y) <= seam_band;
}

}  // namespace

tuinator::Rect DebugApp::layout_node_bounds(LayoutNodeId node_id) const {
    if (const StackedPane* stack = layout_stack(node_id); stack != nullptr) {
        return stack->bounds();
    }
    if (const auto it = layout_splits_.find(node_id); it != layout_splits_.end() && it->second != nullptr) {
        return it->second->bounds();
    }
    return {};
}

tuinator::Rect DebugApp::layout_content_bounds() const {
    const tuinator::Rect root_bounds = layout_node_bounds(layout_tree_.root());
    if (root_bounds.width > 0 && root_bounds.height > 0) {
        return root_bounds;
    }

    tuinator::Rect merged;
    for (const auto& [leaf_id, stack] : layout_stacks_) {
        if (stack == nullptr) {
            continue;
        }
        const tuinator::Rect bounds = stack->bounds();
        if (merged.width <= 0) {
            merged = bounds;
            continue;
        }
        const int x2 = std::max(merged.x + merged.width, bounds.x + bounds.width);
        const int y2 = std::max(merged.y + merged.height, bounds.y + bounds.height);
        merged.x = std::min(merged.x, bounds.x);
        merged.y = std::min(merged.y, bounds.y);
        merged.width = x2 - merged.x;
        merged.height = y2 - merged.y;
    }
    return merged;
}

std::optional<LayoutDropTarget> DebugApp::resolve_layout_drop_target(tuinator::Point position,
                                                                     LayoutDragSourceKind kind,
                                                                     LayoutNodeId source_leaf) const {
    const std::optional<LayoutNodeId> hover_leaf = layout_leaf_at_point(position);
    if (!hover_leaf.has_value()) {
        return std::nullopt;
    }

    const StackedPane* stack = layout_stack(*hover_leaf);
    if (stack == nullptr) {
        return std::nullopt;
    }

    const tuinator::Rect leaf_bounds = stack->bounds();
    const tuinator::Rect content = layout_content_bounds();
    const int shell_y = layout_shell_thickness_y(content);
    const int shell_x = layout_shell_thickness_x(content);
    const LayoutNodeId root = layout_tree_.root();
    const LayoutDropZone zone = layout_drop_zone_at(leaf_bounds, position);

    if (position.y >= content.y + content.height - shell_y) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Below, content, shell_x, shell_y);
    }
    if (position.y < content.y + shell_y) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Above, content, shell_x, shell_y);
    }
    if (position.x >= content.x + content.width - shell_x) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Right, content, shell_x, shell_y);
    }
    if (position.x < content.x + shell_x) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Left, content, shell_x, shell_y);
    }

    if (zone == LayoutDropZone::Below && leaf_touches_content_bottom(leaf_bounds, content)) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Below, content, shell_x, shell_y);
    }
    if (zone == LayoutDropZone::Right && leaf_touches_content_right(leaf_bounds, content)) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Right, content, shell_x, shell_y);
    }
    if (zone == LayoutDropZone::Above && leaf_touches_content_top(leaf_bounds, content)) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Above, content, shell_x, shell_y);
    }
    if (zone == LayoutDropZone::Left && leaf_touches_content_left(leaf_bounds, content)) {
        return make_layout_shell_target(root, *hover_leaf, LayoutDropZone::Left, content, shell_x, shell_y);
    }

    LayoutDropTarget target{
        .anchor = *hover_leaf,
        .hover_leaf = *hover_leaf,
        .zone = zone,
        .highlight = layout_drop_zone_rect(leaf_bounds, zone),
        .spans_siblings = false,
    };

    if (kind == LayoutDragSourceKind::Pane && *hover_leaf == source_leaf && zone == LayoutDropZone::Center) {
        return std::nullopt;
    }
    if (kind == LayoutDragSourceKind::Tab && *hover_leaf == source_leaf && zone == LayoutDropZone::Center) {
        return std::nullopt;
    }

    if (zone == LayoutDropZone::Center) {
        return target;
    }

    const int edge_x = std::max(2, leaf_bounds.width / 4);
    const int edge_y = std::max(1, leaf_bounds.height / 4);
    const bool in_leaf_edge =
        (zone == LayoutDropZone::Below && position.y >= leaf_bounds.y + leaf_bounds.height - edge_y) ||
        (zone == LayoutDropZone::Above && position.y < leaf_bounds.y + edge_y) ||
        (zone == LayoutDropZone::Left && position.x < leaf_bounds.x + edge_x) ||
        (zone == LayoutDropZone::Right && position.x >= leaf_bounds.x + leaf_bounds.width - edge_x);
    if (!in_leaf_edge) {
        return target;
    }

    for (const auto& [split_id, split] : layout_splits_) {
        if (split == nullptr || !layout_tree_.has_node(split_id)) {
            continue;
        }
        const LayoutNode& split_node = layout_tree_.node(split_id);
        if (split_node.kind != LayoutNode::Kind::Container ||
            split_node.container.orientation != tuinator::SplitOrientation::Vertical) {
            continue;
        }

        const tuinator::Rect split_bounds = split->bounds();
        const int divider_y = split_bounds.y + split->first_size();
        if (std::abs(position.y - divider_y) > 2) {
            continue;
        }
        if (position.x < split_bounds.x || position.x >= split_bounds.x + split_bounds.width) {
            continue;
        }

        if (zone == LayoutDropZone::Below && position.y >= divider_y) {
            target.anchor = split_node.container.first;
            target.spans_siblings = true;
            target.highlight = layout_drop_span_rect(split_bounds, zone);
            return target;
        }
        if (zone == LayoutDropZone::Above && position.y < divider_y) {
            target.anchor = split_node.container.second;
            target.spans_siblings = true;
            target.highlight = layout_drop_span_rect(split_bounds, LayoutDropZone::Above);
            return target;
        }
    }

    if (zone == LayoutDropZone::Below) {
        const std::optional<LayoutNodeId> row = horizontal_parent_of(layout_tree_, *hover_leaf);
        if (row.has_value()) {
            const LayoutNode& row_node = layout_tree_.node(*row);
            const bool is_first = row_node.container.first == *hover_leaf;
            const int local_x = position.x - leaf_bounds.x;
            const int inner_band = std::max(2, leaf_bounds.width / 3);
            const bool toward_seam =
                near_horizontal_seam(*hover_leaf, position, leaf_bounds, layout_tree_) ||
                (is_first && local_x >= leaf_bounds.width - inner_band) ||
                (!is_first && local_x < inner_band);
            if (toward_seam) {
                target.anchor = *row;
                target.spans_siblings = true;
                target.highlight = layout_drop_span_rect(layout_node_bounds(*row), zone);
                return target;
            }
        }
    }

    if (zone == LayoutDropZone::Right || zone == LayoutDropZone::Left) {
        for (const auto& [split_id, split] : layout_splits_) {
            if (split == nullptr || !layout_tree_.has_node(split_id)) {
                continue;
            }
            const LayoutNode& split_node = layout_tree_.node(split_id);
            if (split_node.kind != LayoutNode::Kind::Container ||
                split_node.container.orientation != tuinator::SplitOrientation::Horizontal) {
                continue;
            }

            const tuinator::Rect split_bounds = split->bounds();
            const int divider_x = split_bounds.x + split->first_size();
            if (std::abs(position.x - divider_x) > 2) {
                continue;
            }
            if (position.y < split_bounds.y || position.y >= split_bounds.y + split_bounds.height) {
                continue;
            }

            if (zone == LayoutDropZone::Right && position.x >= divider_x) {
                target.anchor = split_node.container.first;
                target.spans_siblings = true;
                target.highlight = layout_drop_span_rect(split_bounds, zone);
                return target;
            }
            if (zone == LayoutDropZone::Left && position.x < divider_x) {
                target.anchor = split_node.container.second;
                target.spans_siblings = true;
                target.highlight = layout_drop_span_rect(split_bounds, LayoutDropZone::Left);
                return target;
            }
        }

        const std::optional<LayoutNodeId> column = vertical_parent_of(layout_tree_, *hover_leaf);
        if (column.has_value()) {
            const LayoutNode& column_node = layout_tree_.node(*column);
            const bool is_first = column_node.container.first == *hover_leaf;
            const int local_y = position.y - leaf_bounds.y;
            const int inner_band = std::max(2, leaf_bounds.height / 3);
            const bool toward_seam =
                near_vertical_seam(*hover_leaf, position, leaf_bounds, layout_tree_) ||
                (is_first && local_y >= leaf_bounds.height - inner_band) ||
                (!is_first && local_y < inner_band);
            if (toward_seam) {
                target.anchor = *column;
                target.spans_siblings = true;
                target.highlight = layout_drop_span_rect(layout_node_bounds(*column), zone);
                return target;
            }
        }
    }

    return target;
}

bool DebugApp::layout_drag_active() const { return layout_drag_.active; }

void DebugApp::on_layout_drag_press(LayoutNodeId leaf_id, LayoutDragSourceKind kind, int tab_index,
                                    tuinator::Point position) {
    if (context_menu_ != nullptr && context_menu_->is_open()) {
        context_menu_->close();
    }

    layout_drag_.pending = LayoutDragState::Pending{leaf_id, kind, tab_index, position};
}

void DebugApp::begin_layout_drag() {
    if (!layout_drag_.pending.has_value()) {
        return;
    }

    layout_drag_.active = true;
    layout_drag_.source_leaf = layout_drag_.pending->source_leaf;
    layout_drag_.kind = layout_drag_.pending->kind;
    layout_drag_.tab_index = layout_drag_.pending->tab_index;
    layout_drag_.position = layout_drag_.pending->start;
    layout_drag_.pending.reset();
    layout_drag_.hover.reset();
    request_repaint();
}

void DebugApp::update_layout_drag_hover(tuinator::Point position) {
    layout_drag_.position = position;
    layout_drag_.hover =
        resolve_layout_drop_target(position, layout_drag_.kind, layout_drag_.source_leaf);
}

void DebugApp::cancel_layout_drag() {
    layout_drag_ = {};
    request_repaint();
}

void DebugApp::commit_layout_drag(tuinator::Point position) {
    if (!layout_drag_.active) {
        return;
    }

    update_layout_drag_hover(position);
    const LayoutNodeId source_leaf = layout_drag_.source_leaf;
    const LayoutDragSourceKind kind = layout_drag_.kind;
    const int tab_index = layout_drag_.tab_index;

    if (!layout_drag_.hover.has_value()) {
        cancel_layout_drag();
        return;
    }

    const LayoutDropTarget target = *layout_drag_.hover;
    const LayoutDropZone zone = target.zone;
    cancel_layout_drag();

    if (kind == LayoutDragSourceKind::Pane) {
        if (source_leaf == target.hover_leaf && zone == LayoutDropZone::Center) {
            return;
        }
        if (zone == LayoutDropZone::Center) {
            swap_layout_panes(source_leaf, target.hover_leaf);
        } else {
            move_layout_pane_adjacent(source_leaf, target.anchor, zone);
        }
        return;
    }

    if (zone == LayoutDropZone::Center) {
        move_tab_to_leaf(source_leaf, tab_index, target.hover_leaf);
    } else {
        move_tab_to_new_pane_adjacent(source_leaf, tab_index, target.anchor, zone);
    }
}

std::optional<LayoutNodeId> DebugApp::layout_leaf_at_point(tuinator::Point position) const {
    for (const auto& [leaf_id, stack] : layout_stacks_) {
        if (stack != nullptr && stack->bounds().contains(position)) {
            return leaf_id;
        }
    }
    return std::nullopt;
}

std::optional<LayoutDropTarget> DebugApp::layout_menu_preview_target() const {
    if (!layout_menu_preview_.has_value()) {
        return std::nullopt;
    }

    const LayoutDropTarget& target = layout_menu_preview_->target;
    if (target.highlight.width <= 0 || target.highlight.height <= 0) {
        return std::nullopt;
    }
    return target;
}

void DebugApp::paint_layout_drop_highlight(tuinator::PaintContext& ctx, const LayoutDropTarget& target,
                                           tuinator::Style style) const {
    const tuinator::Rect local{target.highlight.x, target.highlight.y, target.highlight.width,
                               target.highlight.height};
    if (local.width <= 0 || local.height <= 0) {
        return;
    }
    ctx.canvas.fill_rect(local, ' ', style);
}

void DebugApp::paint_layout_menu_preview(tuinator::PaintContext& ctx) const {
    if (!layout_menu_preview_.has_value()) {
        return;
    }

    const std::optional<LayoutDropTarget> target = layout_menu_preview_target();
    if (!target.has_value()) {
        return;
    }

    tuinator::Style style = dap_theme_.layout_menu_pane;
    if (layout_menu_preview_->mode == LayoutMenuPreview::Mode::Placement) {
        if (layout_menu_preview_->swap) {
            style = dap_theme_.layout_drop_swap;
        } else if (target->spans_siblings) {
            style = dap_theme_.layout_drop_span;
        } else {
            style = dap_theme_.layout_drop_insert;
        }
    }
    paint_layout_drop_highlight(ctx, *target, style);
}

void DebugApp::paint_layout_drag_overlay(tuinator::PaintContext& ctx) const {
    if (!layout_drag_.active || !layout_drag_.hover.has_value()) {
        return;
    }

    const LayoutDropTarget& target = *layout_drag_.hover;
    tuinator::Style style = target.spans_siblings ? dap_theme_.layout_drop_span : dap_theme_.layout_drop_insert;
    if (target.zone == LayoutDropZone::Center) {
        style = layout_drag_.kind == LayoutDragSourceKind::Pane ? dap_theme_.layout_drop_swap
                                                                 : dap_theme_.layout_drop_merge;
    }
    paint_layout_drop_highlight(ctx, target, style);
}

bool DebugApp::handle_layout_drag_mouse(const tuinator::MouseEvent& mouse) {
    if (layout_drag_.active) {
        if (mouse.action == tuinator::MouseAction::Move) {
            update_layout_drag_hover(mouse.position);
            request_repaint();
            return true;
        }
        if (mouse.action == tuinator::MouseAction::Release || mouse.action == tuinator::MouseAction::Click) {
            commit_layout_drag(mouse.position);
            return true;
        }
        return true;
    }

    if (!layout_drag_.pending.has_value()) {
        return false;
    }

    if (mouse.action == tuinator::MouseAction::Move && mouse.left_pressed) {
        const int dx = mouse.position.x - layout_drag_.pending->start.x;
        const int dy = mouse.position.y - layout_drag_.pending->start.y;
        if ((dx * dx + dy * dy) >= 4) {
            begin_layout_drag();
            update_layout_drag_hover(mouse.position);
        }
        return true;
    }

    if (mouse.action == tuinator::MouseAction::Release || mouse.action == tuinator::MouseAction::Click) {
        if (StackedPane* stack = layout_stack(layout_drag_.pending->source_leaf); stack != nullptr) {
            stack->handle_chrome_click(mouse.position);
        }
        layout_drag_.pending.reset();
        request_repaint();
        return true;
    }

    return mouse.left_pressed;
}

void DebugApp::show_pane_add_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "Left",
        [this, leaf_id]() { split_layout_pane(leaf_id, PaneSplitDirection::Left); },
    });
    items.push_back(ContextMenu::Item{
        "Right",
        [this, leaf_id]() { split_layout_pane(leaf_id, PaneSplitDirection::Right); },
    });
    items.push_back(ContextMenu::Item{
        "Up",
        [this, leaf_id]() { split_layout_pane(leaf_id, PaneSplitDirection::Up); },
    });
    items.push_back(ContextMenu::Item{
        "Down",
        [this, leaf_id]() { split_layout_pane(leaf_id, PaneSplitDirection::Down); },
    });

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

namespace {

const char* pane_split_direction_label(PaneSplitDirection direction) {
    switch (direction) {
    case PaneSplitDirection::Left:
        return "left";
    case PaneSplitDirection::Right:
        return "right";
    case PaneSplitDirection::Up:
        return "up";
    case PaneSplitDirection::Down:
        return "down";
    }
    return "pane";
}

}  // namespace

void DebugApp::split_layout_pane(LayoutNodeId leaf_id, PaneSplitDirection direction) {
    if (context_menu_ != nullptr) {
        context_menu_->close();
    }
    (void)layout_tree_.split_leaf(leaf_id, direction);
    rebuild_layout_ui();
    model_.status_message = std::string("Added pane to the ") + pane_split_direction_label(direction) + " (" +
                            std::to_string(layout_tree_.leaf_count()) + " panes)";
    sync_status_bar();
}

void DebugApp::delete_layout_pane(LayoutNodeId leaf_id) {
    if (context_menu_ != nullptr) {
        context_menu_->close();
    }
    if (!layout_tree_.delete_leaf(leaf_id)) {
        model_.status_message = "Cannot delete the last pane";
        sync_status_bar();
        return;
    }
    rebuild_layout_ui();
    model_.status_message = "Pane removed";
    sync_status_bar();
}

void DebugApp::persist_split_size_as_pct(ResizableSplitPane* split, std::uint16_t& pct_out, bool horizontal,
                                         bool invert) {
    if (split == nullptr) {
        return;
    }

    const tuinator::Rect bounds = split->bounds();
    const int total = horizontal ? bounds.width : bounds.height;
    if (total <= 0) {
        return;
    }

    const int first = split->first_size();
    const int value = invert ? (total - first) : first;
    pct_out = static_cast<std::uint16_t>(std::clamp(value * 100 / total, 1, 99));
}

std::vector<SidebarSlot>& DebugApp::dock_slots(PanelDock dock) {
    return leaf_slots(dock_leaf_id(dock));
}

const std::vector<SidebarSlot>& DebugApp::dock_slots(PanelDock dock) const {
    return const_cast<DebugApp*>(this)->dock_slots(dock);
}

StackedPane* DebugApp::dock_stack(PanelDock dock) { return layout_stack(dock_leaf_id(dock)); }

int& DebugApp::dock_stack_index(PanelDock dock) { return leaf_stack_index(dock_leaf_id(dock)); }

std::vector<PanelSlotConfig> DebugApp::leaf_slot_configs(LayoutNodeId leaf_id) const {
    std::vector<PanelSlotConfig> configs;
    const std::vector<SidebarSlot>& slots = layout_tree_.node(leaf_id).leaf.slots;
    configs.reserve(slots.size());
    for (const SidebarSlot& slot : slots) {
        configs.push_back(slot.config);
    }
    return configs;
}

std::vector<PanelSlotConfig> DebugApp::dock_slot_configs(PanelDock dock) const {
    return leaf_slot_configs(dock_leaf_id(dock));
}

SidebarSlot* DebugApp::leaf_slot_at(LayoutNodeId leaf_id, int index) {
    std::vector<SidebarSlot>& slots = leaf_slots(leaf_id);
    if (index < 0 || index >= static_cast<int>(slots.size())) {
        return nullptr;
    }
    return &slots[static_cast<std::size_t>(index)];
}

SidebarSlot* DebugApp::dock_slot_at(PanelDock dock, int index) {
    return leaf_slot_at(dock_leaf_id(dock), index);
}

SidebarSlot* DebugApp::active_dock_slot(PanelDock dock) {
    return dock_slot_at(dock, dock_stack_index(dock));
}

void DebugApp::for_each_sidebar_slot(const std::function<void(SidebarSlot&)>& visitor) {
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        for (SidebarSlot& slot : leaf_slots(leaf_id)) {
            visitor(slot);
        }
    }
}

SidebarSlot* DebugApp::slot_by_id(std::uint64_t slot_id) {
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        for (SidebarSlot& slot : leaf_slots(leaf_id)) {
            if (slot.config.id == slot_id) {
                return &slot;
            }
        }
    }
    return nullptr;
}

void DebugApp::init_default_sidebar_slots(std::vector<SidebarSlot>& slots) {
    auto push_slot = [&](PanelSlotConfig config) {
        config.id = next_slot_id_++;
        std::vector<PanelSlotConfig> existing;
        for (const SidebarSlot& slot : slots) {
            existing.push_back(slot.config);
        }
        config.tab_label = make_panel_tab_label(config, existing);
        SidebarSlot slot;
        slot.config = std::move(config);
        slots.push_back(std::move(slot));
    };

    auto make_config = [](SidebarPanelType type) {
        PanelSlotConfig config;
        config.type = type;
        return config;
    };

    push_slot(make_config(SidebarPanelType::Variables));
    push_slot(make_config(SidebarPanelType::Threads));
    push_slot(make_config(SidebarPanelType::Breakpoints));
    push_slot(make_config(SidebarPanelType::Watches));
    push_slot(make_config(SidebarPanelType::Resources));
    if (!model_.watches.empty()) {
        slots.back().watches_data = std::move(model_.watches);
        model_.watches.clear();
    }
}

void DebugApp::wire_scopes_panel(ScopesPanel& panel, SidebarSlot& slot) {
    const std::uint64_t slot_id = slot.config.id;
    panel.set_on_watch([this](const std::string& variable_name) { add_watch(variable_name); });
    panel.set_on_edit_variable([this](const std::string& variable_name) { begin_edit_variable(variable_name); });
    panel.set_on_submit([this](const std::string& value) { submit_variable_value(value); });
    panel.set_on_change([this](const std::string& value) {
        scope_input_draft_ = value;
        scope_input_focused_ = true;
        model_.focus = Focus::Scopes;
    });
    panel.set_on_inline_edit_cancel([this]() { blur_scope_input(); });
    panel.set_on_activate([this, slot_id](int index) { toggle_scope_row_expand(slot_id, index); });
    panel.set_on_context([this](int index, tuinator::Point anchor) {
        show_scope_variable_context_menu(index, anchor);
    });
}

void DebugApp::wire_watches_panel(WatchesPanel& panel, SidebarSlot& /*slot*/) {
    panel.set_on_submit([this](const std::string& expression) { submit_watch_expression(expression); });
    panel.set_on_change([this](const std::string& expression) {
        watch_input_draft_ = expression;
        watch_input_focused_ = true;
        model_.focus = Focus::Watches;
    });
    panel.set_on_remove([this](int index) { remove_watch_at(static_cast<std::size_t>(index)); });
    panel.set_on_edit([this](int index) { begin_edit_watch_at(index); });
    panel.set_on_add([this]() { begin_add_watch(); });
    panel.set_on_inline_edit_cancel([this]() { blur_watch_input(); });
}

void DebugApp::wire_stacks_panel(StacksPanel& panel) {
    panel.set_on_continue([this]() { send_command("continue"); });
    panel.set_on_activate([this](const StackFrameRow& frame) {
        std::string path = frame.path;
        std::int64_t source_reference = frame.source_reference;
        if (path.rfind("dap:source:", 0) == 0) {
            path.clear();
        }
        if (path.empty() && source_reference <= 0) {
            return;
        }
        open_source_file(path, std::max(1, static_cast<int>(frame.line)), true, source_reference);
        model_.status_message = "Jumped to " + frame.name;
        sync_status_bar();
    });
    panel.set_on_context([this](const StackFrameRow& frame, tuinator::Point anchor) {
        show_stack_frame_context_menu(frame, anchor);
    });
}

void DebugApp::wire_breakpoints_panel(BreakpointsPanel& panel) {
    panel.set_on_activate([this](const BreakpointRow& row) {
        if (row.kind == BreakpointRowKind::Data) {
            model_.status_message = "Data breakpoint: " + row.source_text;
            sync_status_bar();
            return;
        }
        if (row.kind == BreakpointRowKind::Function) {
            model_.status_message = "Function breakpoint: " + row.source_text;
            sync_status_bar();
            return;
        }
        if (row.kind == BreakpointRowKind::Exception) {
            toggle_exception_breakpoint(row.data_id);
            return;
        }
        open_source_file(row.path, row.line, true);
        model_.status_message =
            "Opened " + panel_title_from_path(row.path) + ":" + std::to_string(row.line);
        sync_status_bar();
    });
    panel.set_on_remove([this](const BreakpointRow& row) {
        if (row.kind == BreakpointRowKind::Data) {
            remove_data_breakpoint(row.data_id);
        } else if (row.kind == BreakpointRowKind::Function) {
            remove_function_breakpoint(row.source_text);
        } else if (row.kind == BreakpointRowKind::Exception) {
            if (exception_breakpoints_[row.data_id].enabled) {
                exception_breakpoints_[row.data_id].enabled = false;
                push_exception_breakpoints_to_session();
                sync_breakpoints_list_panel();
                model_.status_message = "Disabled exception breakpoint: " + row.source_text;
                sync_status_bar();
            }
        } else {
            remove_breakpoint_at(row.path, row.line);
        }
    });
    panel.set_on_add_condition([this](const BreakpointRow& row, int display_index, tuinator::Point action_anchor) {
        if (row.kind == BreakpointRowKind::Exception) {
            if (!row.exception_supports_condition) {
                model_.status_message = "This exception filter does not support conditions";
                sync_status_bar();
                return;
            }
            begin_edit_exception_condition(row.data_id);
            return;
        }
        open_breakpoint_condition_editor(row.path, row.line, action_anchor, display_index);
    });
    panel.set_on_edit_when_condition([this](const BreakpointRow& row, tuinator::Point /*action_anchor*/) {
        if (row.kind == BreakpointRowKind::Exception) {
            begin_edit_exception_condition(row.data_id);
            return;
        }
        begin_edit_breakpoint_condition(row.path, row.line);
    });
    panel.set_on_edit_hit_condition([this](const BreakpointRow& row, tuinator::Point /*action_anchor*/) {
        begin_edit_breakpoint_hit_condition(row.path, row.line);
    });
    panel.set_on_clear_when_condition([this](const BreakpointRow& row) {
        if (row.kind == BreakpointRowKind::Exception) {
            set_exception_breakpoint_condition(row.data_id, "");
            return;
        }
        set_breakpoint_condition(row.path, row.line, "");
    });
    panel.set_on_clear_hit_condition([this](const BreakpointRow& row) {
        set_breakpoint_hit_condition(row.path, row.line, "");
    });
    panel.set_on_submit([this](const std::string& condition) {
        if (!editing_exception_filter_.empty()) {
            set_exception_breakpoint_condition(editing_exception_filter_, condition);
            blur_breakpoint_input(false);
            return;
        }
        if (!editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0) {
            submit_breakpoint_condition(condition);
        }
    });
    panel.set_on_change([this](const std::string& condition) {
        if (!editing_exception_filter_.empty() ||
            (!editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0)) {
            breakpoint_input_draft_ = condition;
            breakpoint_input_focused_ = true;
            model_.focus = Focus::Breakpoints;
        }
    });
    panel.set_on_inline_edit_cancel([this]() { blur_breakpoint_input(true); });
}

void DebugApp::ensure_sidebar_slot_panels(SidebarSlot& slot, const tuinator::ScrollViewOptions& scroll_options) {
    switch (slot.config.type) {
    case SidebarPanelType::Variables:
        if (slot.scopes == nullptr) {
            slot.scopes = std::make_unique<ScopesPanel>(dap_theme_, scroll_options);
            wire_scopes_panel(*slot.scopes, slot);
        }
        break;
    case SidebarPanelType::Watches:
        if (slot.watches == nullptr) {
            slot.watches = std::make_unique<WatchesPanel>(dap_theme_, scroll_options);
            wire_watches_panel(*slot.watches, slot);
        }
        break;
    case SidebarPanelType::Threads:
        if (slot.stacks == nullptr) {
            slot.stacks = std::make_unique<StacksPanel>(dap_theme_, scroll_options);
            wire_stacks_panel(*slot.stacks);
        }
        break;
    case SidebarPanelType::Breakpoints:
        if (slot.breakpoints == nullptr) {
            slot.breakpoints = std::make_unique<BreakpointsPanel>(dap_theme_, scroll_options);
            wire_breakpoints_panel(*slot.breakpoints);
        }
        break;
    case SidebarPanelType::Source:
        if (source_content_shell_ != nullptr && slot.shared_host == nullptr) {
            slot.shared_host = std::make_unique<SharedWidgetHost>(source_content_shell_.get());
        }
        break;
    case SidebarPanelType::Memory:
        if (slot.memory == nullptr) {
            slot.memory = std::make_unique<MemoryPanel>(dap_theme_, scroll_options);
            wire_memory_panel(*slot.memory, slot);
        }
        break;
    case SidebarPanelType::DisassemblyAsm:
    case SidebarPanelType::DisassemblyBytes:
        if (slot.disassembly == nullptr) {
            slot.disassembly = std::make_unique<DisassemblyPanel>(dap_theme_, scroll_options,
                                                                  panel_type_label(slot.config.type));
            wire_disassembly_panel(*slot.disassembly, slot);
        }
        break;
    case SidebarPanelType::RuntimeSource:
        if (slot.runtime_source == nullptr) {
            slot.runtime_source = std::make_unique<RuntimeSourcePanel>(dap_theme_, scroll_options);
            wire_runtime_source_panel(*slot.runtime_source, slot);
        }
        break;
    case SidebarPanelType::FileTree:
        if (slot.file_tree == nullptr) {
            slot.file_tree = std::make_unique<FileTreePanel>(dap_theme_, scroll_options);
            wire_file_tree_panel(*slot.file_tree);
            sync_file_tree_slot(slot);
        }
        break;
    case SidebarPanelType::Resources:
        if (slot.resources == nullptr) {
            slot.resources = std::make_unique<ResourcesPanel>(dap_theme_, scroll_options);
            sync_resources_slot(slot);
        }
        break;
    case SidebarPanelType::Network:
        if (network_shell_ != nullptr && slot.shared_host == nullptr) {
            slot.shared_host = std::make_unique<SharedWidgetHost>(network_shell_.get());
        }
        break;
    case SidebarPanelType::Repl:
        if (repl_shell_ != nullptr && slot.shared_host == nullptr) {
            slot.shared_host = std::make_unique<SharedWidgetHost>(repl_shell_.get());
        }
        break;
    case SidebarPanelType::Console:
        if (console_shell_ != nullptr && slot.shared_host == nullptr) {
            slot.shared_host = std::make_unique<SharedWidgetHost>(console_shell_.get());
        }
        break;
    }
}

std::unique_ptr<tuinator::Widget> DebugApp::release_sidebar_slot_widget(SidebarSlot& slot) {
    switch (slot.config.type) {
    case SidebarPanelType::Variables:
        if (slot.scopes != nullptr) {
            return slot.scopes->release_widget();
        }
        break;
    case SidebarPanelType::Watches:
        if (slot.watches != nullptr) {
            return slot.watches->release_widget();
        }
        break;
    case SidebarPanelType::Threads:
        if (slot.stacks != nullptr) {
            return slot.stacks->release_widget();
        }
        break;
    case SidebarPanelType::Breakpoints:
        if (slot.breakpoints != nullptr) {
            return slot.breakpoints->release_widget();
        }
        break;
    case SidebarPanelType::Memory:
        if (slot.memory != nullptr) {
            return slot.memory->release_widget();
        }
        break;
    case SidebarPanelType::DisassemblyAsm:
    case SidebarPanelType::DisassemblyBytes:
        if (slot.disassembly != nullptr) {
            return slot.disassembly->release_widget();
        }
        break;
    case SidebarPanelType::RuntimeSource:
        if (slot.runtime_source != nullptr) {
            return slot.runtime_source->release_widget();
        }
        break;
    case SidebarPanelType::FileTree:
        if (slot.file_tree != nullptr) {
            return slot.file_tree->release_widget();
        }
        break;
    case SidebarPanelType::Resources:
        if (slot.resources != nullptr) {
            return slot.resources->release_widget();
        }
        break;
    case SidebarPanelType::Network:
    case SidebarPanelType::Source:
    case SidebarPanelType::Repl:
    case SidebarPanelType::Console:
        if (slot.shared_host != nullptr) {
            return std::move(slot.shared_host);
        }
        break;
    }
    return std::make_unique<SharedWidgetHost>(nullptr);
}

SidebarSlot* DebugApp::sidebar_slot_at(int index) { return dock_slot_at(PanelDock::Left, index); }

SidebarSlot* DebugApp::active_sidebar_slot() {
    return dock_slot_at(PanelDock::Left, dock_stack_index(PanelDock::Left));
}

SidebarSlot* DebugApp::sidebar_slot_by_id(std::uint64_t slot_id) { return slot_by_id(slot_id); }

bool DebugApp::focus_matches_panel_type(Focus focus, SidebarPanelType type) {
    switch (type) {
    case SidebarPanelType::Variables:
        return focus == Focus::Scopes;
    case SidebarPanelType::Threads:
        return focus == Focus::Stacks;
    case SidebarPanelType::Breakpoints:
        return focus == Focus::Breakpoints;
    case SidebarPanelType::Watches:
        return focus == Focus::Watches;
    case SidebarPanelType::Memory:
        return focus == Focus::Memory;
    case SidebarPanelType::DisassemblyAsm:
    case SidebarPanelType::DisassemblyBytes:
        return focus == Focus::Disassembly;
    case SidebarPanelType::RuntimeSource:
        return focus == Focus::RuntimeSource;
    case SidebarPanelType::FileTree:
        return focus == Focus::FileTree;
    case SidebarPanelType::Resources:
        return focus == Focus::Resources;
    case SidebarPanelType::Network:
        return focus == Focus::Network;
    case SidebarPanelType::Source:
        return focus == Focus::Source;
    case SidebarPanelType::Repl:
        return focus == Focus::Repl;
    case SidebarPanelType::Console:
        return focus == Focus::Console;
    }
    return false;
}

Focus DebugApp::focus_for_panel_type(SidebarPanelType type) {
    switch (type) {
    case SidebarPanelType::Variables:
        return Focus::Scopes;
    case SidebarPanelType::Threads:
        return Focus::Stacks;
    case SidebarPanelType::Breakpoints:
        return Focus::Breakpoints;
    case SidebarPanelType::Watches:
        return Focus::Watches;
    case SidebarPanelType::Memory:
        return Focus::Memory;
    case SidebarPanelType::DisassemblyAsm:
    case SidebarPanelType::DisassemblyBytes:
        return Focus::Disassembly;
    case SidebarPanelType::RuntimeSource:
        return Focus::RuntimeSource;
    case SidebarPanelType::FileTree:
        return Focus::FileTree;
    case SidebarPanelType::Resources:
        return Focus::Resources;
    case SidebarPanelType::Network:
        return Focus::Network;
    case SidebarPanelType::Source:
        return Focus::Source;
    case SidebarPanelType::Repl:
        return Focus::Repl;
    case SidebarPanelType::Console:
        return Focus::Console;
    }
    return Focus::Scopes;
}

int DebugApp::dock_index_for_focus(PanelDock dock, Focus focus) const {
    const std::vector<SidebarSlot>& slots = dock_slots(dock);
    const int current = layout_tree_.node(dock_leaf_id(dock)).leaf.active_index;
    if (current >= 0 && current < static_cast<int>(slots.size()) &&
        focus_matches_panel_type(focus, slots[static_cast<std::size_t>(current)].config.type)) {
        return current;
    }
    for (std::size_t i = 0; i < slots.size(); ++i) {
        if (focus_matches_panel_type(focus, slots[i].config.type)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool DebugApp::is_panel_type_active(SidebarPanelType type) const {
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        const StackedPane* stack = layout_stack(leaf_id);
        if (stack == nullptr) {
            continue;
        }
        const std::vector<SidebarSlot>& slots = layout_tree_.node(leaf_id).leaf.slots;
        const int index = stack->active_index();
        if (index >= 0 && index < static_cast<int>(slots.size()) &&
            slots[static_cast<std::size_t>(index)].config.type == type) {
            return true;
        }
    }
    return false;
}

void DebugApp::sync_dock_stack_to_focus(PanelDock dock) {
    StackedPane* stack = dock_stack(dock);
    if (stack == nullptr) {
        return;
    }
    const int index = dock_index_for_focus(dock, model_.focus);
    if (index >= 0 && index != stack->active_index()) {
        dock_stack_index(dock) = index;
        stack->set_active_index(index);
    }
}

SidebarSlot* DebugApp::active_slot_for_focus() {
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        if (StackedPane* stack = layout_stack(leaf_id); stack != nullptr) {
            const int active = stack->active_index();
            if (SidebarSlot* slot = leaf_slot_at(leaf_id, active); slot != nullptr &&
                                                                    focus_matches_panel_type(model_.focus,
                                                                                             slot->config.type)) {
                return slot;
            }
        }
    }
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        const std::vector<SidebarSlot>& slots = leaf_slots(leaf_id);
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (focus_matches_panel_type(model_.focus, slots[i].config.type)) {
                return leaf_slot_at(leaf_id, static_cast<int>(i));
            }
        }
    }
    return nullptr;
}

SidebarSlot* DebugApp::find_memory_slot() {
    if (memory_focus_slot_id_ != 0) {
        if (SidebarSlot* slot = slot_by_id(memory_focus_slot_id_); slot != nullptr && slot->memory != nullptr) {
            return slot;
        }
    }
    SidebarSlot* found = nullptr;
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (found == nullptr && slot.memory != nullptr) {
            found = &slot;
        }
    });
    return found;
}

SidebarSlot* DebugApp::memory_slot_for_focus(std::uint64_t preferred_slot_id) {
    if (preferred_slot_id != 0) {
        if (SidebarSlot* slot = slot_by_id(preferred_slot_id); slot != nullptr && slot->memory != nullptr) {
            return slot;
        }
    }
    if (memory_focus_slot_id_ != 0) {
        if (SidebarSlot* slot = slot_by_id(memory_focus_slot_id_); slot != nullptr && slot->memory != nullptr) {
            return slot;
        }
    }
    if (SidebarSlot* slot = active_slot_for_focus(); slot != nullptr && slot->memory != nullptr) {
        return slot;
    }
    return find_memory_slot();
}

void DebugApp::activate_memory_toolbar(std::uint64_t slot_id) {
    SidebarSlot* slot = slot_by_id(slot_id);
    if (slot == nullptr || slot->memory == nullptr) {
        return;
    }
    memory_focus_slot_id_ = slot_id;
    memory_write_active_ = false;
    memory_write_row_ = -1;
    memory_toolbar_focused_ = true;
    model_.focus = Focus::Memory;
    apply_focus();
    request_repaint();
}

std::optional<std::pair<LayoutNodeId, int>> DebugApp::find_source_panel_slot() const {
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        const std::vector<SidebarSlot>& slots = layout_tree_.node(leaf_id).leaf.slots;
        for (std::size_t i = 0; i < slots.size(); ++i) {
            if (slots[i].config.type == SidebarPanelType::Source) {
                return std::make_pair(leaf_id, static_cast<int>(i));
            }
        }
    }
    return std::nullopt;
}

bool DebugApp::has_source_panel() const { return find_source_panel_slot().has_value(); }

void DebugApp::move_source_panel_to_leaf(LayoutNodeId target_leaf) {
    const std::optional<std::pair<LayoutNodeId, int>> current = find_source_panel_slot();
    if (current.has_value() && current->first == target_leaf) {
        model_.status_message = "Source is already in this pane";
        sync_status_bar();
        return;
    }

    if (current.has_value()) {
        std::vector<SidebarSlot>& from_slots = leaf_slots(current->first);
        const int index = current->second;
        if (index < 0 || index >= static_cast<int>(from_slots.size())) {
            return;
        }

        SidebarSlot slot = std::move(from_slots[static_cast<std::size_t>(index)]);
        from_slots.erase(from_slots.begin() + static_cast<std::ptrdiff_t>(index));

        int& from_index = leaf_stack_index(current->first);
        if (from_slots.empty()) {
            from_index = 0;
        } else {
            if (from_index > index) {
                --from_index;
            } else if (from_index == index) {
                from_index = std::min(index, static_cast<int>(from_slots.size()) - 1);
            }
            from_index = std::clamp(from_index, 0, static_cast<int>(from_slots.size()) - 1);
        }

        slot.shared_host.reset();
        std::vector<SidebarSlot>& target_slots = leaf_slots(target_leaf);
        target_slots.push_back(std::move(slot));
        collapse_empty_leaf_if_needed(current->first);
    } else {
        PanelSlotConfig config;
        config.type = SidebarPanelType::Source;
        config.id = next_slot_id_++;
        config.tab_label = make_panel_tab_label(config, leaf_slot_configs(target_leaf));
        std::vector<SidebarSlot>& target_slots = leaf_slots(target_leaf);
        SidebarSlot new_slot;
        new_slot.config = std::move(config);
        target_slots.push_back(std::move(new_slot));
    }

    leaf_stack_index(target_leaf) = static_cast<int>(leaf_slots(target_leaf).size()) - 1;
    model_.focus = Focus::Source;

    rebuild_layout_ui();

    source_layout_settling_ = true;
    refresh_source_highlight_if_needed();
    source_layout_settling_ = false;
    refresh_scroll_views(true);
    mark_source_view_dirty();

    model_.status_message = "Moved Source to " + layout_leaf_label(target_leaf);
    sync_status_bar();
}

void DebugApp::move_source_panel_to_dock(PanelDock target_dock) {
    move_source_panel_to_leaf(dock_leaf_id(target_dock));
}

void DebugApp::update_active_panel_pointers() {
    scopes_panel_ = nullptr;
    watches_panel_ = nullptr;
    stacks_panel_ = nullptr;
    breakpoints_panel_ = nullptr;
    if (SidebarSlot* slot = active_slot_for_focus()) {
        if (slot->scopes != nullptr) {
            scopes_panel_ = slot->scopes.get();
        }
        if (slot->watches != nullptr) {
            watches_panel_ = slot->watches.get();
        }
        if (slot->stacks != nullptr) {
            stacks_panel_ = slot->stacks.get();
        }
        if (slot->breakpoints != nullptr) {
            breakpoints_panel_ = slot->breakpoints.get();
        }
    }
}

void DebugApp::sync_scope_slot(SidebarSlot& slot) {
    if (slot.scopes == nullptr || slot.config.type != SidebarPanelType::Variables) {
        return;
    }
    std::vector<bool> show_edit;
    show_edit.reserve(slot.cached_scope_row_meta.size());
    for (const ScopeVariableRowMeta& row : slot.cached_scope_row_meta) {
        show_edit.push_back(row.show_edit);
    }
    slot.scopes->set_scope_names(slot.cached_scope_rows, std::move(show_edit));
}

void DebugApp::refresh_all_scope_slots() {
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type != SidebarPanelType::Variables) {
            return;
        }
        std::vector<ScopeVariableRowMeta> meta;
        slot.cached_scope_rows =
            build_scope_rows(model_, expanded_scope_paths_, pending_scope_paths_, collapsed_scope_sections_, meta,
                             slot.config.scope_filter);
        slot.cached_scope_row_meta = std::move(meta);
        sync_scope_slot(slot);
    });
    apply_scope_value_overrides();
}

std::vector<WatchEntry>& DebugApp::active_watch_list() {
    if (SidebarSlot* slot = active_slot_for_focus(); slot != nullptr && slot->config.type == SidebarPanelType::Watches) {
        return slot->watches_data;
    }
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        for (SidebarSlot& slot : leaf_slots(leaf_id)) {
            if (slot.config.type == SidebarPanelType::Watches) {
                return slot.watches_data;
            }
        }
    }
    static std::vector<WatchEntry> fallback;
    return fallback;
}

void DebugApp::sync_thread_slot(SidebarSlot& slot, const std::vector<ThreadStackContent>& threads) {
    if (slot.stacks == nullptr || slot.config.type != SidebarPanelType::Threads) {
        return;
    }

    std::vector<ThreadStackContent> filtered;
    filtered.reserve(threads.size());
    for (const ThreadStackContent& thread : threads) {
        if (slot.config.thread_id_filter.has_value() && thread.id != *slot.config.thread_id_filter) {
            continue;
        }
        if (slot.config.thread_filter == ThreadPanelFilter::Stopped && !thread.stopped) {
            continue;
        }
        if (slot.config.thread_filter == ThreadPanelFilter::Running && thread.stopped) {
            continue;
        }
        filtered.push_back(thread);
    }

    ThreadStackDisplayOptions options{};
    if (slot.config.thread_id_filter.has_value()) {
        options.hide_thread_headers = true;
        options.collapsible_threads = false;
    } else {
        options.collapsible_threads = true;
    }
    slot.stacks->set_display_options(options);
    slot.stacks->set_thread_stacks(std::move(filtered));
}

void DebugApp::sync_breakpoint_slot(SidebarSlot& slot, const std::vector<BreakpointRow>& rows) {
    if (slot.breakpoints == nullptr || slot.config.type != SidebarPanelType::Breakpoints) {
        return;
    }
    slot.breakpoints->set_kind_filter(slot.config.breakpoint_filter);

    if (!slot.config.breakpoint_filter.has_value()) {
        slot.breakpoints->set_breakpoints(rows);
        return;
    }

    std::vector<BreakpointRow> filtered;
    filtered.reserve(rows.size());
    for (const BreakpointRow& row : rows) {
        if (row.kind == *slot.config.breakpoint_filter) {
            filtered.push_back(row);
        }
    }
    slot.breakpoints->set_breakpoints(std::move(filtered));
}

void DebugApp::show_add_scope_menu(LayoutNodeId leaf_id, SidebarPanelType type, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "All",
        [this, leaf_id, type]() { add_panel_to_leaf(leaf_id, type, std::nullopt); },
    });
    for (const std::string& scope_name : available_scope_names()) {
        items.push_back(ContextMenu::Item{
            scope_name,
            [this, leaf_id, type, name = scope_name]() { add_panel_to_leaf(leaf_id, type, name); },
        });
    }

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_add_panel_menu(tuinator::Point anchor, LayoutNodeId leaf_id) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "Variables ›",
        [this, leaf_id, anchor]() { show_add_scope_menu(leaf_id, SidebarPanelType::Variables, anchor); },
    });
    items.push_back(ContextMenu::Item{
        "Watches",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Watches); },
    });
    items.push_back(ContextMenu::Item{
        "Threads ›",
        [this, leaf_id, anchor]() { show_add_thread_menu(leaf_id, anchor); },
    });
    items.push_back(ContextMenu::Item{
        "Breakpoints ›",
        [this, leaf_id, anchor]() { show_add_breakpoint_menu(leaf_id, anchor); },
    });
    items.push_back(ContextMenu::Item{
        "Advanced ›",
        [this, leaf_id, anchor]() { show_add_advanced_menu(leaf_id, anchor); },
    });
    items.push_back(ContextMenu::Item{
        "Workspace ›",
        [this, leaf_id, anchor]() { show_add_workspace_menu(leaf_id, anchor); },
    });
    items.push_back(ContextMenu::Item{
        "Output ›",
        [this, leaf_id, anchor]() { show_add_output_menu(leaf_id, anchor); },
    });

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_add_advanced_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    if (model_.supports_read_memory_request) {
        items.push_back(ContextMenu::Item{
            "Memory",
            [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Memory); },
        });
    }
    items.push_back(ContextMenu::Item{
        "Runtime Source",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::RuntimeSource); },
    });
    if (model_.supports_disassemble_request) {
        items.push_back(ContextMenu::Item{
            "Disassembly ›",
            [this, leaf_id, anchor]() { show_add_disassembly_menu(leaf_id, anchor); },
        });
    }

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_add_disassembly_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "ASM",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::DisassemblyAsm); },
    });
    items.push_back(ContextMenu::Item{
        "Hex",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::DisassemblyBytes); },
    });

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_add_workspace_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "File Tree",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::FileTree); },
    });
    items.push_back(ContextMenu::Item{
        "Resources",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Resources); },
    });
    if (const std::optional<std::pair<LayoutNodeId, int>> source_loc = find_source_panel_slot();
        !source_loc.has_value() || source_loc->first != leaf_id) {
        items.push_back(ContextMenu::Item{
            "Source",
            [this, leaf_id]() { move_source_panel_to_leaf(leaf_id); },
        });
    }

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_add_output_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "Network",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Network); },
    });
    items.push_back(ContextMenu::Item{
        "REPL",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Repl); },
    });
    items.push_back(ContextMenu::Item{
        "Console",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Console); },
    });

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_add_breakpoint_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "All",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Breakpoints, std::nullopt, std::nullopt); },
    });
    for (BreakpointRowKind kind :
         {BreakpointRowKind::Source, BreakpointRowKind::Data, BreakpointRowKind::Function,
          BreakpointRowKind::Exception}) {
        items.push_back(ContextMenu::Item{
            breakpoint_kind_label(kind),
            [this, leaf_id, kind]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Breakpoints, std::nullopt, kind); },
        });
    }

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::show_add_thread_menu(LayoutNodeId leaf_id, tuinator::Point anchor) {
    if (context_menu_ == nullptr) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "All",
        [this, leaf_id]() { add_panel_to_leaf(leaf_id, SidebarPanelType::Threads); },
    });
    items.push_back(ContextMenu::Item{
        thread_filter_label(ThreadPanelFilter::Stopped),
        [this, leaf_id]() {
            add_panel_to_leaf(leaf_id, SidebarPanelType::Threads, std::nullopt, std::nullopt,
                              ThreadPanelFilter::Stopped);
        },
    });
    items.push_back(ContextMenu::Item{
        thread_filter_label(ThreadPanelFilter::Running),
        [this, leaf_id]() {
            add_panel_to_leaf(leaf_id, SidebarPanelType::Threads, std::nullopt, std::nullopt,
                              ThreadPanelFilter::Running);
        },
    });
    for (const ThreadInfo& thread : available_thread_menu_threads()) {
        const std::string label =
            thread.name.empty() ? ("Thread " + std::to_string(thread.id)) : thread.name;
        items.push_back(ContextMenu::Item{
            label,
            [this, leaf_id, thread_id = thread.id, name = label]() {
                add_panel_to_leaf(leaf_id, SidebarPanelType::Threads, std::nullopt, std::nullopt, std::nullopt,
                                  thread_id, name);
            },
        });
    }

    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::add_panel_to_leaf(LayoutNodeId leaf_id, SidebarPanelType type, std::optional<std::string> scope_filter,
                                 std::optional<BreakpointRowKind> breakpoint_filter,
                                 std::optional<ThreadPanelFilter> thread_filter,
                                 std::optional<std::int64_t> thread_id_filter,
                                 std::optional<std::string> thread_name_filter) {
    if (type == SidebarPanelType::Source) {
        move_source_panel_to_leaf(leaf_id);
        return;
    }

    StackedPane* stack = layout_stack(leaf_id);
    if (stack == nullptr) {
        return;
    }

    PanelSlotConfig config;
    config.id = next_slot_id_++;
    config.type = type;
    config.scope_filter = scope_filter;
    config.breakpoint_filter = breakpoint_filter;
    config.thread_filter = thread_filter;
    config.thread_id_filter = thread_id_filter;
    config.thread_name_filter = thread_name_filter;
    config.tab_label = make_panel_tab_label(config, leaf_slot_configs(leaf_id));

    SidebarSlot slot;
    slot.config = config;
    leaf_slots(leaf_id).push_back(std::move(slot));
    SidebarSlot& new_slot = leaf_slots(leaf_id).back();

    const auto scroll_options = dap_theme_.scroll_view_options();
    ensure_sidebar_slot_panels(new_slot, scroll_options);
    if (new_slot.config.type == SidebarPanelType::Variables) {
        sync_scope_slot(new_slot);
    }
    if (new_slot.config.type == SidebarPanelType::Breakpoints) {
        sync_breakpoint_slot(new_slot, build_breakpoint_rows());
    }
    if (new_slot.config.type == SidebarPanelType::Threads) {
        sync_thread_slot(new_slot, build_thread_stack_contents());
    }
    if (new_slot.config.type == SidebarPanelType::Memory) {
        sync_memory_slot(new_slot);
        maybe_request_dap_panel_data();
    }
    if (new_slot.config.type == SidebarPanelType::DisassemblyAsm ||
        new_slot.config.type == SidebarPanelType::DisassemblyBytes) {
        sync_disassembly_slot(new_slot);
        maybe_request_dap_panel_data();
    }
    if (new_slot.config.type == SidebarPanelType::RuntimeSource) {
        sync_runtime_source_slot(new_slot);
        maybe_request_dap_panel_data();
    }
    if (new_slot.config.type == SidebarPanelType::FileTree) {
        sync_file_tree_slot(new_slot);
    }
    if (new_slot.config.type == SidebarPanelType::Resources) {
        sync_resources_slot(new_slot);
    }
    if (new_slot.config.type == SidebarPanelType::Network) {
        sync_network_slot(new_slot);
    }

    auto widget = release_sidebar_slot_widget(new_slot);
    widget->set_flex(1);
    stack->append_entry(new_slot.config.tab_label, std::move(widget));
    stack->set_active_index(stack->count() - 1);
    leaf_stack_index(leaf_id) = stack->active_index();

    update_active_panel_pointers();
    model_.focus = focus_for_panel_type(type);
    if (model_.focus == Focus::Repl) {
        repl_input_focused_ = false;
    }
    apply_focus();
    model_.status_message = "Added panel: " + new_slot.config.tab_label;
    sync_status_bar();
    request_repaint();
}

void DebugApp::add_panel_to_dock(PanelDock dock, SidebarPanelType type, std::optional<std::string> scope_filter,
                                 std::optional<BreakpointRowKind> breakpoint_filter,
                                 std::optional<ThreadPanelFilter> thread_filter,
                                 std::optional<std::int64_t> thread_id_filter,
                                 std::optional<std::string> thread_name_filter) {
    add_panel_to_leaf(dock_leaf_id(dock), type, scope_filter, breakpoint_filter, thread_filter, thread_id_filter,
                      thread_name_filter);
}

void DebugApp::init_default_bottom_slots(std::vector<SidebarSlot>& slots) {
    for (SidebarPanelType type :
         {SidebarPanelType::Repl, SidebarPanelType::Console, SidebarPanelType::Network}) {
        PanelSlotConfig config;
        config.type = type;
        config.id = next_slot_id_++;
        std::vector<PanelSlotConfig> existing;
        for (const SidebarSlot& slot : slots) {
            existing.push_back(slot.config);
        }
        config.tab_label = make_panel_tab_label(config, existing);
        SidebarSlot slot;
        slot.config = std::move(config);
        slots.push_back(std::move(slot));
    }
}

void DebugApp::init_default_source_slots(std::vector<SidebarSlot>& slots) {
    PanelSlotConfig config;
    config.type = SidebarPanelType::Source;
    config.id = next_slot_id_++;
    std::vector<PanelSlotConfig> existing;
    for (const SidebarSlot& slot : slots) {
        existing.push_back(slot.config);
    }
    config.tab_label = make_panel_tab_label(config, existing);
    SidebarSlot slot;
    slot.config = std::move(config);
    slots.push_back(std::move(slot));
}

void DebugApp::sync_source_stack_title() {
    const std::string title = panel_type_label(SidebarPanelType::Source);
    if (source_section_ != nullptr) {
        source_section_->set_title(title);
    }
    const std::optional<std::pair<LayoutNodeId, int>> source_loc = find_source_panel_slot();
    if (!source_loc.has_value()) {
        return;
    }

    SidebarSlot& slot = leaf_slots(source_loc->first)[static_cast<std::size_t>(source_loc->second)];
    if (slot.tab_label_customized) {
        return;
    }
    if (slot.config.tab_label == title) {
        return;
    }
    slot.config.tab_label = title;
    if (StackedPane* stack = layout_stack(source_loc->first); stack != nullptr) {
        stack->set_entry_label(source_loc->second, title);
    }
}

bool DebugApp::handle_stacked_pane_rename_key(const tuinator::Event& event) {
    for (const auto& [leaf_id, stack] : layout_stacks_) {
        if (stack != nullptr && stack->is_renaming()) {
            return stack->handle_event(event);
        }
    }
    return false;
}

void DebugApp::rename_leaf_panel(LayoutNodeId leaf_id, int index, const std::string& label) {
    if (SidebarSlot* slot = leaf_slot_at(leaf_id, index)) {
        slot->config.tab_label = label;
        if (slot->config.type == SidebarPanelType::Source) {
            slot->tab_label_customized = true;
        }
        model_.status_message = "Renamed panel: " + label;
        sync_status_bar();
    }
}

void DebugApp::rename_dock_panel(PanelDock dock, int index, const std::string& label) {
    rename_leaf_panel(dock_leaf_id(dock), index, label);
}

bool DebugApp::is_sidebar_focus(Focus focus) {
    return focus == Focus::Scopes || focus == Focus::Stacks || focus == Focus::Breakpoints ||
           focus == Focus::Watches || focus == Focus::Memory || focus == Focus::Disassembly ||
           focus == Focus::RuntimeSource || focus == Focus::FileTree || focus == Focus::Resources;
}

Focus DebugApp::focus_for_sidebar_index(int index) {
    if (const SidebarSlot* slot = sidebar_slot_at(index)) {
        return focus_for_panel_type(slot->config.type);
    }
    return Focus::Scopes;
}

int DebugApp::sidebar_index_for_focus(Focus focus) { return dock_index_for_focus(PanelDock::Left, focus); }

void DebugApp::sync_stack_panes_to_focus() {
    sync_dock_stack_to_focus(PanelDock::Left);
    sync_dock_stack_to_focus(PanelDock::Center);
    sync_dock_stack_to_focus(PanelDock::Bottom);
    update_active_panel_pointers();
}

void DebugApp::cycle_sidebar_stack(int delta) {
    if (const std::optional<LayoutNodeId> leaf_id = layout_tree_.find_leaf_for_dock(PanelDock::Left);
        leaf_id.has_value()) {
        if (StackedPane* stack = layout_stack(*leaf_id); stack != nullptr && delta != 0) {
            stack->cycle(delta);
        }
    }
}

void DebugApp::cycle_bottom_stack(int delta) {
    if (const std::optional<LayoutNodeId> leaf_id = layout_tree_.find_leaf_for_dock(PanelDock::Bottom);
        leaf_id.has_value()) {
        if (StackedPane* stack = layout_stack(*leaf_id); stack != nullptr && delta != 0) {
            stack->cycle(delta);
        }
    }
}

LayoutNodeId DebugApp::find_leaf_id_for_focus() const {
    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        const std::vector<SidebarSlot>& slots = layout_tree_.node(leaf_id).leaf.slots;
        for (const SidebarSlot& slot : slots) {
            if (focus_matches_panel_type(model_.focus, slot.config.type)) {
                return leaf_id;
            }
        }
    }
    if (model_.focus == Focus::Source) {
        if (const std::optional<LayoutNodeId> center = layout_tree_.find_leaf_for_dock(PanelDock::Center);
            center.has_value()) {
            return *center;
        }
    }
    if (focused_layout_leaf_ != 0 && layout_tree_.has_node(focused_layout_leaf_)) {
        return focused_layout_leaf_;
    }
    const std::vector<LayoutNodeId> leaves = layout_tree_.leaf_ids();
    return leaves.empty() ? 0 : leaves.front();
}

void DebugApp::sync_focused_layout_leaf() {
    const LayoutNodeId leaf_id = find_leaf_id_for_focus();
    if (leaf_id != 0) {
        focused_layout_leaf_ = leaf_id;
    }
}

void DebugApp::cycle_active_leaf_tabs(int delta) {
    if (delta == 0) {
        return;
    }
    sync_focused_layout_leaf();
    if (focused_layout_leaf_ == 0) {
        return;
    }
    StackedPane* stack = layout_stack(focused_layout_leaf_);
    if (stack == nullptr || stack->count() <= 1) {
        return;
    }
    stack->cycle(delta);
    model_.status_message = "Tab: " + stack->active_label();
    sync_status_bar();
    request_repaint();
}

void DebugApp::cycle_focused_layout_leaf(int delta) {
    const std::vector<LayoutNodeId> leaves = layout_tree_.leaf_ids();
    if (leaves.empty() || delta == 0) {
        return;
    }
    if (leaves.size() == 1) {
        focused_layout_leaf_ = leaves.front();
        return;
    }

    sync_focused_layout_leaf();
    auto it = std::find(leaves.begin(), leaves.end(), focused_layout_leaf_);
    const int current = it == leaves.end() ? 0 : static_cast<int>(std::distance(leaves.begin(), it));
    const int count = static_cast<int>(leaves.size());
    const int next = ((current + delta) % count + count) % count;
    focused_layout_leaf_ = leaves[static_cast<std::size_t>(next)];

    StackedPane* stack = layout_stack(focused_layout_leaf_);
    if (stack == nullptr) {
        return;
    }

    const int active = stack->active_index();
    if (SidebarSlot* slot = leaf_slot_at(focused_layout_leaf_, active); slot != nullptr) {
        model_.focus = focus_for_panel_type(slot->config.type);
    } else if (const std::optional<PanelDock> dock = layout_tree_.node(focused_layout_leaf_).leaf.dock) {
        if (*dock == PanelDock::Center) {
            model_.focus = Focus::Source;
        } else if (*dock == PanelDock::Bottom) {
            model_.focus = Focus::Repl;
        }
    }

    apply_focus();
    model_.status_message = "Window " + std::to_string(next + 1) + "/" + std::to_string(count) + ": " +
                            (stack != nullptr ? stack->active_label() : "");
    sync_status_bar();
    request_repaint();
}

bool DebugApp::handle_tab_navigation_key(const tuinator::KeyPress& key) {
    if (key.key != tuinator::Key::Tab) {
        return false;
    }

    if (is_watch_input_focused() || is_repl_input_focused() || is_breakpoint_input_focused() ||
        is_scope_input_focused() || is_memory_input_focused() ||
        (network_panel_ != nullptr && network_panel_->is_compose_input_focused())) {
        return false;
    }
    if (model_.focus == Focus::Memory && memory_toolbar_focused_) {
        return false;
    }

    const int delta = key.shift ? -1 : 1;
    if (key.ctrl) {
        cycle_focused_layout_leaf(delta);
        return true;
    }

    cycle_active_leaf_tabs(delta);
    return true;
}

bool DebugApp::handle_panel_swap_key(const tuinator::KeyPress& key) {
    if (key.ctrl || key.alt) {
        return false;
    }
    if (is_watch_input_focused() || is_repl_input_focused() || is_breakpoint_input_focused() ||
        is_scope_input_focused() || is_memory_input_focused()) {
        return false;
    }

    int delta = 0;
    if (app_config_.keybindings.matches(key, app_config_.keybindings.panel_prev) ||
        app_config_.keybindings.matches(key, app_config_.keybindings.panel_prev_alt)) {
        delta = -1;
    } else if (app_config_.keybindings.matches(key, app_config_.keybindings.panel_next) ||
               app_config_.keybindings.matches(key, app_config_.keybindings.panel_next_alt)) {
        delta = 1;
    } else {
        return false;
    }

    if (is_sidebar_focus(model_.focus)) {
        cycle_sidebar_stack(delta);
        return true;
    }
    if (model_.focus == Focus::Repl || model_.focus == Focus::Console || model_.focus == Focus::Network) {
        cycle_bottom_stack(delta);
        return true;
    }
    return false;
}

bool DebugApp::handle_layout_resize_key(const tuinator::KeyPress& key) {
    if (!key.alt) {
        return false;
    }

    switch (key.key) {
    case tuinator::Key::Left:
        model_.layout.narrow_sidebar();
        model_.status_message = "Left " + std::to_string(model_.layout.sidebar_pct) + "%";
        build_ui();
        return true;
    case tuinator::Key::Right:
        model_.layout.widen_sidebar();
        model_.status_message = "Left " + std::to_string(model_.layout.sidebar_pct) + "%";
        build_ui();
        return true;
    case tuinator::Key::Up:
        model_.layout.shrink_bottom();
        model_.status_message = "Bottom " + std::to_string(model_.layout.bottom_pct) + "%";
        build_ui();
        return true;
    case tuinator::Key::Down:
        model_.layout.grow_bottom();
        model_.status_message = "Bottom " + std::to_string(model_.layout.bottom_pct) + "%";
        build_ui();
        return true;
    default:
        break;
    }

    return false;
}

bool DebugApp::is_watch_input_focused() const {
    return watches_panel_ != nullptr && watches_panel_->has_active_inline_edit();
}

bool DebugApp::is_repl_input_focused() const {
    return repl_panel_ != nullptr && repl_panel_->input_widget() != nullptr &&
           repl_panel_->input_widget()->is_focused();
}

bool DebugApp::is_breakpoint_input_focused() const {
    return breakpoint_prompt_active();
}

bool DebugApp::is_scope_input_focused() const {
    return (scopes_panel_ != nullptr && scopes_panel_->has_active_inline_edit()) || scope_prompt_active();
}

bool DebugApp::is_memory_input_focused() const { return memory_write_active_ || memory_toolbar_focused_; }

bool DebugApp::should_block_app_quit_key(const tuinator::KeyPress& key) const {
    if (step_in_selection_active()) {
        return true;
    }
    if (context_menu_open()) {
        return true;
    }
    if (is_watch_input_focused() || is_repl_input_focused() || is_breakpoint_input_focused() ||
        is_scope_input_focused() || is_memory_input_focused()) {
        return true;
    }
    if (model_.focus == Focus::Console) {
        return key.character == 'q' || key.character == 'Q' || key.key == tuinator::Key::Escape;
    }
    if (model_.focus == Focus::Repl) {
        return key.character == 'q' || key.character == 'Q' || key.key == tuinator::Key::Escape;
    }
    if (model_.focus == Focus::Watches) {
        return key.character == 'q' || key.character == 'Q' || key.key == tuinator::Key::Escape;
    }
    return false;
}

void DebugApp::blur_watch_input() {
    finish_watch_input();
}

void DebugApp::finish_watch_input() {
    watch_input_draft_.clear();
    watch_input_focused_ = false;
    editing_watch_index_ = -1;
    if (watches_panel_ == nullptr) {
        return;
    }
    watches_panel_->clear_inline_edit();
    sync_watches_panel();
    if (watches_panel_->list_widget() != nullptr) {
        watches_panel_->list_widget()->set_focused(true);
    }
    model_.focus = Focus::Watches;
    apply_focus();
}

void DebugApp::blur_scope_input() {
    if (!editing_variable_name_.empty()) {
        scope_value_overrides_.erase(editing_variable_name_);
    }
    variable_set_in_flight_ = false;
    editing_variable_name_.clear();
    editing_variables_reference_ = 0;
    scope_input_draft_.clear();
    scope_input_focused_ = false;
    if (scopes_panel_ == nullptr) {
        return;
    }
    scopes_panel_->clear_inline_edit();
    sync_scopes_list_panel();
    if (scopes_panel_->list_widget() != nullptr) {
        scopes_panel_->list_widget()->set_focused(true);
    }
    model_.focus = Focus::Scopes;
    apply_focus();
    request_repaint();
}

void DebugApp::blur_memory_input() {
    memory_write_active_ = false;
    memory_write_row_ = -1;
    if (SidebarSlot* slot = slot_by_id(memory_write_slot_id_); slot != nullptr && slot->memory != nullptr) {
        slot->memory->clear_inline_edit();
        if (slot->memory->list_widget() != nullptr) {
            slot->memory->list_widget()->set_focused(true);
        }
    }
    model_.focus = Focus::Memory;
    apply_focus();
    request_repaint();
}

void DebugApp::blur_memory_toolbar_inputs() {
    memory_toolbar_focused_ = false;
    if (SidebarSlot* slot = active_slot_for_focus(); slot != nullptr && slot->memory != nullptr) {
        slot->memory->blur_toolbar_inputs();
    }
    model_.focus = Focus::Memory;
    apply_focus();
    request_repaint();
}

void DebugApp::blur_active_memory_input() {
    if (memory_toolbar_focused_) {
        blur_memory_toolbar_inputs();
        return;
    }
    blur_memory_input();
}

bool DebugApp::handle_global_key(const tuinator::KeyPress& key) {
    if ((layout_drag_.active || layout_drag_.pending.has_value()) && key.key == tuinator::Key::Escape) {
        cancel_layout_drag();
        return true;
    }

    if (handle_step_in_selection_key(key)) {
        return true;
    }

    if (!key.ctrl && !key.alt) {
        if (app_config_.keybindings.matches(key, app_config_.keybindings.breakpoint_fn)) {
            if (source_panel_ != nullptr && !effective_source_path().empty() && source_panel_->cursor_line() > 0) {
                toggle_breakpoint();
                return true;
            }
        } else if (has_active_session()) {
            const char* op = nullptr;
            if (app_config_.keybindings.matches(key, app_config_.keybindings.continue_fn)) {
                op = "continue";
            } else if (app_config_.keybindings.matches(key, app_config_.keybindings.step_over_fn)) {
                op = "step_over";
            } else if (app_config_.keybindings.matches(key, app_config_.keybindings.step_out_shift_fn)) {
                op = "step_out";
            } else if (app_config_.keybindings.matches(key, app_config_.keybindings.step_into_fn)) {
                op = "step_into";
            } else if (app_config_.keybindings.matches(key, app_config_.keybindings.step_out_fn)) {
                op = "step_out";
            }
            if (op != nullptr) {
                send_command(op);
                return true;
            }
        }
    }

    if (handle_panel_swap_key(key)) {
        return true;
    }

    if (handle_tab_navigation_key(key)) {
        return true;
    }

    if (model_.focus == Focus::Console || model_.focus == Focus::Repl) {
        return false;
    }

    if (key.ctrl && key.character == 'c') {
        if (app_ != nullptr) {
            app_->quit();
        }
        return true;
    }

    if (is_watch_input_focused() || is_repl_input_focused() || is_breakpoint_input_focused() ||
        is_scope_input_focused() || is_memory_input_focused() ||
        (network_panel_ != nullptr && network_panel_->is_compose_input_focused())) {
        if (key.alt && handle_layout_resize_key(key)) {
            return true;
        }
        return false;
    }

    if (handle_layout_resize_key(key)) {
        return true;
    }

    if (model_.focus == Focus::Network && !key.ctrl && !key.alt && network_panel_ != nullptr) {
        const char action = static_cast<char>(std::tolower(static_cast<unsigned char>(key.character)));
        if (action == 'v') {
            network_panel_->cycle_view();
            model_.status_message = network_panel_->active_view() == NetworkPanelView::Compose
                                        ? "Network: Compose tab"
                                        : "Network: Traffic tab";
            sync_status_bar();
            apply_focus();
            request_repaint();
            return true;
        }
        if (!network_panel_->is_compose_input_focused() &&
            (action == 'f' || action == 'd' || action == 'e' || action == 'r' || action == 's' || action == 'n')) {
            if (network_panel_->perform_action(action)) {
                request_repaint();
            }
            return true;
        }
    }

    if (key.ctrl || key.alt) {
        return false;
    }

    if (model_.focus == Focus::Memory && (key.character == 'e' || key.character == 'E')) {
        if (SidebarSlot* slot = memory_slot_for_focus(); slot != nullptr && slot->memory != nullptr) {
            const int row = slot->memory->selected_row();
            if (row >= 0) {
                begin_memory_row_edit(slot->config.id, row);
            }
        }
        return true;
    }

    if (model_.focus == Focus::Memory && (key.character == 'g' || key.character == 'G')) {
        if (SidebarSlot* slot = memory_slot_for_focus(); slot != nullptr && slot->memory != nullptr) {
            memory_focus_slot_id_ = slot->config.id;
            memory_toolbar_focused_ = true;
            slot->memory->focus_address_input();
            apply_focus();
            model_.status_message = "Addr: hex (0x…) or expression like &g_buffer";
            sync_status_bar();
            request_repaint();
        }
        return true;
    }

    if (model_.focus == Focus::Memory && key.character == '/') {
        if (SidebarSlot* slot = memory_slot_for_focus(); slot != nullptr && slot->memory != nullptr) {
            memory_focus_slot_id_ = slot->config.id;
            memory_toolbar_focused_ = true;
            slot->memory->focus_search_input();
            apply_focus();
            model_.status_message = "Find: text or hex bytes in loaded memory";
            sync_status_bar();
            request_repaint();
        }
        return true;
    }

    if (model_.focus == Focus::Memory && (key.character == 'n' || key.character == 'N')) {
        if (SidebarSlot* slot = memory_slot_for_focus(); slot != nullptr && slot->memory != nullptr) {
            const std::string query = slot->memory->search_value();
            if (!query.empty()) {
                submit_memory_search(slot->config.id, query, key.character == 'n');
            }
        }
        return true;
    }

    if (key.character >= '1' && key.character <= '9') {
        static constexpr const char* kControlOps[] = {
            "play_pause",     "step_into",   "step_over",        "step_out",      "step_back",
            "step_back_into", "reverse_continue", "restart", "terminate", "disconnect"};
        send_command(kControlOps[key.character - '1']);
        return true;
    }
    if (key.character == '0') {
        send_command("disconnect");
        return true;
    }

    if (app_config_.keybindings.matches(key, app_config_.keybindings.focus_repl)) {
        model_.focus = Focus::Repl;
        const int repl_index = dock_index_for_focus(PanelDock::Bottom, Focus::Repl);
        if (repl_index >= 0) {
            dock_stack_index(PanelDock::Bottom) = repl_index;
            if (StackedPane* bottom_stack = dock_stack(PanelDock::Bottom); bottom_stack != nullptr) {
                bottom_stack->set_active_index(repl_index);
            }
        }
        repl_input_focused_ = true;
        apply_focus();
        if (repl_panel_ != nullptr) {
            repl_panel_->focus_input();
        }
        model_.status_message = "REPL — type expression, Enter to evaluate";
        sync_status_bar();
        return true;
    }

    if (app_config_.keybindings.matches(key, app_config_.keybindings.follow_execution)) {
        follow_execution_ = !follow_execution_;
        if (follow_execution_) {
            maybe_follow_execution();
        }
        model_.status_message = follow_execution_ ? "Follow execution on" : "Follow execution off";
        sync_status_bar();
        return true;
    }

    if (app_config_.keybindings.matches(key, app_config_.keybindings.file_picker)) {
        if (file_picker_open()) {
            close_file_picker();
        } else {
            open_file_picker();
        }
        return true;
    }

    if (file_picker_open()) {
        return false;
    }

    if (is_breakpoint_input_focused() || is_scope_input_focused() || is_memory_input_focused()) {
        if (key.alt && handle_layout_resize_key(key)) {
            return true;
        }
        return false;
    }

    if ((key.character == 'm' || key.character == 'M') && !key.ctrl && !key.alt) {
        if (model_.focus == Focus::Source && source_panel_ != nullptr) {
            const std::string path = effective_source_path();
            const int line = source_panel_->cursor_line();
            if (!path.empty() && line > 0) {
                const tuinator::Point anchor = source_panel_->context_menu_anchor(line, 0);
                std::optional<SourceContextIdentifier> source_identifier;
                if (cached_source_path_ != path) {
                    cached_source_path_ = path;
                    cached_source_text_ = read_file_or_empty(path);
                }
                source_identifier = resolve_source_identifier(path, cached_source_text_, line, 0);
                show_breakpoint_context_menu(path, line, anchor, source_identifier);
                return true;
            }
        } else if (model_.focus == Focus::Breakpoints && breakpoints_panel_ != nullptr) {
            if (const BreakpointRow* row = breakpoints_panel_->selected_row()) {
                tuinator::Point anchor{0, 0};
                if (breakpoints_panel_->list_widget() != nullptr) {
                    const tuinator::Rect list_bounds = breakpoints_panel_->list_widget()->bounds();
                    anchor = {list_bounds.x + 2, list_bounds.y + 2};
                }
                std::optional<SourceContextIdentifier> source_identifier;
                const int line_width = tuinator::text_display_width(row->source_text);
                for (int column = 0; column < line_width; ++column) {
                    source_identifier = resolve_source_identifier(row->path, row->source_text, row->line, column);
                    if (source_identifier.has_value()) {
                        break;
                    }
                }
                show_breakpoint_context_menu(row->path, row->line, anchor, source_identifier);
                return true;
            }
        }
    }

    if (!key.ctrl && !key.alt && source_panel_ != nullptr && model_.focus == Focus::Source &&
        (app_config_.keybindings.matches(key, app_config_.keybindings.breakpoint) || app_config_.keybindings.matches(key, app_config_.keybindings.breakpoint_alt))) {
        if (!source_panel_->is_focused()) {
            apply_focus();
        }
        toggle_breakpoint();
        return true;
    }

    if (key.character == 'c' && !key.ctrl && !key.alt) {
        if (model_.focus == Focus::Source && source_panel_ != nullptr) {
            const std::string path = effective_source_path();
            const int line = source_panel_->cursor_line();
            const auto file_it = breakpoints_by_path_.find(path);
            if (file_it != breakpoints_by_path_.end() && file_it->second.contains(line)) {
                tuinator::Point anchor{0, 0};
                if (source_panel_ != nullptr) {
                    const tuinator::Rect bounds = source_panel_->bounds();
                    anchor = {bounds.x + 2, bounds.y + 2};
                }
                open_breakpoint_condition_editor(path, line, anchor, std::nullopt);
                return true;
            }
        } else if (model_.focus == Focus::Breakpoints && breakpoints_panel_ != nullptr) {
            const BreakpointRow* row = breakpoints_panel_->selected_row();
            if (row != nullptr && row->kind == BreakpointRowKind::Source && row->line > 0 && !row->path.empty()) {
                open_breakpoint_condition_editor(row->path, row->line, std::nullopt,
                                                 breakpoints_panel_->selected_index());
                return true;
            }
        }
    }

    if (model_.focus == Focus::Breakpoints && breakpoints_panel_ != nullptr) {
        const BreakpointRow* row = breakpoints_panel_->selected_row();
        if (row != nullptr && row->kind == BreakpointRowKind::Source) {
            if (key.character == 'd') {
                remove_breakpoint_at(row->path, row->line);
                return true;
            }
            if (key.character == 't' || key.character == ' ') {
                toggle_breakpoint_at(row->path, row->line);
                return true;
            }
        }
    }

    if (model_.focus == Focus::Watches && watches_panel_ != nullptr) {
        if (key.character == 'd') {
            const int index = watches_panel_->selected_watch_index();
            if (index >= 0) {
                remove_watch_at(static_cast<std::size_t>(index));
            }
            return true;
        }
        if (key.character == 'w' || key.key == tuinator::Key::Enter) {
            begin_add_watch();
            return true;
        }
    }

    if (!has_active_session()) {
        return false;
    }

    if (app_config_.keybindings.matches(key, app_config_.keybindings.continue_key)) {
        send_command("continue");
        return true;
    }
    if (app_config_.keybindings.matches(key, app_config_.keybindings.step_over)) {
        send_command("step_over");
        return true;
    }
    if (app_config_.keybindings.matches(key, app_config_.keybindings.step_into)) {
        send_command("step_into");
        return true;
    }
    if (app_config_.keybindings.matches(key, app_config_.keybindings.step_out)) {
        send_command("step_out");
        return true;
    }

    return false;
}

void DebugApp::apply_execution_command_started(const char* op) {
    if (op == nullptr) {
        return;
    }

    if (std::strcmp(op, "continue") == 0 || std::strcmp(op, "play_pause") == 0) {
        if (is_session_stopped()) {
            model_.session_state = "running";
            model_.stop_reason.clear();
            last_counted_breakpoint_stop_.reset();
            clear_scope_value_overrides();
        } else if (std::strcmp(op, "play_pause") == 0) {
            model_.status_message = "Pausing…";
        }
        return;
    }

    if (std::strcmp(op, "step_over") == 0 || std::strcmp(op, "step_into") == 0 || std::strcmp(op, "step_out") == 0 ||
        std::strcmp(op, "step_back") == 0 || std::strcmp(op, "reverse_continue") == 0) {
        last_counted_breakpoint_stop_.reset();
    }

    if (std::strcmp(op, "pause") == 0) {
        model_.status_message = "Pausing…";
        return;
    }

    if (std::strcmp(op, "step_over") == 0 || std::strcmp(op, "next") == 0 || std::strcmp(op, "step_into") == 0 ||
        std::strcmp(op, "step_in") == 0 || std::strcmp(op, "step_out") == 0 || std::strcmp(op, "step_back") == 0 ||
        std::strcmp(op, "step_back_into") == 0 || std::strcmp(op, "reverse_continue") == 0 ||
        std::strcmp(op, "goto") == 0) {
        model_.session_state = "running";
        model_.stop_reason.clear();
        clear_scope_value_overrides();
        return;
    }

    if (std::strcmp(op, "restart") == 0) {
        model_.stop_reason.clear();
        invalidate_scope_variables();
        return;
    }

    if (std::strcmp(op, "disconnect") == 0) {
        model_.session_state = "disconnected";
        model_.stop_reason.clear();
        invalidate_scope_variables();
        return;
    }

    if (std::strcmp(op, "terminate") == 0) {
        model_.session_state = "exited";
        model_.stop_reason.clear();
        invalidate_scope_variables();
    }
}

bool DebugApp::step_in_selection_active() const {
    return step_in_selection_.has_value() && step_in_selection_->active();
}

void DebugApp::sync_step_in_selection_to_panel() {
    if (source_panel_ == nullptr) {
        return;
    }
    source_panel_->set_step_in_selection(step_in_selection_);
    source_panel_->mark_dirty();
    sync_status_bar();
}

void DebugApp::cancel_step_in_selection() {
    if (!step_in_selection_active()) {
        return;
    }
    step_in_selection_.reset();
    sync_step_in_selection_to_panel();
}

std::string DebugApp::execution_line_source_text() const {
    const int line = source_panel_ != nullptr && source_panel_->execution_line() > 0
                         ? source_panel_->execution_line()
                         : static_cast<int>(model_.execution_line);
    if (line <= 0) {
        return {};
    }
    if (!cached_source_text_.empty()) {
        return line_text_at(cached_source_text_, line);
    }
    if (source_panel_ != nullptr) {
        return highlighted_line_text(source_panel_, line);
    }
    return {};
}

void DebugApp::begin_step_in_selection(std::vector<StepInTargetSpan> targets) {
    if (targets.size() <= 1) {
        return;
    }

    const int line = source_panel_ != nullptr && source_panel_->execution_line() > 0
                         ? source_panel_->execution_line()
                         : static_cast<int>(model_.execution_line);
    if (line <= 0) {
        model_.status_message = "No execution line for step-in selection";
        sync_status_bar();
        return;
    }

    StepInSelectionState selection{};
    selection.line = line;
    selection.targets = std::move(targets);
    selection.active_index = 0;
    step_in_selection_ = std::move(selection);

    model_.focus = Focus::Source;
    apply_focus();
    if (source_panel_ != nullptr) {
        source_panel_->set_cursor_line(line);
        source_panel_->ensure_cursor_visible();
    }

    update_step_in_status_message();
    sync_step_in_selection_to_panel();
    sync_ui_from_model();
}

void DebugApp::update_step_in_status_message() {
    if (!step_in_selection_active()) {
        return;
    }
    const StepInTargetSpan* active = step_in_selection_->active_target();
    const std::string label = active != nullptr ? active->label : "target";
    model_.status_message = "Step into [" + std::to_string(step_in_selection_->active_index + 1) + "/" +
                            std::to_string(step_in_selection_->targets.size()) + "]: " + label +
                            "  |  click target  |  ←/→ choose  |  i/Enter confirm  |  Esc cancel";
}

std::int64_t DebugApp::current_frame_id() const {
    if (!model_.stack_frames.empty()) {
        return model_.stack_frames.front().id;
    }
    return 0;
}

bool DebugApp::handle_step_in_request() {
    if (!is_session_stopped() || step_in_selection_active() || step_in_targets_pending_) {
        return false;
    }
    if (session_io_ == nullptr || !session_io_->is_active()) {
        return false;
    }

    const std::int64_t frame_id = current_frame_id();
    if (frame_id <= 0) {
        model_.status_message = "No active frame for step into";
        sync_status_bar();
        return true;
    }

    step_in_targets_pending_ = true;
    session_io_->request_step_in_targets(frame_id);
    model_.status_message = "Resolving step-in targets…";
    sync_status_bar();
    return true;
}

void DebugApp::handle_step_in_targets_payload(const SessionIoEvent& event) {
    step_in_targets_pending_ = false;

    const std::string line_text = execution_line_source_text();
    std::vector<StepInTargetSpan> targets;
    if (event.success) {
        std::string error;
        targets = build_step_in_target_spans(line_text, event.payload, error);
        if (!error.empty() && targets.empty()) {
            model_.status_message = "Step-in targets failed: " + error;
            sync_status_bar();
            return;
        }
    } else if (!line_text.empty()) {
        targets = find_step_in_targets_on_line(line_text);
        if (!event.payload.empty()) {
            model_.status_message = "Step-in targets failed: " + event.payload;
        }
    }

    if (targets.empty()) {
        send_step_into_command(std::nullopt);
        return;
    }
    if (targets.size() == 1) {
        send_step_into_command(targets.front().target_id >= 0 ? std::optional<std::int64_t>(targets.front().target_id)
                                                              : std::nullopt);
        return;
    }

    begin_step_in_selection(std::move(targets));
}

void DebugApp::send_step_into_command(std::optional<std::int64_t> target_id) {
    if (session_io_ == nullptr || !session_io_->is_active()) {
        model_.status_message = "No active session";
        sync_status_bar();
        return;
    }

    apply_execution_command_started("step_into");
    if (target_id.has_value() && *target_id >= 0) {
        session_io_->post_command(R"({"op":"step_into","target_id":)" + std::to_string(*target_id) + "}");
        model_.status_message = "Step into";
    } else {
        session_io_->post_command("step_into");
        model_.status_message = "Step into";
    }
    sync_ui_from_model();
}

void DebugApp::send_command_direct(const char* op) {
    if (op == nullptr || session_io_ == nullptr || !session_io_->is_active()) {
        model_.status_message = "No active session";
        sync_status_bar();
        return;
    }

    apply_execution_command_started(op);
    if (!command_state_synced_via_snapshot(op)) {
        model_.status_message = command_status_message(op);
    }
    session_io_->post_command(op);
    sync_ui_from_model();
}

void DebugApp::cycle_step_in_target(int delta) {
    if (!step_in_selection_active()) {
        return;
    }

    const int count = static_cast<int>(step_in_selection_->targets.size());
    if (count <= 0) {
        return;
    }

    int next = step_in_selection_->active_index + delta;
    next %= count;
    if (next < 0) {
        next += count;
    }
    step_in_selection_->active_index = next;

    update_step_in_status_message();
    sync_step_in_selection_to_panel();
    sync_status_bar();
}

void DebugApp::confirm_step_in_selection() {
    if (!step_in_selection_active()) {
        return;
    }

    const StepInTargetSpan* active = step_in_selection_->active_target();
    const std::string label = active != nullptr ? active->label : "target";
    const std::optional<std::int64_t> target_id =
        active != nullptr && active->target_id >= 0 ? std::optional<std::int64_t>(active->target_id) : std::nullopt;
    cancel_step_in_selection();
    model_.status_message = "Step into " + label;
    sync_status_bar();
    send_step_into_command(target_id);
}

bool DebugApp::handle_step_in_selection_key(const tuinator::KeyPress& key) {
    if (!step_in_selection_active()) {
        return false;
    }

    if (key.key == tuinator::Key::Escape) {
        cancel_step_in_selection();
        model_.status_message = "Step into cancelled";
        sync_status_bar();
        return true;
    }

    if (key.key == tuinator::Key::Left || key.character == 'h') {
        cycle_step_in_target(-1);
        return true;
    }
    if (key.key == tuinator::Key::Right || key.character == 'l') {
        cycle_step_in_target(1);
        return true;
    }
    if (key.key == tuinator::Key::Up || key.character == 'k') {
        cycle_step_in_target(-1);
        return true;
    }
    if (key.key == tuinator::Key::Down || key.character == 'j') {
        cycle_step_in_target(1);
        return true;
    }
    if (key.key == tuinator::Key::Enter || key.character == 'i') {
        confirm_step_in_selection();
        return true;
    }
    if (key.character >= '1' && key.character <= '8') {
        if (key.character == '2') {
            confirm_step_in_selection();
        }
        return true;
    }

    return true;
}

void DebugApp::send_command(const char* op) {
    if (op == nullptr) {
        return;
    }

    if (try_skip_exception_runtime_to_catch(op)) {
        return;
    }

    if (std::strcmp(op, "step_into") == 0) {
        if (step_in_selection_active()) {
            confirm_step_in_selection();
            return;
        }
        if (handle_step_in_request()) {
            sync_ui_from_model();
            return;
        }
    }

    if (!session_io_->is_active()) {
        model_.status_message = "No active session";
        sync_ui_from_model();
        return;
    }

    if (std::strcmp(op, "restart") == 0) {
        restart_pending_ = true;
        scope_variables_fetch_pending_ = false;
        apply_execution_command_started(op);
        mark_all_panels_dirty();
    } else if (std::strcmp(op, "terminate") == 0 || std::strcmp(op, "disconnect") == 0) {
        apply_execution_command_started(op);
    } else if (command_state_synced_via_snapshot(op)) {
        // Snapshot from the IO thread owns the next stopped/running state.
    } else {
        apply_execution_command_started(op);
    }

    model_.status_message = command_status_message(op);
    session_io_->post_command(op);
    sync_ui_from_model();
}

void DebugApp::cycle_focus_next() {
    switch (model_.focus) {
    case Focus::Source:
        model_.focus = Focus::Scopes;
        break;
    case Focus::Scopes:
        model_.focus = Focus::Stacks;
        break;
    case Focus::Stacks:
        model_.focus = Focus::Breakpoints;
        break;
    case Focus::Breakpoints:
        model_.focus = Focus::Watches;
        break;
    case Focus::Watches:
        model_.focus = Focus::Memory;
        break;
    case Focus::Memory:
        model_.focus = Focus::Disassembly;
        break;
    case Focus::Disassembly:
        model_.focus = Focus::RuntimeSource;
        break;
    case Focus::RuntimeSource:
        model_.focus = Focus::FileTree;
        break;
    case Focus::FileTree:
        model_.focus = Focus::Resources;
        break;
    case Focus::Resources:
        model_.focus = Focus::Network;
        break;
    case Focus::Network:
        model_.focus = Focus::Repl;
        repl_input_focused_ = true;
        break;
    case Focus::Repl:
        model_.focus = Focus::Console;
        break;
    default:
        model_.focus = Focus::Source;
        break;
    }
    apply_focus();
    model_.status_message = "Focus: " + model_.focus_label();
    sync_ui_from_model();
}

void DebugApp::apply_focus() {
    sync_stack_panes_to_focus();
    sync_focused_layout_leaf();

    if (model_.focus != Focus::Watches) {
        watch_input_focused_ = false;
    }
    if (model_.focus != Focus::Repl) {
        repl_input_focused_ = false;
        if (repl_panel_ != nullptr) {
            repl_panel_->set_input_active(false);
        }
    }
    if (model_.focus != Focus::Breakpoints && !breakpoint_prompt_active()) {
        breakpoint_input_focused_ = false;
    }
    if (model_.focus != Focus::Scopes) {
        scope_input_focused_ = false;
        if (editing_variable_name_.empty()) {
            scope_input_draft_.clear();
        }
    }
    if (model_.focus != Focus::Memory && !memory_write_active_) {
        memory_toolbar_focused_ = false;
    }

    std::vector<tuinator::Widget*> widgets;
    if (source_panel_ != nullptr) {
        widgets.push_back(source_panel_);
    }
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.scopes != nullptr && slot.scopes->list_widget() != nullptr) {
            widgets.push_back(slot.scopes->list_widget());
        }
        if (slot.watches != nullptr && slot.watches->list_widget() != nullptr) {
            widgets.push_back(slot.watches->list_widget());
        }
        if (slot.stacks != nullptr && slot.stacks->list_widget() != nullptr) {
            widgets.push_back(slot.stacks->list_widget());
        }
        if (slot.breakpoints != nullptr && slot.breakpoints->list_widget() != nullptr) {
            widgets.push_back(slot.breakpoints->list_widget());
        }
        if (slot.memory != nullptr) {
            if (slot.memory->list_widget() != nullptr) {
                widgets.push_back(slot.memory->list_widget());
            }
            if (slot.memory->address_input_widget() != nullptr) {
                widgets.push_back(slot.memory->address_input_widget());
            }
            if (slot.memory->search_input_widget() != nullptr) {
                widgets.push_back(slot.memory->search_input_widget());
            }
        }
        if (slot.disassembly != nullptr && slot.disassembly->list_widget() != nullptr) {
            widgets.push_back(slot.disassembly->list_widget());
        }
        if (slot.runtime_source != nullptr && slot.runtime_source->list_widget() != nullptr) {
            widgets.push_back(slot.runtime_source->list_widget());
        }
        if (slot.file_tree != nullptr && slot.file_tree->list_widget() != nullptr) {
            widgets.push_back(slot.file_tree->list_widget());
        }
    });
    if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr) {
        widgets.push_back(stacks_panel_->list_widget());
    }
    if (breakpoints_panel_ != nullptr && breakpoints_panel_->list_widget() != nullptr) {
        widgets.push_back(breakpoints_panel_->list_widget());
    }
    if (repl_panel_ != nullptr) {
        if (repl_panel_->shell_widget() != nullptr) {
            widgets.push_back(repl_panel_->shell_widget());
        }
        if (repl_panel_->history_widget() != nullptr) {
            widgets.push_back(repl_panel_->history_widget());
        }
        if (repl_panel_->input_widget() != nullptr) {
            widgets.push_back(repl_panel_->input_widget());
        }
    }
    if (console_panel_ != nullptr) {
        widgets.push_back(console_panel_);
    }
    if (network_panel_ != nullptr) {
        network_panel_->collect_focusable(widgets);
    }

    for (tuinator::Widget* widget : widgets) {
        widget->set_focused(false);
    }

    tuinator::Widget* target = nullptr;
    switch (model_.focus) {
    case Focus::Source:
        target = source_panel_;
        break;
    case Focus::Scopes:
        target = scopes_panel_ != nullptr ? scopes_panel_->list_widget() : nullptr;
        break;
    case Focus::Stacks:
        target = stacks_panel_ != nullptr ? stacks_panel_->list_widget() : nullptr;
        break;
    case Focus::Breakpoints:
        target = breakpoints_panel_ != nullptr ? breakpoints_panel_->list_widget() : nullptr;
        break;
    case Focus::Watches:
        target = watches_panel_ != nullptr ? watches_panel_->list_widget() : nullptr;
        if (watch_input_focused_ && watches_panel_ != nullptr && watches_panel_->has_inline_edit()) {
            watches_panel_->focus_inline_edit();
        }
        break;
    case Focus::Memory:
        if (SidebarSlot* slot = memory_slot_for_focus(); slot != nullptr && slot->memory != nullptr) {
            if (memory_write_active_ && slot->memory->has_inline_edit()) {
                target = slot->memory->list_widget();
            } else if (memory_toolbar_focused_) {
                if (slot->memory->is_search_toolbar_active()) {
                    slot->memory->focus_search_input();
                    target = slot->memory->search_input_widget();
                } else {
                    slot->memory->focus_address_input();
                    target = slot->memory->address_input_widget();
                }
            } else {
                target = slot->memory->list_widget();
            }
        }
        break;
    case Focus::Disassembly:
        if (SidebarSlot* slot = active_slot_for_focus(); slot != nullptr && slot->disassembly != nullptr) {
            target = slot->disassembly->list_widget();
        }
        break;
    case Focus::RuntimeSource:
        if (SidebarSlot* slot = active_slot_for_focus(); slot != nullptr && slot->runtime_source != nullptr) {
            target = slot->runtime_source->list_widget();
        }
        break;
    case Focus::FileTree:
        if (SidebarSlot* slot = active_slot_for_focus(); slot != nullptr && slot->file_tree != nullptr) {
            target = slot->file_tree->list_widget();
        }
        break;
    case Focus::Network:
        if (network_panel_ != nullptr) {
            target = network_panel_->focus_widget();
        }
        break;
    case Focus::Repl:
        if (repl_input_focused_ && repl_panel_ != nullptr) {
            repl_panel_->set_input_active(true);
            target = repl_panel_->input_widget();
            if (repl_panel_->shell_widget() != nullptr) {
                repl_panel_->shell_widget()->set_focused(true);
            }
        } else if (repl_panel_ != nullptr) {
            repl_panel_->set_input_active(false);
            target = repl_panel_->shell_widget();
        }
        break;
    case Focus::Console:
        target = console_panel_;
        break;
    default:
        target = source_panel_;
        break;
    }

    if (target != nullptr) {
        target->set_focused(true);
    }
    mark_all_panels_dirty();
}

void DebugApp::sync_focus_from_ui() {
    Focus detected = model_.focus;

    if (breakpoints_panel_ != nullptr && breakpoints_panel_->list_widget() != nullptr &&
        breakpoints_panel_->list_widget()->is_focused()) {
        detected = Focus::Breakpoints;
        breakpoint_input_focused_ = breakpoints_panel_->has_inline_edit();
    } else {
        bool panel_list_focused = false;
        for_each_sidebar_slot([&](SidebarSlot& slot) {
            if (panel_list_focused) {
                return;
            }
            if (slot.watches != nullptr && slot.watches->list_widget() != nullptr &&
                slot.watches->list_widget()->is_focused()) {
                detected = Focus::Watches;
                watch_input_focused_ = slot.watches->has_inline_edit();
                panel_list_focused = true;
                return;
            }
            if (slot.scopes != nullptr && slot.scopes->list_widget() != nullptr &&
                slot.scopes->list_widget()->is_focused()) {
                detected = Focus::Scopes;
                scope_input_focused_ = slot.scopes->has_inline_edit();
                panel_list_focused = true;
                return;
            }
            if (slot.stacks != nullptr && slot.stacks->list_widget() != nullptr &&
                slot.stacks->list_widget()->is_focused()) {
                detected = Focus::Stacks;
                panel_list_focused = true;
                return;
            }
            if (slot.breakpoints != nullptr && slot.breakpoints->list_widget() != nullptr &&
                slot.breakpoints->list_widget()->is_focused()) {
                detected = Focus::Breakpoints;
                breakpoint_input_focused_ = slot.breakpoints->has_inline_edit();
                panel_list_focused = true;
                return;
            }
            if (slot.memory != nullptr && slot.memory->is_toolbar_active()) {
                detected = Focus::Memory;
                memory_focus_slot_id_ = slot.config.id;
                memory_toolbar_focused_ = true;
                panel_list_focused = true;
                return;
            }
            if (slot.memory != nullptr && slot.memory->list_widget() != nullptr &&
                slot.memory->list_widget()->is_focused()) {
                detected = Focus::Memory;
                memory_focus_slot_id_ = slot.config.id;
                memory_toolbar_focused_ = false;
                panel_list_focused = true;
                return;
            }
            if (slot.disassembly != nullptr && slot.disassembly->list_widget() != nullptr &&
                slot.disassembly->list_widget()->is_focused()) {
                detected = Focus::Disassembly;
                panel_list_focused = true;
                return;
            }
            if (slot.runtime_source != nullptr && slot.runtime_source->list_widget() != nullptr &&
                slot.runtime_source->list_widget()->is_focused()) {
                detected = Focus::RuntimeSource;
                panel_list_focused = true;
                return;
            }
            if (slot.file_tree != nullptr && slot.file_tree->list_widget() != nullptr &&
                slot.file_tree->list_widget()->is_focused()) {
                detected = Focus::FileTree;
                panel_list_focused = true;
                return;
            }
            if (network_panel_ != nullptr && is_panel_type_active(SidebarPanelType::Network) &&
                network_panel_->list_widget() != nullptr && network_panel_->list_widget()->is_focused()) {
                detected = Focus::Network;
                panel_list_focused = true;
                return;
            }
            if (network_panel_ != nullptr && is_panel_type_active(SidebarPanelType::Network) &&
                network_panel_->is_compose_input_focused()) {
                detected = Focus::Network;
                panel_list_focused = true;
            }
        });
        if (!panel_list_focused) {
            if (repl_panel_ != nullptr && is_panel_type_active(SidebarPanelType::Repl) &&
                repl_panel_->input_widget() != nullptr && repl_panel_->input_widget()->is_focused()) {
                detected = Focus::Repl;
                repl_input_focused_ = repl_panel_->input_active();
            } else if (repl_panel_ != nullptr && is_panel_type_active(SidebarPanelType::Repl) &&
                       repl_panel_->shell_widget() != nullptr && repl_panel_->shell_widget()->is_focused()) {
                detected = Focus::Repl;
                repl_input_focused_ = repl_panel_->input_active();
            } else if (repl_panel_ != nullptr && is_panel_type_active(SidebarPanelType::Repl) &&
                       repl_panel_->history_widget() != nullptr && repl_panel_->history_widget()->is_focused()) {
                detected = Focus::Repl;
                repl_input_focused_ = false;
            } else if (console_panel_ != nullptr && is_panel_type_active(SidebarPanelType::Console) &&
                       console_panel_->is_focused()) {
                detected = Focus::Console;
            } else if (source_panel_ != nullptr && source_panel_->is_focused()) {
                detected = Focus::Source;
            }
        }
    }

    const bool focus_changed = detected != model_.focus;
    if (focus_changed) {
        model_.focus = detected;
        cached_status_bar_text_.clear();
    }

    bool repl_deactivated = false;
    if (repl_panel_ != nullptr && repl_panel_->input_active()) {
        const bool input_focused = repl_panel_->input_widget() != nullptr &&
                                   repl_panel_->input_widget()->is_focused();
        if (detected != Focus::Repl || !input_focused) {
            deactivate_repl_input(false);
            repl_deactivated = true;
        }
    }

    if (!focus_changed && !repl_deactivated) {
        return;
    }

    apply_focus();
    sync_ui_from_model();
}

void DebugApp::mark_all_panels_dirty() {
    if (source_panel_ != nullptr) {
        source_panel_->mark_dirty();
    }
    if (source_scroll_view_ != nullptr) {
        source_scroll_view_->mark_dirty();
    }
    for_each_sidebar_slot([&](const SidebarSlot& slot) {
        if (slot.scopes != nullptr && slot.scopes->list_widget() != nullptr) {
            slot.scopes->list_widget()->mark_dirty();
        }
        if (slot.watches != nullptr && slot.watches->list_widget() != nullptr) {
            slot.watches->list_widget()->mark_dirty();
        }
        if (slot.stacks != nullptr && slot.stacks->list_widget() != nullptr) {
            slot.stacks->list_widget()->mark_dirty();
        }
        if (slot.breakpoints != nullptr && slot.breakpoints->list_widget() != nullptr) {
            slot.breakpoints->list_widget()->mark_dirty();
        }
        if (slot.memory != nullptr && slot.memory->list_widget() != nullptr) {
            slot.memory->list_widget()->mark_dirty();
        }
        if (slot.disassembly != nullptr && slot.disassembly->list_widget() != nullptr) {
            slot.disassembly->list_widget()->mark_dirty();
        }
        if (slot.runtime_source != nullptr && slot.runtime_source->list_widget() != nullptr) {
            slot.runtime_source->list_widget()->mark_dirty();
        }
    });
    if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr) {
        stacks_panel_->list_widget()->mark_dirty();
    }
    if (breakpoints_panel_ != nullptr && breakpoints_panel_->list_widget() != nullptr) {
        breakpoints_panel_->list_widget()->mark_dirty();
    }
    if (repl_panel_ != nullptr) {
        if (repl_panel_->history_widget() != nullptr) {
            repl_panel_->history_widget()->mark_dirty();
        }
        if (repl_panel_->input_widget() != nullptr) {
            repl_panel_->input_widget()->mark_dirty();
        }
    }
    if (console_panel_ != nullptr) {
        console_panel_->mark_dirty();
    }
    if (controls_bar_ != nullptr) {
        controls_bar_->mark_dirty();
    }
    if (status_bar_ != nullptr) {
        status_bar_->mark_dirty();
    }
    refresh_scroll_views(false);
}

int DebugApp::find_source_file_tab_index(const std::string& cache_key) const {
    for (std::size_t i = 0; i < source_file_tabs_.size(); ++i) {
        if (source_file_tabs_[i].cache_key == cache_key) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void DebugApp::save_active_source_file_tab() {
    if (active_source_file_tab_ < 0 || active_source_file_tab_ >= static_cast<int>(source_file_tabs_.size())) {
        return;
    }

    SourceFileTab& tab = source_file_tabs_[static_cast<std::size_t>(active_source_file_tab_)];
    tab.path = model_.source_path;
    tab.source_reference = model_.source_reference;
    tab.cache_key = cached_source_path_;
    tab.cached_text = cached_source_text_;
    if (source_panel_ != nullptr) {
        tab.cursor_line = source_panel_->cursor_line();
    }
    if (source_scroll_view_ != nullptr) {
        tab.scroll_y = source_scroll_view_->scroll_y();
    }
}

void DebugApp::sync_source_file_tab_bar() {
    if (source_tab_bar_ == nullptr) {
        return;
    }

    std::vector<SourceTabBar::Tab> tabs;
    tabs.reserve(source_file_tabs_.size());
    for (const SourceFileTab& tab : source_file_tabs_) {
        tabs.push_back({panel_title_from_path(tab.path)});
    }
    const int active = source_file_tabs_.empty() ? 0 : std::clamp(active_source_file_tab_, 0,
                                                                   static_cast<int>(source_file_tabs_.size()) - 1);
    source_tab_bar_->set_tabs(std::move(tabs), active);
}

void DebugApp::activate_source_file_tab(int index, int line) {
    if (index < 0 || index >= static_cast<int>(source_file_tabs_.size())) {
        return;
    }

    if (active_source_file_tab_ >= 0 && active_source_file_tab_ < static_cast<int>(source_file_tabs_.size()) &&
        active_source_file_tab_ != index) {
        save_active_source_file_tab();
    }

    active_source_file_tab_ = index;
    const SourceFileTab& tab = source_file_tabs_[static_cast<std::size_t>(index)];

    model_.source_path = tab.path;
    model_.source_reference = tab.source_reference;
    cached_source_path_ = tab.cache_key;
    cached_source_reference_ = tab.source_reference;
    cached_source_text_ = tab.cached_text;

    cached_highlight_first_line_ = -1;
    cached_highlight_line_count_ = -1;
    highlight_request_first_line_ = -1;
    highlight_request_line_count_ = -1;
    cached_highlight_scroll_y_ = -1;

    if (source_panel_ != nullptr) {
        source_panel_->set_file_line_count(std::max(1, count_file_lines(cached_source_text_)));
        source_panel_->set_lines({});
        const int target_line = line > 0 ? line : tab.cursor_line;
        source_panel_->set_cursor_line(std::max(1, target_line));
    }

    if (cached_source_text_.empty() && tab.source_reference > 0 && session_io_ != nullptr &&
        session_io_->is_active()) {
        pending_source_fetch_key_ = tab.cache_key;
        session_io_->request_source_fetch(tab.source_reference, tab.cache_key);
    }

    sync_source_stack_title();
    sync_breakpoints_to_panel();

    if (!cached_source_text_.empty() && source_panel_ != nullptr) {
        if (uses_full_file_source()) {
            ensure_source_plain_lines();
        } else {
            const int line_count = std::max(1, source_viewport_height());
            const int first_line =
                std::max(1, (line > 0 ? line : tab.cursor_line) - line_count / 2);
            apply_instant_source_viewport(first_line, line_count);
        }
    }

    if (source_scroll_view_ != nullptr) {
        const int scroll_y = line > 0 ? std::max(0, line - source_viewport_height() / 2 - 1) : tab.scroll_y;
        source_scroll_view_->scroll_to(0, scroll_y);
        source_scroll_view_->refresh_content();
        cached_highlight_scroll_y_ = scroll_y;
        if (source_scroll_view_->bounds().height > 0) {
            cached_source_viewport_height_ = source_scroll_view_->bounds().height;
        }
    } else if (line > 0) {
        scroll_source_to_line(line);
    }

    maybe_request_source_highlight();
    sync_source_file_tab_bar();
    mark_source_view_dirty();
    sync_status_bar();
}

void DebugApp::switch_source_file_tab(int index) {
    if (index == active_source_file_tab_) {
        return;
    }
    activate_source_file_tab(index, 0);
}

void DebugApp::close_source_file_tab(int index) {
    if (index < 0 || index >= static_cast<int>(source_file_tabs_.size())) {
        return;
    }

    source_file_tabs_.erase(source_file_tabs_.begin() + index);

    if (source_file_tabs_.empty()) {
        active_source_file_tab_ = -1;
        model_.source_path.clear();
        model_.source_reference = 0;
        cached_source_path_.clear();
        cached_source_reference_ = 0;
        cached_source_text_.clear();
        if (source_panel_ != nullptr) {
            source_panel_->set_lines({});
            source_panel_->set_file_line_count(1);
        }
        sync_source_file_tab_bar();
        mark_source_view_dirty();
        sync_status_bar();
        return;
    }

    int next_active = active_source_file_tab_;
    if (index < active_source_file_tab_) {
        next_active--;
    } else if (index == active_source_file_tab_) {
        next_active = std::min(index, static_cast<int>(source_file_tabs_.size()) - 1);
    }
    activate_source_file_tab(next_active, 0);
}

void DebugApp::open_file_picker() {
    if (file_picker_ == nullptr) {
        return;
    }

    workspace_root_ = workspace_root_for_program(program_path_);
    workspace_files_ = list_source_files(workspace_root_);
    sync_file_tree_slots();
    file_picker_->open(source_context_clip_bounds(), workspace_files_, workspace_root_);
    model_.status_message = "Open file — fuzzy search by path, Enter to open, Esc to cancel";
    sync_status_bar();
    request_repaint();
}

void DebugApp::close_file_picker() {
    if (file_picker_ == nullptr || !file_picker_->is_open()) {
        return;
    }

    file_picker_->close();
    model_.status_message = "File picker closed";
    sync_status_bar();
    request_repaint();
}

void DebugApp::open_source_file(const std::string& path, int line, bool pin, std::int64_t source_reference) {
    std::string disk_path = path;
    if (!disk_path.empty() && disk_path.rfind("dap:source:", 0) != 0) {
        disk_path = resolve_debugger_source_path(disk_path, program_path_);
        if (looks_like_elf_executable(disk_path)) {
            if (const std::string sibling = sibling_source_for_executable(disk_path); !sibling.empty()) {
                disk_path = sibling;
            } else {
                return;
            }
        }
    }

    const std::string cache_key = source_cache_key(disk_path.empty() ? path : disk_path, source_reference);
    if (cache_key.empty()) {
        return;
    }
    if (pin) {
        follow_execution_ = false;
    }

    const std::string display_path =
        (disk_path.empty() && source_reference > 0) ? cache_key : (disk_path.empty() ? path : disk_path);
    const std::string normalized_display_path =
        display_path.rfind("dap:source:", 0) == 0 ? display_path : source_cache_key(display_path, 0);

    const int existing_index = find_source_file_tab_index(cache_key);
    if (existing_index >= 0) {
        if (existing_index == active_source_file_tab_) {
            const bool same_view = source_panel_ != nullptr && line > 0 && source_panel_->cursor_line() == line &&
                                   !source_panel_->lines().empty();
            if (same_view) {
                if (is_session_stopped() && model_.execution_line > 0) {
                    source_panel_->set_execution_line(static_cast<int>(model_.execution_line));
                }
                return;
            }
            if (line > 0) {
                source_panel_->set_cursor_line(line);
                scroll_source_to_line(line);
                save_active_source_file_tab();
                maybe_request_source_highlight();
                mark_source_view_dirty();
            }
            return;
        }
        activate_source_file_tab(existing_index, line);
        return;
    }

    save_active_source_file_tab();

    SourceFileTab tab;
    tab.path = normalized_display_path;
    tab.source_reference = source_reference;
    tab.cache_key = cache_key;
    tab.cached_text = disk_path.empty() ? std::string{} : read_file_or_empty(disk_path);
    tab.cursor_line = line > 0 ? line : 1;
    tab.scroll_y = 0;
    source_file_tabs_.push_back(std::move(tab));
    activate_source_file_tab(static_cast<int>(source_file_tabs_.size()) - 1, line);
}

namespace {

bool is_runtime_exception_handler_frame(const StackFrameInfo& frame) {
    if (!frame.path.empty() &&
        (frame.path.find(".so") != std::string::npos || frame.path.find("/lib") != std::string::npos)) {
        return true;
    }
    const std::string& name = frame.name;
    return name.find("__cxa_") != std::string::npos || name.find("terminate") != std::string::npos ||
           name.find("__gnu_cxx") != std::string::npos || name.find("__cxxabiv1") != std::string::npos;
}

const StackFrameInfo* first_user_stack_frame(const std::vector<StackFrameInfo>& frames) {
    for (const StackFrameInfo& frame : frames) {
        if (frame.line <= 0) {
            continue;
        }
        if (!frame.path.empty()) {
            if (frame.path.find(".so") != std::string::npos || frame.path.find("/lib") != std::string::npos) {
                continue;
            }
            return &frame;
        }
        if (frame.source_reference > 0) {
            return &frame;
        }
    }
    return nullptr;
}

std::vector<const StackFrameInfo*> user_stack_frames(const std::vector<StackFrameInfo>& frames) {
    std::vector<const StackFrameInfo*> user;
    for (const StackFrameInfo& frame : frames) {
        if (frame.line <= 0) {
            continue;
        }
        if (!frame.path.empty()) {
            if (frame.path.find(".so") != std::string::npos || frame.path.find("/lib") != std::string::npos) {
                continue;
            }
            user.push_back(&frame);
            continue;
        }
        if (frame.source_reference > 0) {
            user.push_back(&frame);
        }
    }
    return user;
}

const StackFrameInfo* catch_search_anchor_frame(const std::vector<StackFrameInfo>& frames) {
    const std::vector<const StackFrameInfo*> user = user_stack_frames(frames);
    if (user.size() >= 2) {
        return user[1];
    }
    return user.empty() ? nullptr : user.front();
}

std::string trim_ascii(std::string value) {
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0) {
        value.pop_back();
    }
    return value;
}

std::optional<int> find_user_catch_body_line(const std::string& source_text, int search_from_line) {
    if (search_from_line <= 0 || source_text.empty()) {
        return std::nullopt;
    }

    std::vector<std::string> lines;
    std::istringstream stream(source_text);
    std::string line;
    while (std::getline(stream, line)) {
        lines.push_back(line);
    }

    for (int index = search_from_line - 1; index < static_cast<int>(lines.size()); ++index) {
        const std::string trimmed = trim_ascii(lines[static_cast<std::size_t>(index)]);
        const bool is_catch = trimmed.find("} catch") != std::string::npos ||
                              trimmed.rfind("catch", 0) == 0 || trimmed.find(" catch (") != std::string::npos;
        if (!is_catch) {
            continue;
        }

        int body_index = index;
        if (lines[static_cast<std::size_t>(index)].find('{') != std::string::npos) {
            body_index = index + 1;
        } else {
            for (int probe = index + 1; probe < static_cast<int>(lines.size()) && probe < index + 4; ++probe) {
                if (lines[static_cast<std::size_t>(probe)].find('{') != std::string::npos) {
                    body_index = probe + 1;
                    break;
                }
            }
        }

        while (body_index < static_cast<int>(lines.size()) &&
               trim_ascii(lines[static_cast<std::size_t>(body_index)]).empty()) {
            ++body_index;
        }
        if (body_index < static_cast<int>(lines.size())) {
            return body_index + 1;
        }
    }

    return std::nullopt;
}

}  // namespace

bool DebugApp::is_catch_phase_exception_stop() const {
    if (model_.stop_reason != "exception" || model_.stack_frames.empty()) {
        return false;
    }
    const std::string& name = model_.stack_frames.front().name;
    return name.find("begin_catch") != std::string::npos;
}

bool DebugApp::try_skip_exception_runtime_to_catch(const char* op) {
    if (op == nullptr || !is_catch_phase_exception_stop() || ephemeral_catch_skip_.has_value()) {
        return false;
    }

    const bool continue_like =
        std::strcmp(op, "continue") == 0 || std::strcmp(op, "step_over") == 0 || std::strcmp(op, "next") == 0 ||
        (std::strcmp(op, "play_pause") == 0 && is_session_stopped());
    if (!continue_like) {
        return false;
    }

    const StackFrameInfo* anchor = catch_search_anchor_frame(model_.stack_frames);
    if (anchor == nullptr) {
        return false;
    }

    std::string path = anchor->path;
    if (path.empty()) {
        path = effective_source_path();
    }
    path = resolve_debugger_source_path(path, program_path_);
    if (path.empty()) {
        return false;
    }

    const std::optional<int> catch_line =
        find_user_catch_body_line(read_file_or_empty(path), static_cast<int>(anchor->line));
    if (!catch_line.has_value()) {
        return false;
    }

    const std::string normalized = normalize_source_path(path);
    EphemeralCatchSkipBreakpoint pending{};
    pending.path = normalized;
    pending.line = *catch_line;
    if (!breakpoints_by_path_[normalized].contains(*catch_line)) {
        breakpoints_by_path_[normalized][*catch_line] = BreakpointInfo{.line = *catch_line};
        pending.added = true;
        push_breakpoints_to_session(normalized);
        sync_breakpoints_to_panel();
    }
    ephemeral_catch_skip_ = std::move(pending);

    send_command_direct("continue");
    model_.status_message = "Skipping runtime unwind to catch block at line " + std::to_string(*catch_line);
    sync_status_bar();
    return true;
}

void DebugApp::maybe_finish_ephemeral_catch_skip() {
    if (!ephemeral_catch_skip_.has_value() || !is_session_stopped()) {
        return;
    }

    const EphemeralCatchSkipBreakpoint pending = *ephemeral_catch_skip_;
    std::string execution_path = model_.execution_path;
    if (execution_path.rfind("dap:source:", 0) == 0) {
        execution_path = effective_source_path();
    }
    execution_path = normalize_source_path(resolve_debugger_source_path(execution_path, program_path_));

    const bool at_catch_site = execution_path == pending.path &&
                               static_cast<int>(model_.execution_line) == pending.line;
    const bool breakpoint_hit = model_.stop_reason.find("breakpoint") != std::string::npos;
    if (!at_catch_site && !breakpoint_hit) {
        return;
    }

    if (pending.added) {
        remove_breakpoint_at(pending.path, pending.line);
    }
    ephemeral_catch_skip_.reset();

    open_source_file(pending.path, pending.line, false);
    model_.focus = Focus::Source;
    apply_focus();
    model_.status_message =
        "In catch handler at " + panel_title_from_path(pending.path) + ":" + std::to_string(pending.line);
    sync_status_bar();
}

bool DebugApp::both_cxx_exception_filters_enabled() const {
    bool throw_enabled = false;
    bool catch_enabled = false;
    for (const auto& [filter, entry] : exception_breakpoints_) {
        if (!entry.enabled) {
            continue;
        }
        const std::string lower = filter;
        if (lower.find("throw") != std::string::npos) {
            throw_enabled = true;
        }
        if (lower.find("catch") != std::string::npos) {
            catch_enabled = true;
        }
    }
    return throw_enabled && catch_enabled;
}

void DebugApp::navigate_to_user_stop_frame() {
    const StackFrameInfo* user_frame = first_user_stack_frame(model_.stack_frames);
    if (user_frame == nullptr) {
        return;
    }

    std::string user_path = user_frame->path;
    if (!user_path.empty()) {
        user_path = resolve_debugger_source_path(user_path, program_path_);
        model_.execution_path = user_path;
    } else if (user_frame->source_reference > 0) {
        model_.execution_path = "dap:source:" + std::to_string(user_frame->source_reference);
    }
    model_.execution_source_reference = user_frame->source_reference;
    model_.execution_line = static_cast<std::uint32_t>(user_frame->line);

    const bool runtime_handler_stop =
        !model_.stack_frames.empty() && is_runtime_exception_handler_frame(model_.stack_frames.front());
    const std::string throw_site =
        panel_title_from_path(user_path.empty() ? user_frame->path : user_path) + ":" +
        std::to_string(user_frame->line);

    model_.focus = Focus::Stacks;
    apply_focus();

    if (runtime_handler_stop) {
        const std::string& top_name = model_.stack_frames.front().name;
        model_.status_message = "Stopped in " + top_name + " (runtime) — throw at " + throw_site;
        model_.status_message += " · Continue/Next jumps to catch block";
        sync_status_bar();
        return;
    }

    std::string open_path = user_path;
    if (open_path.rfind("dap:source:", 0) == 0) {
        open_path.clear();
    }
    open_source_file(open_path, std::max(1, static_cast<int>(user_frame->line)), false,
                      user_frame->source_reference);
    model_.status_message = "Stopped on C++ throw at " + user_frame->name + " (" + throw_site + ")";
    if (both_cxx_exception_filters_enabled()) {
        model_.status_message += " · Continue may stop again on Catch";
    }
    sync_status_bar();
}

void DebugApp::maybe_follow_execution() {
    if (!follow_execution_ || model_.execution_path.empty()) {
        return;
    }

    const int line = model_.execution_line > 0 ? static_cast<int>(model_.execution_line) : 1;
    std::string path = model_.execution_path;
    if (path.rfind("dap:source:", 0) == 0) {
        path.clear();
    } else {
        path = resolve_debugger_source_path(path, program_path_);
    }
    open_source_file(path, line, false, model_.execution_source_reference);
}

std::vector<ThreadStackContent> DebugApp::build_thread_stack_contents() const {
    auto to_stack_frame_row = [this](const StackFrameInfo& frame) {
        StackFrameRow row{};
        row.name = frame.name;
        row.line = static_cast<std::uint32_t>(std::max<std::int64_t>(0, frame.line));
        row.path = frame.path;
        row.source_reference = frame.source_reference;
        if (row.path.empty() && frame.source_reference > 0) {
            row.path = "dap:source:" + std::to_string(frame.source_reference);
        } else if (row.path.empty() && !model_.execution_path.empty()) {
            row.path = model_.execution_path;
            row.source_reference = model_.execution_source_reference;
        }
        return row;
    };

    std::unordered_map<std::int64_t, std::vector<StackFrameRow>> stacks_by_thread;
    for (const ThreadStackInfo& stack : model_.thread_stacks) {
        std::vector<StackFrameRow> rows;
        rows.reserve(stack.frames.size());
        for (const StackFrameInfo& frame : stack.frames) {
            rows.push_back(to_stack_frame_row(frame));
        }
        stacks_by_thread[stack.thread_id] = std::move(rows);
    }
    if (stacks_by_thread.empty() && !model_.stack_frames.empty() && model_.stopped_thread_id > 0) {
        std::vector<StackFrameRow> rows;
        rows.reserve(model_.stack_frames.size());
        for (const StackFrameInfo& frame : model_.stack_frames) {
            rows.push_back(to_stack_frame_row(frame));
        }
        stacks_by_thread[model_.stopped_thread_id] = std::move(rows);
    }

    const bool preserve_stopped_thread =
        model_.stopped_thread_id > 0 && (is_session_stopped() || model_.session_state == "running");

    std::vector<ThreadStackContent> thread_contents;
    thread_contents.reserve(model_.threads.size());
    for (const ThreadInfo& thread : model_.threads) {
        ThreadStackContent content{};
        content.id = thread.id;
        content.name = thread.name.empty() ? ("Thread " + std::to_string(thread.id)) : thread.name;
        content.stopped = preserve_stopped_thread && thread.id == model_.stopped_thread_id;
        const auto frames_it = stacks_by_thread.find(thread.id);
        if (frames_it != stacks_by_thread.end()) {
            content.frames = frames_it->second;
        }
        thread_contents.push_back(std::move(content));
    }

    if (thread_contents.empty() && !model_.stack_frames.empty()) {
        ThreadStackContent content{};
        content.id = model_.stopped_thread_id > 0 ? model_.stopped_thread_id : 1;
        content.name = "MainThread";
        content.stopped = preserve_stopped_thread || model_.stopped_thread_id <= 0;
        for (const StackFrameInfo& frame : model_.stack_frames) {
            content.frames.push_back(to_stack_frame_row(frame));
        }
        thread_contents.push_back(std::move(content));
    }

    return thread_contents;
}

void DebugApp::sync_threads_list_panel() {
    std::vector<ThreadStackContent> threads = build_thread_stack_contents();
    const bool session_ended =
        model_.session_state == "exited" || model_.session_state == "disconnected";
    if (!threads.empty()) {
        cached_thread_stack_contents_ = threads;
    } else if (session_ended && !cached_thread_stack_contents_.empty()) {
        threads = cached_thread_stack_contents_;
    }
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type == SidebarPanelType::Threads) {
            sync_thread_slot(slot, threads);
        }
    });
}

std::vector<BreakpointRow> DebugApp::build_breakpoint_rows() const {
    std::vector<BreakpointRow> rows;
    for (const DataBreakpointEntry& entry : data_breakpoints_) {
        BreakpointRow row{};
        row.kind = BreakpointRowKind::Data;
        if (!entry.variable_name.empty()) {
            row.source_text = entry.variable_name;
        } else if (!entry.description.empty()) {
            row.source_text = entry.description;
        } else {
            row.source_text = entry.data_id;
        }
        row.data_id = entry.data_id;
        row.access_type = entry.access_type;
        rows.push_back(std::move(row));
    }
    for (const FunctionBreakpointEntry& entry : function_breakpoints_) {
        if (entry.name.empty()) {
            continue;
        }
        BreakpointRow row{};
        row.kind = BreakpointRowKind::Function;
        row.source_text = entry.name;
        rows.push_back(std::move(row));
    }
    for (const auto& [path, breakpoints] : breakpoints_by_path_) {
        const std::string normalized = normalize_source_path(path);
        const std::string file_text = read_file_or_empty(normalized);
        for (const auto& [bp_line, info] : breakpoints) {
            BreakpointRow row{};
            row.kind = BreakpointRowKind::Source;
            row.path = normalized;
            row.line = bp_line;
            row.source_text = trimmed_line_text_at(file_text, bp_line);
            row.condition = info.condition;
            row.hit_condition = info.hit_condition;
            row.hit_count = info.hit_count;
            rows.push_back(std::move(row));
        }
    }
    const std::string exception_source_path =
        !effective_source_path().empty() ? effective_source_path() : program_path_;
    for (const DebugUiModel::ExceptionBreakpointFilterInfo& filter : model_.exception_breakpoint_filters) {
        if (filter.filter.empty() || !exception_filter_relevant_for_source(filter, exception_source_path)) {
            continue;
        }
        BreakpointRow row{};
        row.kind = BreakpointRowKind::Exception;
        row.data_id = filter.filter;
        row.source_text = filter.label.empty() ? filter.filter : filter.label;
        const auto exception_it = exception_breakpoints_.find(filter.filter);
        row.exception_enabled =
            exception_it != exception_breakpoints_.end() && exception_it->second.enabled;
        row.exception_supports_condition = filter.supports_condition;
        if (exception_it != exception_breakpoints_.end()) {
            row.condition = exception_it->second.condition;
        }
        rows.push_back(std::move(row));
    }

    return rows;
}

void DebugApp::sync_breakpoints_list_panel() {
    const std::vector<BreakpointRow> rows = build_breakpoint_rows();
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type == SidebarPanelType::Breakpoints) {
            sync_breakpoint_slot(slot, rows);
        }
    });
}

void DebugApp::toggle_breakpoint() {
    if (source_panel_ == nullptr) {
        return;
    }
    toggle_breakpoint_at_line(source_panel_->cursor_line());
}

std::string DebugApp::normalize_source_path(const std::string& path) const {
    if (path.empty() || path.rfind("dap:source:", 0) == 0) {
        return path;
    }

    try {
        std::filesystem::path resolved(path);
        if (resolved.is_relative()) {
            resolved = std::filesystem::absolute(resolved);
        }
        return std::filesystem::weakly_canonical(resolved).string();
    } catch (...) {
        return path;
    }
}

void DebugApp::normalize_breakpoint_path_keys() {
    BreakpointsByPath normalized;
    for (auto& [path, breakpoints] : breakpoints_by_path_) {
        auto& bucket = normalized[normalize_source_path(path)];
        for (auto& [line, info] : breakpoints) {
            bucket[line] = info;
        }
    }
    breakpoints_by_path_ = std::move(normalized);
}

std::string DebugApp::effective_source_path() const {
    if (!model_.source_path.empty() && model_.source_path.rfind("dap:source:", 0) != 0) {
        return normalize_source_path(model_.source_path);
    }
    return normalize_source_path(program_path_);
}

void DebugApp::flush_breakpoints_to_session() {
    if (session_io_ == nullptr || !session_io_->is_active() || model_.session_state == "disconnected") {
        return;
    }

    std::unordered_set<std::string> pushed;
    for (const auto& [path, breakpoints] : breakpoints_by_path_) {
        (void)breakpoints;
        const std::string normalized = normalize_source_path(path);
        if (pushed.contains(normalized)) {
            continue;
        }
        pushed.insert(normalized);
        push_breakpoints_to_session(normalized);
    }
    flush_data_breakpoints_to_session();
    flush_function_breakpoints_to_session();
    flush_exception_breakpoints_to_session();
}

void DebugApp::apply_exception_filter_defaults() {
    if (exception_defaults_applied_ || model_.exception_breakpoint_filters.empty()) {
        return;
    }

    for (const DebugUiModel::ExceptionBreakpointFilterInfo& filter : model_.exception_breakpoint_filters) {
        if (filter.default_enabled) {
            exception_breakpoints_[filter.filter].enabled = true;
        }
    }
    exception_defaults_applied_ = true;
    if (session_io_ != nullptr && session_io_->is_active()) {
        bool any_enabled = false;
        for (const auto& [filter, entry] : exception_breakpoints_) {
            if (!filter.empty() && entry.enabled) {
                any_enabled = true;
                break;
            }
        }
        if (any_enabled) {
            push_exception_breakpoints_to_session();
        }
    }
}

std::string DebugApp::build_exception_breakpoints_json() const {
    std::string json = "[";
    bool first = true;
    for (const auto& [filter, entry] : exception_breakpoints_) {
        if (filter.empty() || !entry.enabled) {
            continue;
        }
        if (!first) {
            json += ',';
        }
        first = false;
        json += "{\"filter\":\"" + escape_json_string(filter) + "\"";
        if (!entry.condition.empty()) {
            json += ",\"condition\":\"" + escape_json_string(entry.condition) + "\"";
        }
        json += '}';
    }
    json += ']';
    return json;
}

void DebugApp::push_exception_breakpoints_to_session() {
    if (session_io_ == nullptr || !session_io_->is_active() || model_.exception_breakpoint_filters.empty()) {
        return;
    }
    session_io_->post_set_exception_breakpoints(build_exception_breakpoints_json());
}

void DebugApp::flush_exception_breakpoints_to_session() { push_exception_breakpoints_to_session(); }

void DebugApp::toggle_exception_breakpoint(const std::string& filter) {
    if (filter.empty() || model_.exception_breakpoint_filters.empty()) {
        return;
    }

    ExceptionBreakpointEntry& entry = exception_breakpoints_[filter];
    if (entry.enabled) {
        entry.enabled = false;
        model_.status_message = "Disabled exception breakpoint";
    } else {
        entry.enabled = true;
        model_.status_message = "Enabled exception breakpoint";
    }

    push_exception_breakpoints_to_session();
    sync_breakpoints_list_panel();
    sync_status_bar();
}

void DebugApp::begin_edit_exception_condition(const std::string& filter) {
    if (filter.empty()) {
        return;
    }

    ExceptionBreakpointEntry& entry = exception_breakpoints_[filter];
    if (!entry.enabled) {
        entry.enabled = true;
        push_exception_breakpoints_to_session();
    }

    editing_exception_filter_ = filter;
    editing_breakpoint_path_.clear();
    editing_breakpoint_line_ = 0;
    editing_breakpoint_hit_ = false;
    breakpoint_input_draft_ = entry.condition;
    breakpoint_input_focused_ = true;
    model_.focus = Focus::Breakpoints;

    if (breakpoints_panel_ != nullptr) {
        breakpoints_panel_->set_exception_inline_edit(filter, breakpoint_input_draft_);
        sync_breakpoints_list_panel();
        breakpoints_panel_->focus_inline_edit();
    }
    apply_focus();

    model_.status_message = "Exception condition for " + filter + " — Enter to save, Esc to cancel";
    sync_status_bar();
    request_repaint();
}

void DebugApp::set_exception_breakpoint_condition(const std::string& filter, const std::string& condition) {
    if (filter.empty()) {
        return;
    }

    ExceptionBreakpointEntry& entry = exception_breakpoints_[filter];
    entry.condition = condition;
    if (!entry.enabled && !condition.empty()) {
        entry.enabled = true;
    }

    push_exception_breakpoints_to_session();
    sync_breakpoints_list_panel();
    model_.status_message = condition.empty() ? "Cleared exception condition" : "Updated exception condition";
    sync_status_bar();
}

std::string DebugApp::build_data_breakpoints_json() const {
    std::string json = "[";
    bool first = true;
    for (const DataBreakpointEntry& entry : data_breakpoints_) {
        if (entry.data_id.empty()) {
            continue;
        }
        if (!first) {
            json += ',';
        }
        json += "{\"dataId\":\"" + escape_json_string(entry.data_id) + "\"";
        if (!entry.description.empty()) {
            json += ",\"description\":\"" + escape_json_string(entry.description) + "\"";
        }
        if (!entry.access_type.empty()) {
            json += ",\"accessType\":\"" + escape_json_string(entry.access_type) + "\"";
        }
        if (!entry.condition.empty()) {
            json += ",\"condition\":\"" + escape_json_string(entry.condition) + "\"";
        }
        json += '}';
        first = false;
    }
    json += ']';
    return json;
}

void DebugApp::push_data_breakpoints_to_session() {
    if (session_io_ == nullptr || !session_io_->is_active() || model_.session_state == "disconnected") {
        return;
    }
    if (!model_.supports_data_breakpoints) {
        return;
    }
    session_io_->post_set_data_breakpoints(build_data_breakpoints_json());
}

void DebugApp::flush_data_breakpoints_to_session() { push_data_breakpoints_to_session(); }

std::string DebugApp::build_function_breakpoints_json() const {
    std::string json = "[";
    bool first = true;
    for (const FunctionBreakpointEntry& entry : function_breakpoints_) {
        if (entry.name.empty()) {
            continue;
        }
        if (!first) {
            json += ',';
        }
        json += "{\"name\":\"" + escape_json_string(entry.name) + "\"";
        if (!entry.condition.empty()) {
            json += ",\"condition\":\"" + escape_json_string(entry.condition) + "\"";
        }
        if (!entry.hit_condition.empty()) {
            json += ",\"hitCondition\":\"" + escape_json_string(entry.hit_condition) + "\"";
        }
        json += '}';
        first = false;
    }
    json += ']';
    return json;
}

void DebugApp::push_function_breakpoints_to_session() {
    if (session_io_ == nullptr || !session_io_->is_active() || model_.session_state == "disconnected") {
        return;
    }
    if (!adapter_supports_function_breakpoints()) {
        return;
    }
    session_io_->post_set_function_breakpoints(build_function_breakpoints_json());
}

void DebugApp::flush_function_breakpoints_to_session() { push_function_breakpoints_to_session(); }

bool DebugApp::adapter_supports_function_breakpoints() const {
    return model_.supports_function_breakpoints || launch_ui_.assume_function_breakpoints;
}

bool DebugApp::has_function_breakpoint(const std::string& name) const {
    return std::any_of(function_breakpoints_.begin(), function_breakpoints_.end(),
                       [&](const FunctionBreakpointEntry& entry) { return entry.name == name; });
}

std::optional<std::string> DebugApp::function_name_at_breakpoint_line(const std::string& path,
                                                                      const std::string& source_text,
                                                                      int line) const {
    if (!adapter_supports_function_breakpoints() || line <= 0) {
        return std::nullopt;
    }

    return function_name_at_line_from_treesitter(language_from_path(path), source_text, line);
}

void DebugApp::add_function_breakpoint(const std::string& name, const std::string& path, int line) {
    if (!adapter_supports_function_breakpoints()) {
        model_.status_message = "Function breakpoints are not supported by this adapter";
        sync_status_bar();
        return;
    }
    if (name.empty()) {
        return;
    }

    auto existing = std::find_if(function_breakpoints_.begin(), function_breakpoints_.end(),
                                 [&](const FunctionBreakpointEntry& entry) { return entry.name == name; });
    if (existing == function_breakpoints_.end()) {
        FunctionBreakpointEntry entry{};
        entry.name = name;
        entry.path = path;
        entry.line = line;
        function_breakpoints_.push_back(std::move(entry));
    } else if (!path.empty() && line > 0) {
        existing->path = path;
        existing->line = line;
    }

    model_.status_message = "Function breakpoint on " + name;
    sync_breakpoints_list_panel();
    sync_breakpoints_to_panel();
    push_function_breakpoints_to_session();
    sync_status_bar();
    mark_all_panels_dirty();
}

void DebugApp::toggle_function_breakpoint(const std::string& name, const std::string& path, int line) {
    if (has_function_breakpoint(name)) {
        remove_function_breakpoint(name);
        return;
    }
    add_function_breakpoint(name, path, line);
}

void DebugApp::remove_function_breakpoint(const std::string& name) {
    if (name.empty()) {
        return;
    }

    const auto it = std::remove_if(function_breakpoints_.begin(), function_breakpoints_.end(),
                                   [&](const FunctionBreakpointEntry& entry) { return entry.name == name; });
    if (it == function_breakpoints_.end()) {
        return;
    }
    function_breakpoints_.erase(it, function_breakpoints_.end());
    model_.status_message = "Removed function breakpoint on " + name;
    sync_breakpoints_list_panel();
    sync_breakpoints_to_panel();
    push_function_breakpoints_to_session();
    sync_status_bar();
    mark_all_panels_dirty();
}

int DebugApp::find_function_definition_line(const std::string& source_text, const std::string& name) const {
    if (name.empty()) {
        return 0;
    }

    const std::string path = effective_source_path();
    const std::string language = language_from_path(path.empty() ? program_path_ : path);
    if (const std::optional<int> treesitter_line =
            function_definition_line_from_treesitter(language, source_text, name);
        treesitter_line.has_value()) {
        return *treesitter_line;
    }
    return 0;
}

void DebugApp::show_stack_frame_context_menu(const StackFrameRow& frame, tuinator::Point anchor) {
    if (context_menu_ == nullptr || frame.name.empty()) {
        return;
    }
    if (!adapter_supports_function_breakpoints()) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    const bool already_set = has_function_breakpoint(frame.name);
    items.push_back(ContextMenu::Item{
        already_set ? "Remove function breakpoint" : "Add function breakpoint",
        [this, name = frame.name, already_set]() {
            if (already_set) {
                remove_function_breakpoint(name);
            } else {
                add_function_breakpoint(name);
            }
        },
    });
    if (items.empty()) {
        return;
    }

    model_.focus = Focus::Stacks;
    apply_focus();
    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::request_data_breakpoint(const std::string& variable_name, std::int64_t container_reference,
                                       const std::string& access_type) {
    if (!model_.supports_data_breakpoints) {
        model_.status_message = "Data breakpoints not supported by this adapter";
        sync_status_bar();
        return;
    }
    if (!is_session_stopped()) {
        model_.status_message = "Set data breakpoints while stopped";
        sync_status_bar();
        return;
    }
    if (session_io_ == nullptr || variable_name.empty() || container_reference <= 0) {
        return;
    }

    pending_data_breakpoint_ = PendingDataBreakpointRequest{variable_name, access_type};
    session_io_->request_data_breakpoint_info(container_reference, current_frame_id(), variable_name, access_type);
    model_.status_message = "Resolving data breakpoint for " + variable_name + "…";
    sync_status_bar();
}

void DebugApp::remove_data_breakpoint(const std::string& data_id) {
    if (data_id.empty()) {
        return;
    }

    const auto it = std::remove_if(data_breakpoints_.begin(), data_breakpoints_.end(),
                                   [&](const DataBreakpointEntry& entry) { return entry.data_id == data_id; });
    if (it == data_breakpoints_.end()) {
        return;
    }
    data_breakpoints_.erase(it, data_breakpoints_.end());
    model_.status_message = "Removed data breakpoint";
    sync_breakpoints_list_panel();
    push_data_breakpoints_to_session();
    sync_status_bar();
    mark_all_panels_dirty();
}

namespace {

std::optional<std::string> extract_json_string_field(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\":\"";
    const std::size_t start = json.find(needle);
    if (start == std::string::npos) {
        return std::nullopt;
    }
    std::size_t index = start + needle.size();
    std::string value;
    while (index < json.size()) {
        const char ch = json[index++];
        if (ch == '\\' && index < json.size()) {
            value.push_back(json[index++]);
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

}  // namespace

void DebugApp::handle_data_breakpoint_info_payload(const SessionIoEvent& event) {
    if (!pending_data_breakpoint_.has_value()) {
        return;
    }

    PendingDataBreakpointRequest pending = *pending_data_breakpoint_;
    pending_data_breakpoint_.reset();

    if (!event.success || event.payload.empty()) {
        model_.status_message =
            event.detail.empty() ? "Failed to resolve data breakpoint" : ("Data breakpoint failed: " + event.detail);
        sync_status_bar();
        return;
    }

    const std::optional<std::string> data_id = extract_json_string_field(event.payload, "dataId");
    if (!data_id.has_value() || data_id->empty()) {
        model_.status_message = "Adapter returned no data breakpoint id";
        sync_status_bar();
        return;
    }

    const std::optional<std::string> description = extract_json_string_field(event.payload, "description");
    DataBreakpointEntry entry{};
    entry.data_id = *data_id;
    entry.variable_name = pending.variable_name;
    entry.description = description.has_value() && !description->empty() ? *description : pending.variable_name;
    entry.access_type = event.detail.empty() ? pending.access_type : event.detail;

    const std::string status_label = entry.variable_name.empty() ? entry.description : entry.variable_name;
    const std::string status_access = entry.access_type;
    auto existing = std::find_if(data_breakpoints_.begin(), data_breakpoints_.end(),
                                 [&](const DataBreakpointEntry& candidate) { return candidate.data_id == entry.data_id; });
    if (existing != data_breakpoints_.end()) {
        *existing = std::move(entry);
    } else {
        data_breakpoints_.push_back(std::move(entry));
    }

    model_.status_message = "Data breakpoint on " + status_label + " (" + status_access + ")";
    sync_breakpoints_list_panel();
    push_data_breakpoints_to_session();
    sync_status_bar();
    mark_all_panels_dirty();
}

void DebugApp::show_scope_variable_context_menu(int row_index, tuinator::Point anchor) {
    SidebarSlot* slot = active_sidebar_slot();
    if (context_menu_ == nullptr || slot == nullptr || slot->config.type != SidebarPanelType::Variables ||
        row_index < 0 || row_index >= static_cast<int>(slot->cached_scope_row_meta.size())) {
        return;
    }

    const ScopeVariableRowMeta& row_meta = slot->cached_scope_row_meta[static_cast<std::size_t>(row_index)];
    if (row_meta.variable_name.empty() || row_meta.container_reference <= 0) {
        return;
    }

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        "Add watch",
        [this, name = row_meta.variable_name]() { add_watch(name); },
    });

    if (model_.supports_data_breakpoints && is_session_stopped()) {
        items.push_back(ContextMenu::Item{
            "Break on write",
            [this, name = row_meta.variable_name, container = row_meta.container_reference]() {
                request_data_breakpoint(name, container, "write");
            },
        });
        items.push_back(ContextMenu::Item{
            "Break on read",
            [this, name = row_meta.variable_name, container = row_meta.container_reference]() {
                request_data_breakpoint(name, container, "read");
            },
        });
        items.push_back(ContextMenu::Item{
            "Break on read/write",
            [this, name = row_meta.variable_name, container = row_meta.container_reference]() {
                request_data_breakpoint(name, container, "readWrite");
            },
        });
    }

    if (items.empty()) {
        return;
    }

    model_.focus = Focus::Scopes;
    apply_focus();
    context_menu_->open(anchor, overlay_clip_bounds(), std::move(items));
    context_menu_->layout(overlay_clip_bounds());
    request_full_screen_refresh();
}

void DebugApp::sync_breakpoints_to_panel() {
    if (source_panel_ == nullptr) {
        return;
    }

    const std::string path = effective_source_path();
    if (path.empty()) {
        return;
    }

    const std::string normalized = normalize_source_path(path);
    std::unordered_map<int, bool> breakpoints;
    const auto it = breakpoints_by_path_.find(path);
    if (it == breakpoints_by_path_.end()) {
        const auto normalized_it = breakpoints_by_path_.find(normalized);
        if (normalized_it != breakpoints_by_path_.end()) {
            breakpoints = source_breakpoints_from_info(normalized_it->second);
        }
    } else {
        breakpoints = source_breakpoints_from_info(it->second);
    }

    const std::string file_text =
        cached_source_path_ == path || cached_source_path_ == normalized ? cached_source_text_
                                                                       : read_file_or_empty(normalized);
    std::unordered_set<int> function_lines;
    for (const FunctionBreakpointEntry& entry : function_breakpoints_) {
        if (!entry.path.empty() && entry.line > 0) {
            if (normalize_source_path(entry.path) == normalized) {
                function_lines.insert(entry.line);
            }
            continue;
        }
        if (const int definition_line = find_function_definition_line(file_text, entry.name); definition_line > 0) {
            function_lines.insert(definition_line);
        }
    }

    if (source_panel_->breakpoints() != breakpoints) {
        source_panel_->set_breakpoints(std::move(breakpoints));
    }
    source_panel_->set_function_breakpoint_lines(std::move(function_lines));
}

void DebugApp::push_breakpoints_to_session(const std::string& path) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || session_io_ == nullptr || !session_io_->is_active() ||
        model_.session_state == "disconnected") {
        return;
    }

    const auto it = find_breakpoints_path(normalized);
    std::string json = "[";
    bool first = true;
    if (it != breakpoints_by_path_.end()) {
        std::vector<int> lines;
        lines.reserve(it->second.size());
        for (const auto& [line, _] : it->second) {
            lines.push_back(line);
        }
        std::sort(lines.begin(), lines.end());
        for (int bp_line : lines) {
            const BreakpointInfo& info = it->second.at(bp_line);
            if (!first) {
                json += ',';
            }
            json += "{\"line\":" + std::to_string(bp_line);
            if (!info.condition.empty()) {
                json += ",\"condition\":\"" + escape_json_string(info.condition) + "\"";
            }
            if (!info.hit_condition.empty()) {
                json += ",\"hitCondition\":\"" + escape_json_string(info.hit_condition) + "\"";
            }
            json += '}';
            first = false;
        }
    }
    json += ']';
    session_io_->post_set_breakpoints(it != breakpoints_by_path_.end() ? it->first : normalized, json);
}

void DebugApp::remove_breakpoint_at(const std::string& path, int line) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    auto it = breakpoints_by_path_.find(normalized);
    if (it == breakpoints_by_path_.end() || !it->second.contains(line)) {
        return;
    }

    it->second.erase(line);
    if (it->second.empty()) {
        breakpoints_by_path_.erase(it);
    }
    model_.status_message =
        "Removed breakpoint at " + panel_title_from_path(normalized) + ":" + std::to_string(line);

    if (source_panel_ != nullptr && effective_source_path() == normalized) {
        std::unordered_map<int, bool> breakpoints;
        const auto current = breakpoints_by_path_.find(normalized);
        if (current != breakpoints_by_path_.end()) {
            breakpoints = source_breakpoints_from_info(current->second);
        }
        source_panel_->set_breakpoints(std::move(breakpoints));
    }

    if (session_io_ != nullptr && session_io_->is_active()) {
        push_breakpoints_to_session(normalized);
    }
    sync_breakpoints_to_panel();
    sync_breakpoints_list_panel();
    sync_status_bar();
    mark_all_panels_dirty();
}

void DebugApp::toggle_breakpoint_at(const std::string& path, int line) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    const auto it = breakpoints_by_path_.find(normalized);
    if (it != breakpoints_by_path_.end() && it->second.contains(line)) {
        remove_breakpoint_at(path, line);
        return;
    }

    if (normalized == effective_source_path()) {
        toggle_breakpoint_at_line(line);
        return;
    }

    breakpoints_by_path_[normalized][line] = BreakpointInfo{.line = line};
    model_.status_message =
        "Set breakpoint at " + panel_title_from_path(normalized) + ":" + std::to_string(line);
    if (session_io_ != nullptr && session_io_->is_active()) {
        push_breakpoints_to_session(normalized);
    }
    sync_breakpoints_list_panel();
    sync_status_bar();
    mark_all_panels_dirty();
}

void DebugApp::toggle_breakpoint_at_line(int line) {
    const std::string path = effective_source_path();
    if (source_panel_ == nullptr || path.empty() || line <= 0) {
        return;
    }

    if (cached_source_path_ != path) {
        cached_source_path_ = path;
        cached_source_text_ = read_file_or_empty(cached_source_path_);
        source_panel_->set_file_line_count(std::max(1, count_file_lines(cached_source_text_)));
    }

    source_panel_->set_cursor_line(line);

    if (adapter_supports_function_breakpoints()) {
        if (const std::optional<std::string> function_name =
                function_name_at_breakpoint_line(path, cached_source_text_, line);
            function_name.has_value()) {
            toggle_function_breakpoint(*function_name, normalize_source_path(path), line);
            return;
        }
    }

    auto& breakpoints = breakpoints_by_path_[path];
    if (breakpoints.contains(line)) {
        remove_breakpoint_at(path, line);
        return;
    }

    if (!is_breakpointable_line(source_panel_, cached_source_text_, line)) {
        model_.status_message = "Cannot set breakpoint on an empty line";
        sync_status_bar();
        return;
    }

    breakpoints[line] = BreakpointInfo{.line = line};
    model_.status_message = "Set breakpoint at line " + std::to_string(line);
    source_panel_->set_breakpoints(source_breakpoints_from_info(breakpoints));

    if (source_panel_->lines().empty() && !cached_source_text_.empty()) {
        const int line_count =
            uses_full_file_source() ? source_panel_->file_line_count() : std::max(1, source_viewport_height());
        const int first_line = uses_full_file_source() ? 1 : std::max(1, line - line_count / 2);
        apply_instant_source_viewport(first_line, line_count);
    }

    if (session_io_ != nullptr && session_io_->is_active()) {
        push_breakpoints_to_session(path);
    }
    sync_breakpoints_list_panel();
    sync_status_bar();
    mark_all_panels_dirty();
}

void DebugApp::set_breakpoint_condition(const std::string& path, int line, const std::string& condition) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    auto it = breakpoints_by_path_.find(normalized);
    if (it == breakpoints_by_path_.end() || !it->second.contains(line)) {
        return;
    }

    const std::string trimmed = trim_breakpoint_condition(condition);
    it->second[line].condition = trimmed;
    if (trimmed.empty()) {
        model_.status_message =
            "Cleared condition at " + panel_title_from_path(normalized) + ":" + std::to_string(line);
    } else {
        model_.status_message = "Condition at " + panel_title_from_path(normalized) + ":" +
                                std::to_string(line) + " = " + trimmed;
    }

    sync_breakpoints_to_panel();
    sync_breakpoints_list_panel();
    if (session_io_ != nullptr && session_io_->is_active()) {
        push_breakpoints_to_session(normalized);
    }
    sync_status_bar();
    mark_all_panels_dirty();
}

void DebugApp::set_breakpoint_hit_condition(const std::string& path, int line,
                                            const std::string& hit_condition) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    auto it = breakpoints_by_path_.find(normalized);
    if (it == breakpoints_by_path_.end() || !it->second.contains(line)) {
        return;
    }

    const std::string trimmed = trim_breakpoint_condition(hit_condition);
    it->second[line].hit_condition = trimmed;
    if (trimmed.empty()) {
        it->second[line].hit_count = 0;
        model_.status_message =
            "Cleared hit condition at " + panel_title_from_path(normalized) + ":" + std::to_string(line);
    } else {
        model_.status_message = "Hit condition at " + panel_title_from_path(normalized) + ":" +
                                std::to_string(line) + " = " + trimmed;
    }

    sync_breakpoints_list_panel();
    if (session_io_ != nullptr && session_io_->is_active()) {
        push_breakpoints_to_session(normalized);
    }
    sync_status_bar();
    mark_all_panels_dirty();
}

BreakpointsByPath::iterator DebugApp::find_breakpoints_path(const std::string& path) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty()) {
        return breakpoints_by_path_.end();
    }

    auto exact = breakpoints_by_path_.find(normalized);
    if (exact != breakpoints_by_path_.end()) {
        return exact;
    }

    const std::string target_name = panel_title_from_path(normalized);
    for (auto it = breakpoints_by_path_.begin(); it != breakpoints_by_path_.end(); ++it) {
        if (normalize_source_path(it->first) == normalized || panel_title_from_path(it->first) == target_name) {
            return it;
        }
    }
    return breakpoints_by_path_.end();
}

int DebugApp::find_breakpoint_line_at_stop(const BreakpointsByPath::mapped_type& breakpoints,
                                           int execution_line) const {
    if (execution_line <= 0) {
        return -1;
    }
    if (breakpoints.contains(execution_line)) {
        return execution_line;
    }

    int best_line = -1;
    int best_distance = 4;
    for (const auto& [bp_line, info] : breakpoints) {
        if (!info.hit_condition.empty()) {
            const int distance = std::abs(bp_line - execution_line);
            if (distance < best_distance) {
                best_distance = distance;
                best_line = bp_line;
            }
        }
    }
    if (best_line > 0) {
        return best_line;
    }

    for (const auto& [bp_line, _] : breakpoints) {
        const int distance = std::abs(bp_line - execution_line);
        if (distance < best_distance) {
            best_distance = distance;
            best_line = bp_line;
        }
    }
    return best_line;
}

void DebugApp::apply_breakpoint_hit_entry(const std::string& path, int line, std::uint64_t hit_count) {
    if (path.empty() || line <= 0) {
        return;
    }

    auto path_it = find_breakpoints_path(path);
    if (path_it == breakpoints_by_path_.end()) {
        const std::string resolved = resolve_debugger_source_path(path, program_path_);
        if (!resolved.empty()) {
            path_it = find_breakpoints_path(resolved);
        }
    }
    if (path_it == breakpoints_by_path_.end()) {
        return;
    }

    auto bp_it = path_it->second.find(line);
    if (bp_it == path_it->second.end()) {
        const int matched_line = find_breakpoint_line_at_stop(path_it->second, line);
        if (matched_line <= 0) {
            return;
        }
        bp_it = path_it->second.find(matched_line);
        if (bp_it == path_it->second.end()) {
            return;
        }
    }

    if (!bp_it->second.hit_condition.empty()) {
        bp_it->second.hit_count = hit_count;
    } else {
        bp_it->second.hit_count = std::max(bp_it->second.hit_count, hit_count);
    }
}

void DebugApp::record_breakpoint_hit() {
    if (!is_session_stopped() || model_.execution_line <= 0) {
        return;
    }

    std::string reason = model_.stop_reason;
    std::transform(reason.begin(), reason.end(), reason.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    const bool breakpoint_stop = reason.find("breakpoint") != std::string::npos || reason.find("bkpt") != std::string::npos;

    std::string path = model_.execution_path;
    if (path.rfind("dap:source:", 0) == 0) {
        path = effective_source_path();
    }
    if (path.empty()) {
        return;
    }

    auto path_it = find_breakpoints_path(path);
    if (path_it == breakpoints_by_path_.end()) {
        const std::string resolved = resolve_debugger_source_path(path, program_path_);
        if (resolved.empty()) {
            return;
        }
        path_it = find_breakpoints_path(resolved);
        if (path_it == breakpoints_by_path_.end()) {
            return;
        }
    }

    const int execution_line = static_cast<int>(model_.execution_line);
    const int bp_line = find_breakpoint_line_at_stop(path_it->second, execution_line);
    if (bp_line <= 0) {
        return;
    }

    auto bp_it = path_it->second.find(bp_line);
    if (bp_it == path_it->second.end()) {
        return;
    }
    if (!bp_it->second.hit_condition.empty()) {
        return;
    }
    if (!breakpoint_stop) {
        return;
    }

    const BreakpointStopRecord stop{normalize_source_path(path_it->first), static_cast<std::uint32_t>(bp_line),
                                    model_.stopped_thread_id};
    if (last_counted_breakpoint_stop_ == stop) {
        return;
    }
    last_counted_breakpoint_stop_ = stop;
    bp_it->second.hit_count += 1;
}

void DebugApp::refresh_breakpoint_hit_counts_from_session() {
    if (session_io_ == nullptr || !session_io_->is_active() || breakpoints_by_path_.empty()) {
        return;
    }

    flush_breakpoints_to_session();
}

void DebugApp::apply_breakpoint_hit_counts(const std::string& path, const std::string& results_json) {
    if (results_json.empty()) {
        return;
    }

    auto path_it = find_breakpoints_path(path);
    if (path_it == breakpoints_by_path_.end()) {
        return;
    }

    std::vector<int> request_lines;
    request_lines.reserve(path_it->second.size());
    for (const auto& [line, _] : path_it->second) {
        request_lines.push_back(line);
    }
    std::sort(request_lines.begin(), request_lines.end());

    std::vector<std::uint64_t> adapter_hits;
    std::size_t pos = 0;
    while ((pos = results_json.find("\"hitCount\"", pos)) != std::string::npos) {
        const std::size_t hit_colon = results_json.find(':', pos);
        if (hit_colon == std::string::npos) {
            break;
        }
        std::size_t hit_start = hit_colon + 1;
        while (hit_start < results_json.size() &&
               std::isspace(static_cast<unsigned char>(results_json[hit_start])) != 0) {
            ++hit_start;
        }
        std::size_t hit_end = hit_start;
        while (hit_end < results_json.size() &&
               std::isdigit(static_cast<unsigned char>(results_json[hit_end])) != 0) {
            ++hit_end;
        }
        if (hit_end > hit_start) {
            try {
                adapter_hits.push_back(
                    static_cast<std::uint64_t>(std::stoull(results_json.substr(hit_start, hit_end - hit_start))));
            } catch (...) {
            }
        }
        pos = hit_end;
    }

    for (std::size_t index = 0; index < request_lines.size() && index < adapter_hits.size(); ++index) {
        if (adapter_hits[index] == 0) {
            continue;
        }
        apply_breakpoint_hit_entry(path_it->first, request_lines[index], adapter_hits[index]);
    }

    pos = 0;
    while ((pos = results_json.find("\"line\"", pos)) != std::string::npos) {
        const std::size_t line_colon = results_json.find(':', pos);
        if (line_colon == std::string::npos) {
            break;
        }
        std::size_t line_start = line_colon + 1;
        while (line_start < results_json.size() &&
               std::isspace(static_cast<unsigned char>(results_json[line_start])) != 0) {
            ++line_start;
        }
        std::size_t line_end = line_start;
        while (line_end < results_json.size() &&
               std::isdigit(static_cast<unsigned char>(results_json[line_end])) != 0) {
            ++line_end;
        }
        if (line_end <= line_start) {
            pos += 6;
            continue;
        }

        int bp_line = 0;
        try {
            bp_line = std::stoi(results_json.substr(line_start, line_end - line_start));
        } catch (...) {
            pos = line_end;
            continue;
        }
        if (bp_line <= 0) {
            pos = line_end;
            continue;
        }

        const std::size_t hit_key = results_json.find("\"hitCount\"", line_end);
        const std::size_t next_object = results_json.find('{', line_end);
        if (hit_key == std::string::npos || (next_object != std::string::npos && hit_key > next_object)) {
            pos = line_end;
            continue;
        }
        const std::size_t hit_colon = results_json.find(':', hit_key);
        if (hit_colon == std::string::npos) {
            pos = line_end;
            continue;
        }
        std::size_t hit_start = hit_colon + 1;
        while (hit_start < results_json.size() &&
               std::isspace(static_cast<unsigned char>(results_json[hit_start])) != 0) {
            ++hit_start;
        }
        std::size_t hit_end = hit_start;
        while (hit_end < results_json.size() &&
               std::isdigit(static_cast<unsigned char>(results_json[hit_end])) != 0) {
            ++hit_end;
        }
        if (hit_end <= hit_start) {
            pos = line_end;
            continue;
        }

        try {
            const std::uint64_t hit_count =
                static_cast<std::uint64_t>(std::stoull(results_json.substr(hit_start, hit_end - hit_start)));
            if (hit_count > 0) {
                apply_breakpoint_hit_entry(path_it->first, bp_line, hit_count);
            }
        } catch (...) {
        }

        pos = line_end;
    }
}

void DebugApp::apply_breakpoint_hits_from_snapshot_json(const std::string& json) {
    for (const BreakpointHitUpdate& update : parse_breakpoint_hits_from_poll_json(json)) {
        apply_breakpoint_hit_entry(update.path, update.line, update.hit_count);
    }
}

void DebugApp::open_breakpoint_condition_editor(const std::string& path, int line,
                                                std::optional<tuinator::Point> action_anchor,
                                                std::optional<int> breakpoints_display_index) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0 || context_menu_ == nullptr) {
        return;
    }

    auto path_it = find_breakpoints_path(normalized);
    if (path_it == breakpoints_by_path_.end() || !path_it->second.contains(line)) {
        model_.status_message = "No breakpoint on this line";
        sync_status_bar();
        return;
    }

    const BreakpointInfo& info = path_it->second.at(line);
    const bool has_when = !info.condition.empty();
    const bool has_hit = !info.hit_condition.empty();
    const std::string bp_path = path_it->first;

    std::vector<ContextMenu::Item> items;
    items.push_back(ContextMenu::Item{
        has_when ? "Edit when condition" : "Add when condition",
        [this, bp_path, line]() { begin_edit_breakpoint_condition(bp_path, line); },
    });
    items.push_back(ContextMenu::Item{
        has_hit ? "Edit hit condition" : "Add hit condition",
        [this, bp_path, line]() { begin_edit_breakpoint_hit_condition(bp_path, line); },
    });

    tuinator::Point menu_anchor{};
    bool open_above = false;
    if (breakpoints_display_index.has_value() && breakpoints_panel_ != nullptr) {
        menu_anchor = action_anchor.has_value()
                          ? *action_anchor
                          : breakpoints_panel_->row_action_anchor(*breakpoints_display_index, RowActionType::Add);
        open_above = true;
    } else if (action_anchor.has_value()) {
        menu_anchor = *action_anchor;
    }

    const tuinator::Rect clip_bounds =
        breakpoints_display_index.has_value() ? breakpoints_panel_clip_bounds() : overlay_clip_bounds();
    context_menu_->open(menu_anchor, clip_bounds, std::move(items), open_above);
    context_menu_->layout(clip_bounds);
    request_repaint();
}

void DebugApp::begin_edit_breakpoint_condition(const std::string& path, int line) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    auto path_it = find_breakpoints_path(normalized);
    if (path_it == breakpoints_by_path_.end() || !path_it->second.contains(line)) {
        model_.status_message = "No breakpoint on this line";
        sync_status_bar();
        return;
    }

    editing_breakpoint_path_ = path_it->first;
    editing_breakpoint_line_ = line;
    editing_breakpoint_hit_ = false;
    breakpoint_input_draft_ = path_it->second.at(line).condition;
    breakpoint_input_focused_ = true;
    model_.focus = Focus::Breakpoints;

    if (breakpoints_panel_ != nullptr) {
        breakpoints_panel_->set_inline_edit(path_it->first, line, false, breakpoint_input_draft_);
        sync_breakpoints_list_panel();
        breakpoints_panel_->focus_inline_edit();
    }
    apply_focus();

    model_.status_message = "When condition for " + panel_title_from_path(path_it->first) + ":" +
                            std::to_string(line) + " — Enter to save, Esc to cancel";
    sync_status_bar();
    request_repaint();
}

void DebugApp::begin_edit_breakpoint_hit_condition(const std::string& path, int line) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    auto path_it = find_breakpoints_path(normalized);
    if (path_it == breakpoints_by_path_.end() || !path_it->second.contains(line)) {
        model_.status_message = "No breakpoint on this line";
        sync_status_bar();
        return;
    }

    editing_breakpoint_path_ = path_it->first;
    editing_breakpoint_line_ = line;
    editing_breakpoint_hit_ = true;
    breakpoint_input_draft_ = path_it->second.at(line).hit_condition;
    breakpoint_input_focused_ = true;
    model_.focus = Focus::Breakpoints;

    if (breakpoints_panel_ != nullptr) {
        breakpoints_panel_->set_inline_edit(path_it->first, line, true, breakpoint_input_draft_);
        sync_breakpoints_list_panel();
        breakpoints_panel_->focus_inline_edit();
    }
    apply_focus();

    model_.status_message = "Hit condition for " + panel_title_from_path(path_it->first) + ":" +
                            std::to_string(line) + " — Enter to save, Esc to cancel";
    sync_status_bar();
    request_repaint();
}

void DebugApp::sync_breakpoint_panel_input() {
    if (breakpoints_panel_ == nullptr) {
        return;
    }
    if (!editing_exception_filter_.empty()) {
        breakpoints_panel_->set_exception_inline_edit(editing_exception_filter_, breakpoint_input_draft_);
        sync_breakpoints_list_panel();
        breakpoint_input_focused_ = true;
        model_.focus = Focus::Breakpoints;
        breakpoints_panel_->focus_inline_edit();
        apply_focus();
        sync_status_bar();
        return;
    }
    if (editing_breakpoint_path_.empty() || editing_breakpoint_line_ <= 0) {
        return;
    }

    breakpoints_panel_->set_inline_edit(editing_breakpoint_path_, editing_breakpoint_line_, editing_breakpoint_hit_,
                                        breakpoint_input_draft_);
    sync_breakpoints_list_panel();
    breakpoint_input_focused_ = true;
    model_.focus = Focus::Breakpoints;
    breakpoints_panel_->focus_inline_edit();
    apply_focus();
    sync_status_bar();
}

bool DebugApp::handle_breakpoint_input_key(const tuinator::Event& event) {
    if (!breakpoint_prompt_active() || breakpoints_panel_ == nullptr) {
        return false;
    }

    if (!breakpoints_panel_->has_inline_edit()) {
        apply_focus();
        return false;
    }
    return breakpoints_panel_->handle_inline_edit_key(event);
}

void DebugApp::submit_breakpoint_condition(const std::string& condition) {
    if (editing_breakpoint_path_.empty() || editing_breakpoint_line_ <= 0) {
        return;
    }

    if (editing_breakpoint_hit_) {
        set_breakpoint_hit_condition(editing_breakpoint_path_, editing_breakpoint_line_, condition);
    } else {
        set_breakpoint_condition(editing_breakpoint_path_, editing_breakpoint_line_, condition);
    }
    blur_breakpoint_input(false);
}

void DebugApp::capture_breakpoint_input_state() {
    if (breakpoints_panel_ == nullptr) {
        return;
    }
    if (breakpoints_panel_->has_inline_edit()) {
        breakpoint_input_draft_ = breakpoints_panel_->inline_edit_value();
        breakpoint_input_focused_ = true;
        model_.focus = Focus::Breakpoints;
    }
}

void DebugApp::restore_breakpoint_input_state() {
    if (breakpoints_panel_ == nullptr) {
        return;
    }
    if (!editing_exception_filter_.empty() ||
        (!editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0)) {
        sync_breakpoint_panel_input();
        return;
    }
    if (!breakpoint_input_focused_) {
        return;
    }
    model_.focus = Focus::Breakpoints;
    breakpoints_panel_->focus_input();
}

void DebugApp::begin_edit_variable(const std::string& variable_name) {
    if (!is_session_stopped()) {
        model_.status_message = "Cannot edit variables while running";
        sync_status_bar();
        return;
    }

    const std::optional<VariableEditTarget> target = find_variable_for_edit(model_, variable_name);
    if (!target.has_value()) {
        model_.status_message = "Variable not found: " + variable_name;
        sync_status_bar();
        return;
    }

    editing_variable_name_ = variable_name;
    editing_variables_reference_ = target->variables_reference;
    scope_input_draft_ = target->value;
    scope_input_focused_ = true;
    model_.focus = Focus::Scopes;

    if (scopes_panel_ != nullptr) {
        scopes_panel_->set_inline_edit(variable_name, scope_input_draft_);
        sync_scopes_list_panel();
        scopes_panel_->focus_inline_edit();
    }

    model_.status_message = "Edit " + variable_name + " — Enter to save, Esc to cancel";
    apply_focus();
    sync_status_bar();
    request_repaint();
}

void DebugApp::submit_variable_value(const std::string& value) {
    if (editing_variable_name_.empty() || editing_variables_reference_ <= 0) {
        scope_input_draft_.clear();
        scope_input_focused_ = false;
        if (scopes_panel_ != nullptr) {
            scopes_panel_->clear_inline_edit();
            sync_scopes_list_panel();
        }
        return;
    }

    if (session_io_ == nullptr) {
        model_.status_message = "No debug session";
        sync_status_bar();
        return;
    }

    const std::string variable_name = editing_variable_name_;
    const std::int64_t variables_reference = editing_variables_reference_;

    pending_variable_value_ = value;
    scope_input_draft_ = value;
    scope_value_overrides_[variable_name] = value;
    variable_set_in_flight_ = true;
    scope_input_focused_ = false;
    if (scopes_panel_ != nullptr) {
        scopes_panel_->clear_inline_edit();
    }
    patch_local_variable_value(variable_name, value);
    if (scopes_panel_ != nullptr && scopes_panel_->list_widget() != nullptr) {
        scopes_panel_->list_widget()->set_focused(true);
    }

    session_io_->post_set_variable(variables_reference, variable_name, value);
    model_.status_message = "Setting " + editing_variable_name_ + "…";
    sync_status_bar();
    request_repaint();
}

void DebugApp::capture_scope_input_state() {
    if (scopes_panel_ == nullptr) {
        return;
    }
    if (scopes_panel_->has_inline_edit()) {
        scope_input_draft_ = scopes_panel_->inline_edit_value();
        scope_input_focused_ = true;
        model_.focus = Focus::Scopes;
    }
}

void DebugApp::restore_scope_input_state() {
    if (scopes_panel_ == nullptr) {
        return;
    }
    if (!editing_variable_name_.empty() && scope_input_focused_) {
        scopes_panel_->set_inline_edit(editing_variable_name_, scope_input_draft_);
        sync_scopes_list_panel();
        scopes_panel_->focus_inline_edit();
        model_.focus = Focus::Scopes;
        apply_focus();
        return;
    }
    if (!scope_input_focused_) {
        return;
    }
    model_.focus = Focus::Scopes;
    scopes_panel_->focus_input();
}

void DebugApp::sync_execution_location_ui() {
    if (!is_session_stopped() || source_panel_ == nullptr || model_.execution_line <= 0 ||
        model_.execution_path.empty()) {
        if (source_panel_ != nullptr) {
            source_panel_->set_execution_line(0);
        }
        return;
    }

    const bool viewing =
        viewing_same_source(model_.source_path, model_.source_reference, model_.execution_path,
                            model_.execution_source_reference);
    const int line = static_cast<int>(model_.execution_line);

    if (follow_execution_) {
        maybe_follow_execution();
    }

    source_panel_->set_execution_line(viewing ? line : 0);
    if (viewing) {
        source_panel_->set_cursor_line(line);
        if (follow_execution_) {
            scroll_source_to_line(line);
        } else {
            source_panel_->mark_dirty();
        }
        cached_follow_line_ = model_.execution_line;
    }
    sync_status_bar();
}

void DebugApp::sync_scopes_list_panel() {
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type == SidebarPanelType::Variables) {
            sync_scope_slot(slot);
        }
    });
}

bool DebugApp::scope_prompt_active() const {
    return scope_input_focused_ && !editing_variable_name_.empty();
}

bool DebugApp::handle_scope_input_key(const tuinator::Event& event) {
    if (scopes_panel_ == nullptr || !scopes_panel_->has_active_inline_edit()) {
        return false;
    }
    return scopes_panel_->handle_inline_edit_key(event);
}

bool DebugApp::handle_memory_toolbar_input_key(const tuinator::Event& event) {
    SidebarSlot* slot = memory_slot_for_focus();
    if (slot == nullptr || slot->memory == nullptr) {
        return false;
    }
    if (!memory_toolbar_focused_ && !slot->memory->is_toolbar_active()) {
        return false;
    }
    if (!slot->memory->is_toolbar_active()) {
        memory_toolbar_focused_ = true;
        slot->memory->focus_address_input();
    }
    return slot->memory->handle_toolbar_input_key(event);
}

bool DebugApp::handle_memory_input_key(const tuinator::Event& event) {
    if (SidebarSlot* slot = memory_slot_for_focus(); slot != nullptr && slot->memory != nullptr &&
                              slot->memory->has_inline_edit()) {
        return slot->memory->handle_inline_edit_key(event);
    }
    return false;
}

bool DebugApp::handle_watch_input_key(const tuinator::Event& event) {
    if (watches_panel_ == nullptr || !watches_panel_->has_active_inline_edit()) {
        return false;
    }
    return watches_panel_->handle_inline_edit_key(event);
}

bool DebugApp::handle_repl_input_key(const tuinator::Event& event) {
    if (repl_panel_ == nullptr || model_.focus != Focus::Repl || !repl_panel_->input_active()) {
        return false;
    }

    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        const bool completion_up = key->key == tuinator::Key::Up;
        const bool completion_down = key->key == tuinator::Key::Down;
        if (completion_up || completion_down) {
            const bool completion_active = repl_completion_results_ready_ || repl_completion_pending_id_ != 0 ||
                                           repl_panel_->has_ghost_suggestion();
            if (completion_active) {
                if (repl_completion_results_ready_) {
                    rebuild_repl_completion_matches();
                    if (!repl_completion_matches_.empty()) {
                        cycle_repl_completion(completion_up ? -1 : 1);
                    }
                }
                return true;
            }
        }
    }

    if (repl_panel_->input_widget() != nullptr) {
        repl_panel_->input_widget()->set_focused(true);
    }
    if (repl_panel_->history_widget() != nullptr) {
        repl_panel_->history_widget()->set_focused(false);
    }
    return repl_panel_->handle_input_event(event);
}

void DebugApp::handle_pointer_pick(const tuinator::MouseEvent& mouse) {
    for (const auto& [leaf_id, stack] : layout_stacks_) {
        if (stack != nullptr) {
            stack->finish_rename_on_click_outside(mouse.position);
        }
    }

    if (repl_panel_ == nullptr || !repl_panel_->input_active()) {
        return;
    }

    if (repl_panel_->contains_point(mouse.position)) {
        return;
    }

    deactivate_repl_input(false);
}

void DebugApp::deactivate_repl_input(bool clear_draft) {
    if (repl_panel_ == nullptr) {
        return;
    }

    clear_repl_completion_state();
    repl_last_edit_time_ = {};

    if (clear_draft) {
        repl_input_draft_.clear();
        repl_panel_->set_input_value("");
    } else {
        repl_input_draft_ = repl_panel_->input_value();
    }

    repl_input_focused_ = false;
    repl_panel_->set_input_active(false);
    if (repl_panel_->input_widget() != nullptr) {
        repl_panel_->input_widget()->set_focused(false);
    }
    if (repl_panel_->shell_widget() != nullptr) {
        repl_panel_->shell_widget()->mark_dirty();
    }
    if (repl_panel_->input_widget() != nullptr) {
        repl_panel_->input_widget()->mark_dirty();
    }
}

void DebugApp::blur_repl_input() {
    deactivate_repl_input(true);
    apply_focus();
    request_repaint();
}

bool DebugApp::context_menu_open() const {
    return context_menu_ != nullptr && context_menu_->is_open();
}

bool DebugApp::file_picker_open() const {
    return file_picker_ != nullptr && file_picker_->is_open();
}

bool DebugApp::breakpoint_prompt_active() const {
    return breakpoint_input_focused_ &&
           ((!editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0) || !editing_exception_filter_.empty());
}

bool DebugApp::overlay_intercepts_events() const {
    return context_menu_open() || file_picker_open() || layout_drag_active();
}

void DebugApp::sync_overlay_mouse_tracking() {
    // xterm mode 1003 + Tuinator pointer_hover_tracking: required for toolbar tooltips,
    // context-menu hover, and other pointer-move UI without holding a mouse button.
    constexpr bool want = true;
    if (want == overlay_hover_tracking_active_) {
        return;
    }

    overlay_hover_tracking_active_ = want;
    set_xterm_mouse_hover_tracking(want);
    if (app_ != nullptr) {
        app_->set_pointer_hover_tracking(want);
    }
}

void DebugApp::blur_breakpoint_input(bool cancelled) {
    editing_breakpoint_path_.clear();
    editing_breakpoint_line_ = 0;
    editing_breakpoint_hit_ = false;
    editing_exception_filter_.clear();
    breakpoint_input_draft_.clear();
    breakpoint_input_focused_ = false;
    if (breakpoints_panel_ != nullptr) {
        breakpoints_panel_->clear_inline_edit();
        sync_breakpoints_list_panel();
        if (breakpoints_panel_->list_widget() != nullptr) {
            breakpoints_panel_->list_widget()->set_focused(true);
        }
    }
    apply_focus();
    if (cancelled) {
        model_.status_message = "Breakpoint edit cancelled";
    }
    sync_status_bar();
    request_repaint();
}

bool DebugApp::handle_overlay_event(const tuinator::Event& event) {
    sync_overlay_mouse_tracking();

    if (file_picker_ != nullptr && file_picker_->is_open()) {
        file_picker_->layout(overlay_clip_bounds());
        const bool handled = file_picker_->handle_event(event);
        if (handled) {
            request_repaint();
        }
        return handled;
    }

    if (context_menu_ != nullptr && context_menu_->is_open()) {
        context_menu_->layout(overlay_clip_bounds());
        const bool handled = context_menu_->handle_event(event);
        std::function<void()> pending_action = context_menu_->take_pending_action();
        if (pending_action) {
            pending_action();
        }
        if (handled || pending_action) {
            request_repaint();
        }
        const bool consumed = handled || static_cast<bool>(pending_action);
        sync_overlay_mouse_tracking();
        return consumed;
    }

    sync_overlay_mouse_tracking();
    return false;
}

void DebugApp::paint_overlay(tuinator::PaintContext& ctx) const {
    if (controls_bar_ != nullptr) {
        controls_bar_->paint_tooltip(ctx, overlay_clip_bounds());
    }
    paint_layout_drag_overlay(ctx);
    paint_layout_menu_preview(ctx);
    if (file_picker_ != nullptr && file_picker_->is_open()) {
        file_picker_->layout(overlay_clip_bounds());
        file_picker_->paint(ctx);
    }
    if (context_menu_ != nullptr && context_menu_->is_open()) {
        context_menu_->layout(overlay_clip_bounds());
        context_menu_->paint(ctx);
    }
}

tuinator::Rect DebugApp::overlay_clip_bounds() const {
    const tuinator::Size term = app_->terminal_size();
    return {0, 0, term.width, term.height};
}

tuinator::Rect DebugApp::breakpoints_panel_clip_bounds() const {
    if (breakpoints_panel_ != nullptr) {
        const tuinator::Rect panel_bounds = breakpoints_panel_->panel_bounds();
        if (panel_bounds.width > 0 && panel_bounds.height > 0) {
            return panel_bounds;
        }
    }
    return overlay_clip_bounds();
}

tuinator::Rect DebugApp::source_context_clip_bounds() const {
    if (source_scroll_view_ != nullptr) {
        return source_scroll_view_->bounds();
    }
    if (source_panel_ != nullptr) {
        return source_panel_->bounds();
    }
    return overlay_clip_bounds();
}

std::optional<SourceContextIdentifier> DebugApp::resolve_source_identifier(const std::string& path,
                                                                           const std::string& source_text, int line,
                                                                           int display_column) const {
    if (line <= 0 || display_column < 0 || source_text.empty()) {
        return std::nullopt;
    }

    const std::string line_text = line_text_at(source_text, line);
    const int byte_column = static_cast<int>(tuinator::text_byte_length_for_width(line_text, display_column));
    const std::string language = language_from_path(path);
    if (!highlight_language_is_loaded(language)) {
        return std::nullopt;
    }

    return identifier_at_position_from_treesitter(language, source_text, line, byte_column);
}

std::optional<std::int64_t> DebugApp::scope_container_for_local(const std::string& name) const {
    if (!is_simple_watch_identifier(name)) {
        return std::nullopt;
    }

    for (LayoutNodeId leaf_id : layout_tree_.leaf_ids()) {
        for (const SidebarSlot& slot : layout_tree_.node(leaf_id).leaf.slots) {
            if (slot.config.type != SidebarPanelType::Variables) {
                continue;
            }
            for (const ScopeVariableRowMeta& row : slot.cached_scope_row_meta) {
                if (row.variable_name == name && row.container_reference > 0) {
                    return row.container_reference;
                }
            }
        }
    }

    for (const tui_debug_ui::ScopeInfo& scope : model_.scopes) {
        if (scope.variables_reference <= 0) {
            continue;
        }
        const auto vars_it = model_.scope_variables.find(scope.variables_reference);
        if (vars_it == model_.scope_variables.end()) {
            continue;
        }
        for (const tui_debug_ui::VariableInfo& variable : vars_it->second) {
            if (variable.name == name) {
                return scope.variables_reference;
            }
        }
    }

    return std::nullopt;
}

void DebugApp::begin_watch_expression(const std::string& seed) { begin_add_watch(seed); }

void DebugApp::begin_add_watch(const std::string& seed) {
    editing_watch_index_ = -1;
    watch_input_draft_ = seed;
    watch_input_focused_ = true;
    model_.focus = Focus::Watches;

    if (watches_panel_ != nullptr) {
        watches_panel_->set_inline_edit(-1, seed, "?");
        sync_watches_panel();
        watches_panel_->focus_inline_edit();
    }

    model_.status_message = seed.empty() ? "Add watch expression" : "Watch expression";
    apply_focus();
    sync_status_bar();
    request_repaint();
}

void DebugApp::show_breakpoint_context_menu(const std::string& path, int line, tuinator::Point anchor,
                                            const std::optional<SourceContextIdentifier>& source_identifier) {
    open_source_context_menu(path, line, anchor, source_identifier, {}, false);
}

std::string DebugApp::goto_probe_source_path() const {
    if (!model_.execution_path.empty() && model_.execution_path.rfind("dap:source:", 0) != 0) {
        return normalize_source_path(model_.execution_path);
    }
    if (!model_.source_path.empty() && model_.source_path.rfind("dap:source:", 0) != 0) {
        return normalize_source_path(model_.source_path);
    }
    const std::string preferred = preferred_program_source_path(program_path_);
    if (!preferred.empty()) {
        return normalize_source_path(preferred);
    }
    return effective_source_path();
}

void DebugApp::begin_source_context_menu(const std::string& path, int line, int code_column,
                                         tuinator::Point anchor,
                                         const std::optional<SourceContextIdentifier>& source_identifier) {
    if (context_menu_ == nullptr || path.empty() || line <= 0) {
        return;
    }

    const bool probe_goto =
        !launch_ui_.is_rr_backend && is_session_stopped() && session_io_ != nullptr && session_io_->is_active();

    if (!probe_goto) {
        open_source_context_menu(path, line, anchor, source_identifier, {}, false);
        return;
    }

    pending_source_context_menu_ = PendingSourceContextMenu{path, line, anchor, source_identifier};
    goto_targets_pending_ = true;
    const int dap_column = code_column >= 0 ? code_column + 1 : -1;
    session_io_->request_goto_targets(path, line, dap_column);
}

namespace {

std::vector<std::pair<std::int64_t, std::string>> parse_goto_targets_json(const std::string& json) {
    std::vector<std::pair<std::int64_t, std::string>> targets;
    std::size_t pos = 0;
    while ((pos = json.find("\"id\"", pos)) != std::string::npos) {
        const std::size_t id_colon = json.find(':', pos);
        if (id_colon == std::string::npos) {
            break;
        }
        std::size_t id_start = id_colon + 1;
        while (id_start < json.size() && std::isspace(static_cast<unsigned char>(json[id_start])) != 0) {
            ++id_start;
        }
        std::size_t id_end = id_start;
        while (id_end < json.size() && (std::isdigit(static_cast<unsigned char>(json[id_end])) != 0 ||
                                        json[id_end] == '-')) {
            ++id_end;
        }
        if (id_end <= id_start) {
            pos += 4;
            continue;
        }

        const std::size_t label_key = json.find("\"label\"", id_end);
        if (label_key == std::string::npos) {
            break;
        }
        const std::size_t label_quote = json.find('"', json.find(':', label_key) + 1);
        if (label_quote == std::string::npos) {
            break;
        }
        const std::size_t label_start = label_quote + 1;
        const std::size_t label_end = json.find('"', label_start);
        if (label_end == std::string::npos) {
            break;
        }

        try {
            const std::int64_t id = std::stoll(json.substr(id_start, id_end - id_start));
            targets.emplace_back(id, json.substr(label_start, label_end - label_start));
        } catch (...) {
        }

        pos = label_end + 1;
    }
    return targets;
}

}  // namespace

void DebugApp::handle_goto_targets_payload(const SessionIoEvent& event) {
    goto_targets_pending_ = false;
    if (!pending_source_context_menu_.has_value()) {
        return;
    }

    PendingSourceContextMenu pending = std::move(*pending_source_context_menu_);
    pending_source_context_menu_.reset();

    if (event.scope_ref != pending.line ||
        normalize_source_path(event.detail) != normalize_source_path(pending.path)) {
        open_source_context_menu(pending.path, pending.line, pending.anchor, pending.source_identifier, {}, false);
        return;
    }

    std::vector<std::pair<std::int64_t, std::string>> goto_targets;
    if (event.success) {
        goto_targets = parse_goto_targets_json(event.payload);
    }

    const bool offer_lldb_line_jump =
        goto_targets.empty() && launch_ui_.lldb_goto_line_fallback && pending.line > 0 &&
        static_cast<std::uint32_t>(pending.line) != model_.execution_line;

    open_source_context_menu(pending.path, pending.line, pending.anchor, pending.source_identifier,
                             std::move(goto_targets), offer_lldb_line_jump);
}

void DebugApp::send_goto_command(std::int64_t target_id) {
    if (session_io_ == nullptr || !session_io_->is_active()) {
        model_.status_message = "No active session";
        sync_status_bar();
        return;
    }

    apply_execution_command_started("goto");
    session_io_->post_command(R"({"op":"goto","target_id":)" + std::to_string(target_id) + "}");
    model_.status_message = "Jump to here";
    sync_ui_from_model();
}

void DebugApp::send_goto_line_command(const std::string& path, int line) {
    if (session_io_ == nullptr || !session_io_->is_active()) {
        model_.status_message = "No active session";
        sync_status_bar();
        return;
    }

    std::ostringstream cmd;
    cmd << R"({"op":"goto_line","line":)" << line << R"(,"path":")";
    for (char ch : path) {
        if (ch == '"' || ch == '\\') {
            cmd << '\\';
        }
        cmd << ch;
    }
    cmd << "\"}";
    session_io_->post_command(cmd.str());
    model_.status_message = "Jump to line " + std::to_string(line);
    sync_ui_from_model();
}

void DebugApp::open_source_context_menu(
    const std::string& path, int line, tuinator::Point anchor,
    const std::optional<SourceContextIdentifier>& source_identifier,
    const std::vector<std::pair<std::int64_t, std::string>>& goto_targets,
    bool offer_lldb_line_jump) {
    if (context_menu_ == nullptr || path.empty() || line <= 0) {
        return;
    }

    const tuinator::Point menu_anchor =
        source_panel_ != nullptr ? source_panel_->context_menu_anchor(line, 0) : anchor;
    const tuinator::Rect clip_bounds = source_context_clip_bounds();

    const std::string normalized = normalize_source_path(path);
    const auto path_it = breakpoints_by_path_.find(normalized);
    const bool has_breakpoint =
        path_it != breakpoints_by_path_.end() && path_it->second.contains(line);
    const bool has_condition =
        has_breakpoint && !path_it->second.at(line).condition.empty();
    const bool has_hit_condition =
        has_breakpoint && !path_it->second.at(line).hit_condition.empty();

    auto ensure_breakpoint = [this, normalized, line]() {
        auto& breakpoints = breakpoints_by_path_[normalized];
        if (breakpoints.contains(line)) {
            return;
        }
        breakpoints[line] = BreakpointInfo{.line = line};
        if (normalized == effective_source_path()) {
            sync_breakpoints_to_panel();
        }
        if (session_io_ != nullptr && session_io_->is_active()) {
            push_breakpoints_to_session(normalized);
        }
        sync_breakpoints_list_panel();
        mark_all_panels_dirty();
    };

    std::vector<ContextMenu::Item> items;
    for (const auto& [target_id, label] : goto_targets) {
        const std::string menu_label =
            goto_targets.size() == 1 ? "Jump to here" : ("Jump to here: " + label);
        items.push_back(ContextMenu::Item{
            menu_label,
            [this, target_id]() { send_goto_command(target_id); },
        });
    }
    if (offer_lldb_line_jump) {
        items.push_back(ContextMenu::Item{
            "Jump to here",
            [this, path, line]() { send_goto_line_command(path, line); },
        });
    }

    if (has_breakpoint) {
        items.push_back(ContextMenu::Item{
            "Edit conditions",
            [this, normalized, line, menu_anchor]() {
                open_breakpoint_condition_editor(normalized, line, menu_anchor, std::nullopt);
            },
        });
        if (has_condition) {
            items.push_back(ContextMenu::Item{
                "Clear when condition",
                [this, normalized, line]() { set_breakpoint_condition(normalized, line, ""); },
            });
        }
        if (has_hit_condition) {
            items.push_back(ContextMenu::Item{
                "Clear hit condition",
                [this, normalized, line]() { set_breakpoint_hit_condition(normalized, line, ""); },
            });
        }
        items.push_back(ContextMenu::Item{
            "Remove breakpoint",
            [this, normalized, line]() { remove_breakpoint_at(normalized, line); },
        });
    } else {
        items.push_back(ContextMenu::Item{
            "Add breakpoint",
            [this, normalized, line]() { toggle_breakpoint_at(normalized, line); },
        });
        items.push_back(ContextMenu::Item{
            "Add conditional breakpoint",
            [this, ensure_breakpoint, normalized, line, menu_anchor]() {
                ensure_breakpoint();
                open_breakpoint_condition_editor(normalized, line, menu_anchor, std::nullopt);
            },
        });
    }

    if (adapter_supports_function_breakpoints()) {
        const std::string file_text = read_file_or_empty(normalized);
        if (const std::optional<std::string> function_name =
                function_name_at_breakpoint_line(normalized, file_text, line);
            function_name.has_value()) {
            const bool already_set = has_function_breakpoint(*function_name);
            items.push_back(ContextMenu::Item{
                already_set ? "Remove function breakpoint" : "Add function breakpoint",
                [this, name = *function_name, normalized, line, already_set]() {
                    if (already_set) {
                        remove_function_breakpoint(name);
                    } else {
                        add_function_breakpoint(name, normalized, line);
                    }
                },
            });
        }
    }

    if (source_identifier.has_value() && !source_identifier->watch_expression.empty()) {
        const std::string watch_expr = source_identifier->watch_expression;
        const std::optional<std::string>& local_name = source_identifier->local_name;
        const std::optional<std::int64_t> scope_container =
            local_name.has_value() ? scope_container_for_local(*local_name) : std::nullopt;
        const bool local_in_scope = scope_container.has_value();

        items.push_back(ContextMenu::Item{
            "Add watch",
            [this, watch_expr]() { add_watch(watch_expr); },
        });

        if (local_in_scope && model_.supports_data_breakpoints && is_session_stopped()) {
            const std::string name = *local_name;
            const std::int64_t container_reference = *scope_container;
            items.push_back(ContextMenu::Item{
                "Break on write",
                [this, name, container_reference]() { request_data_breakpoint(name, container_reference, "write"); },
            });
            items.push_back(ContextMenu::Item{
                "Break on read",
                [this, name, container_reference]() { request_data_breakpoint(name, container_reference, "read"); },
            });
            items.push_back(ContextMenu::Item{
                "Break on read/write",
                [this, name, container_reference]() {
                    request_data_breakpoint(name, container_reference, "readWrite");
                },
            });
        }
    }

    context_menu_->open(menu_anchor, clip_bounds, std::move(items));
    context_menu_->layout(clip_bounds);
    request_full_screen_refresh();
}

std::string DebugApp::command_status_message(const char* op) const {
    if (std::strcmp(op, "continue") == 0 || std::strcmp(op, "play_pause") == 0) {
        return is_session_stopped() ? "Running…" : "Pausing…";
    }
    if (std::strcmp(op, "pause") == 0) {
        return "Pausing…";
    }
    if (std::strcmp(op, "restart") == 0) {
        return "Restarting…";
    }
    if (std::strcmp(op, "disconnect") == 0) {
        return "Disconnected";
    }
    if (std::strcmp(op, "terminate") == 0) {
        return "Session ended";
    }
    if (std::strcmp(op, "step_over") == 0) {
        return "Step over";
    }
    if (std::strcmp(op, "step_into") == 0) {
        return "Step into";
    }
    if (std::strcmp(op, "step_out") == 0) {
        return "Step out";
    }
    if (std::strcmp(op, "reverse_continue") == 0) {
        return "Continue back";
    }
    if (std::strcmp(op, "step_back") == 0) {
        return "Step back";
    }
    if (std::strcmp(op, "step_back_into") == 0) {
        return "Step back into";
    }
    return std::string("Sent: ") + op;
}

void DebugApp::capture_watch_input_state() {
    if (watches_panel_ == nullptr) {
        return;
    }
    if (watches_panel_->has_inline_edit()) {
        watch_input_draft_ = watches_panel_->inline_edit_value();
        watch_input_focused_ = true;
        model_.focus = Focus::Watches;
    }
}

void DebugApp::restore_watch_input_state() {
    if (watches_panel_ == nullptr) {
        return;
    }
    if (watch_input_focused_) {
        const std::vector<WatchEntry>& watches = active_watch_list();
        if (editing_watch_index_ >= 0 && editing_watch_index_ < static_cast<int>(watches.size())) {
            const WatchEntry& watch = watches[static_cast<std::size_t>(editing_watch_index_)];
            std::string suffix;
            if (!watch.error.empty()) {
                suffix = "<error: " + watch.error + ">";
            } else if (watch.value.empty()) {
                suffix = "?";
            } else {
                suffix = watch.value;
            }
            watches_panel_->set_inline_edit(editing_watch_index_, watch_input_draft_, suffix);
        } else {
            watches_panel_->set_inline_edit(-1, watch_input_draft_, "?");
        }
        sync_watches_panel();
        watches_panel_->focus_inline_edit();
        model_.focus = Focus::Watches;
    }
    apply_focus();
}

void DebugApp::sync_repl_panel() {
    if (repl_panel_ == nullptr) {
        return;
    }

    std::vector<std::string> lines;
    for (const std::string& entry : model_.repl_history) {
        std::string::size_type start = 0;
        while (start <= entry.size()) {
            const std::string::size_type end = entry.find('\n', start);
            if (end == std::string::npos) {
                if (start < entry.size()) {
                    lines.push_back(entry.substr(start));
                }
                break;
            }
            lines.push_back(entry.substr(start, end - start));
            start = end + 1;
        }
    }
    repl_panel_->set_history_lines(std::move(lines));
}

void DebugApp::rebuild_repl_completion_matches() {
    repl_completion_matches_.clear();
    if (repl_panel_ == nullptr || repl_completion_candidates_.empty()) {
        return;
    }

    const std::string& text = repl_panel_->input_value();
    const std::size_t cursor = repl_panel_->input_cursor_column();
    const auto [token_start, token_length] = repl_completion_token_range(text, cursor);
    const std::string typed = text.substr(token_start, token_length);

    for (const ReplCompletionCandidate& candidate : repl_completion_candidates_) {
        if (!typed.empty() && repl_completion_ghost_suffix(typed, candidate.label).empty()) {
            continue;
        }
        repl_completion_matches_.push_back(candidate);
    }

    if (repl_completion_matches_.empty()) {
        repl_completion_selected_index_ = 0;
        return;
    }

    if (repl_completion_selected_index_ < 0 ||
        repl_completion_selected_index_ >= static_cast<int>(repl_completion_matches_.size())) {
        repl_completion_selected_index_ = 0;
    }
}

void DebugApp::clear_repl_completion_state() {
    repl_completion_pending_id_ = 0;
    repl_completion_fetch_sent_ = false;
    repl_completion_results_ready_ = false;
    repl_completion_candidates_.clear();
    repl_completion_matches_.clear();
    repl_completion_selected_index_ = 0;
    if (repl_panel_ != nullptr) {
        repl_panel_->set_completion_menu_active(false);
        repl_panel_->clear_ghost_suggestion();
    }
}

void DebugApp::tick_repl_completion() {
    if (repl_panel_ == nullptr || session_io_ == nullptr) {
        return;
    }
    if (model_.focus != Focus::Repl || !repl_panel_->input_active()) {
        return;
    }
    if (!is_session_stopped()) {
        return;
    }
    if (!model_.supports_completions_request && mode_ != SessionMode::Mock) {
        return;
    }

    const std::string text = repl_panel_->input_value();
    if (text.empty()) {
        return;
    }
    if (repl_last_edit_time_ == std::chrono::steady_clock::time_point{}) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = now - repl_last_edit_time_;

    if (elapsed >= kReplCompletionFetchDelay && !repl_completion_fetch_sent_ && repl_completion_pending_id_ == 0) {
        request_repl_completion();
    }

    if (repl_completion_results_ready_ && elapsed >= kReplCompletionShowDelay && !repl_panel_->has_ghost_suggestion()) {
        show_repl_completion_ghost();
    }
}

void DebugApp::request_repl_completion() {
    if (repl_panel_ == nullptr || session_io_ == nullptr) {
        return;
    }
    if (!is_session_stopped()) {
        return;
    }
    if (!model_.supports_completions_request && mode_ != SessionMode::Mock) {
        return;
    }

    const std::int64_t frame_id = current_frame_id();
    if (frame_id <= 0) {
        return;
    }

    const std::string text = repl_panel_->input_value();
    if (text.empty()) {
        return;
    }

    const std::size_t cursor = repl_panel_->input_cursor_column();
    const std::int64_t column = static_cast<std::int64_t>(cursor) + 1;

    repl_completion_fetch_sent_ = true;
    repl_completion_results_ready_ = false;
    repl_completion_candidates_.clear();
    repl_completion_matches_.clear();
    repl_completion_selected_index_ = 0;
    repl_panel_->set_completion_menu_active(false);
    repl_panel_->clear_ghost_suggestion();

    repl_completion_request_id_++;
    repl_completion_pending_id_ = repl_completion_request_id_;
    repl_completion_request_text_ = text;
    repl_completion_request_column_ = cursor;

    session_io_->post_completions(text, column, frame_id, repl_completion_request_id_);
}

void DebugApp::show_repl_completion_ghost() {
    if (repl_panel_ == nullptr) {
        return;
    }

    rebuild_repl_completion_matches();
    if (repl_completion_matches_.empty()) {
        repl_panel_->set_completion_menu_active(false);
        repl_panel_->clear_ghost_suggestion();
        request_repaint();
        return;
    }

    const std::string& text = repl_panel_->input_value();
    const std::size_t cursor = repl_panel_->input_cursor_column();
    const auto [token_start, token_length] = repl_completion_token_range(text, cursor);
    const std::string typed = text.substr(token_start, token_length);

    const ReplCompletionCandidate& candidate =
        repl_completion_matches_[static_cast<std::size_t>(repl_completion_selected_index_)];
    const std::string suffix = repl_completion_ghost_suffix(typed, candidate.label);
    if (suffix.empty()) {
        repl_panel_->set_completion_menu_active(false);
        repl_panel_->clear_ghost_suggestion();
        request_repaint();
        return;
    }

    ReplGhostSuggestion ghost;
    ghost.label = candidate.label;
    ghost.replace_start = token_start;
    ghost.replace_length = token_length;
    ghost.suffix = suffix;
    repl_panel_->set_completion_menu_active(repl_completion_matches_.size() > 1);
    repl_panel_->set_ghost_suggestion(std::move(ghost));
    request_repaint();
}

bool DebugApp::cycle_repl_completion(int delta) {
    if (repl_panel_ == nullptr) {
        return false;
    }

    rebuild_repl_completion_matches();
    if (repl_completion_matches_.empty()) {
        return false;
    }

    if (!repl_panel_->has_ghost_suggestion()) {
        show_repl_completion_ghost();
        return repl_panel_->has_ghost_suggestion();
    }

    if (repl_completion_matches_.size() < 2) {
        return true;
    }

    const int count = static_cast<int>(repl_completion_matches_.size());
    repl_completion_selected_index_ = (repl_completion_selected_index_ + delta + count) % count;
    show_repl_completion_ghost();
    return true;
}

void DebugApp::handle_repl_completions_event(const SessionIoEvent& event) {
    if (repl_panel_ == nullptr) {
        return;
    }

    std::uint64_t request_id = 0;
    try {
        request_id = static_cast<std::uint64_t>(std::stoull(event.detail));
    } catch (const std::exception&) {
        return;
    }
    if (request_id != repl_completion_pending_id_) {
        return;
    }
    repl_completion_pending_id_ = 0;

    if (!event.success) {
        return;
    }

    if (repl_panel_->input_value() != repl_completion_request_text_ ||
        repl_panel_->input_cursor_column() != repl_completion_request_column_) {
        return;
    }

    repl_completion_candidates_ = parse_repl_completions(event.payload);
    repl_completion_selected_index_ = 0;
    repl_completion_results_ready_ = !repl_completion_candidates_.empty();
    rebuild_repl_completion_matches();
    if (!repl_completion_results_ready_) {
        repl_panel_->set_completion_menu_active(false);
        repl_panel_->clear_ghost_suggestion();
        request_repaint();
        return;
    }

    const auto elapsed = std::chrono::steady_clock::now() - repl_last_edit_time_;
    if (elapsed >= kReplCompletionShowDelay) {
        show_repl_completion_ghost();
    } else {
        request_repaint();
    }
}

void DebugApp::submit_repl_expression(const std::string& expression) {
    std::string trimmed = expression;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        repl_input_draft_.clear();
        if (repl_panel_ != nullptr) {
            repl_panel_->set_input_value("");
        }
        return;
    }

    const std::int64_t frame_id = current_frame_id();
    if (frame_id <= 0) {
        model_.repl_history.push_back("> " + trimmed + "\n= (no active frame)");
        sync_repl_panel();
        model_.status_message = "No active frame for eval";
        sync_status_bar();
        repl_input_draft_.clear();
        if (repl_panel_ != nullptr) {
            repl_panel_->set_input_value("");
        }
        return;
    }

    if (session_io_ != nullptr) {
        session_io_->post_evaluate(trimmed, frame_id, "repl");
        model_.status_message = "Evaluating: " + trimmed;
        sync_status_bar();
    }

    repl_input_draft_.clear();
    if (repl_panel_ != nullptr) {
        repl_panel_->set_input_value("");
    }
}

void DebugApp::sync_watches_panel() {
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type != SidebarPanelType::Watches || slot.watches == nullptr) {
            return;
        }
        std::vector<std::string> lines;
        lines.reserve(slot.watches_data.size());
        for (const WatchEntry& watch : slot.watches_data) {
            if (!watch.error.empty()) {
                lines.push_back(watch.expression + " = <error: " + watch.error + ">");
            } else if (watch.value.empty()) {
                lines.push_back(watch.expression + " = ?");
            } else {
                lines.push_back(watch.expression + " = " + watch.value);
            }
        }
        slot.watches->set_lines(std::move(lines));
    });
}

void DebugApp::begin_edit_watch_at(int index) {
    std::vector<WatchEntry>& watches = active_watch_list();
    if (watches_panel_ == nullptr || index < 0 || index >= static_cast<int>(watches.size())) {
        return;
    }

    const WatchEntry& watch = watches[static_cast<std::size_t>(index)];
    std::string suffix;
    if (!watch.error.empty()) {
        suffix = "<error: " + watch.error + ">";
    } else if (watch.value.empty()) {
        suffix = "?";
    } else {
        suffix = watch.value;
    }

    editing_watch_index_ = index;
    watch_input_draft_ = watch.expression;
    watch_input_focused_ = true;
    model_.focus = Focus::Watches;
    watches_panel_->set_inline_edit(index, watch_input_draft_, suffix);
    sync_watches_panel();
    watches_panel_->focus_inline_edit();
    model_.status_message = "Edit watch expression";
    apply_focus();
    sync_status_bar();
    request_repaint();
}

void DebugApp::submit_watch_expression(const std::string& expression) {
    std::string trimmed = expression;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }

    if (trimmed.empty()) {
        finish_watch_input();
        return;
    }

    std::vector<WatchEntry>& watches = active_watch_list();
    if (editing_watch_index_ >= 0 && editing_watch_index_ < static_cast<int>(watches.size())) {

        WatchEntry& watch = watches[static_cast<std::size_t>(editing_watch_index_)];
        watch.expression = normalize_watch_expression(trimmed);
        watch.value.clear();
        watch.error.clear();
        editing_watch_index_ = -1;

        finish_watch_input();
        sync_watches_panel();
        model_.status_message = "Updated watch";
        sync_status_bar();
        resolve_watches_from_locals();
        return;
    }

    add_watch(expression);
}

void DebugApp::add_watch(const std::string& expression) {
    editing_watch_index_ = -1;
    std::string trimmed = expression;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }
    if (trimmed.empty()) {
        return;
    }

    WatchEntry entry{};
    entry.id = model_.next_watch_id++;
    entry.expression = normalize_watch_expression(trimmed);
    active_watch_list().push_back(std::move(entry));

    finish_watch_input();
    sync_watches_panel();
    model_.status_message = "Added watch";
    sync_status_bar();

    resolve_watches_from_locals();
}

void DebugApp::remove_watch_at(std::size_t index) {
    std::vector<WatchEntry>& watches = active_watch_list();
    if (index >= watches.size()) {
        return;
    }
    if (editing_watch_index_ == static_cast<int>(index)) {
        finish_watch_input();
    } else if (editing_watch_index_ > static_cast<int>(index)) {
        --editing_watch_index_;
    }
    watches.erase(watches.begin() + static_cast<std::ptrdiff_t>(index));
    sync_watches_panel();
    model_.status_message = "Removed watch";
    sync_status_bar();
}

void DebugApp::resolve_watches_from_locals() {
    if (!launch_complete_handled_ || !is_session_stopped()) {
        return;
    }

    bool has_watches = false;
    for_each_sidebar_slot([&](const SidebarSlot& slot) {
        if (slot.config.type == SidebarPanelType::Watches && !slot.watches_data.empty()) {
            has_watches = true;
        }
    });
    if (!has_watches) {
        return;
    }

    bool changed = false;
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type != SidebarPanelType::Watches) {
            return;
        }

        std::vector<std::string> scope_rows = slot.cached_scope_rows;
        if (scope_rows.empty()) {
            std::vector<ScopeVariableRowMeta> meta;
            scope_rows = build_scope_rows(model_, expanded_scope_paths_, pending_scope_paths_,
                                          collapsed_scope_sections_, meta, slot.config.scope_filter);
        }
        const bool locals_visible = !model_.variables.empty() || !model_.scope_variables.empty() ||
                                    scope_rows_include_variables(scope_rows);

        for (WatchEntry& watch : slot.watches_data) {
            watch.expression = normalize_watch_expression(watch.expression);

            std::optional<std::string> resolved = try_resolve_watch_from_model(model_, watch.expression);
            if (!resolved) {
                resolved = try_resolve_watch_from_scope_rows(scope_rows, watch.expression);
            }

            if (resolved) {
                if (watch.value != *resolved || !watch.error.empty()) {
                    watch.value = *resolved;
                    watch.error.clear();
                    changed = true;
                }
                continue;
            }

            if (!locals_visible) {
                continue;
            }

            if (!watch.value.empty() || watch.error != "not in scope") {
                watch.value.clear();
                watch.error = "not in scope";
                changed = true;
            }
        }
    });

    if (changed) {
        sync_watches_panel();
    }
}

void DebugApp::append_console_text(const std::string& text, const std::string& category) {
    if (text.empty()) {
        return;
    }

    ConsoleLine entry{};
    entry.category = category;
    entry.text = text;
    model_.console_lines.push_back(entry);

    if (console_panel_ != nullptr) {
        console_panel_->feed_output(text);
        console_synced_line_count_ = model_.console_lines.size();
    }
}

void DebugApp::remove_dollar_exception_watches() {
    bool changed = false;
    for_each_sidebar_slot([&](SidebarSlot& slot) {
        if (slot.config.type != SidebarPanelType::Watches) {
            return;
        }
        for (auto it = slot.watches_data.begin(); it != slot.watches_data.end();) {
            if (normalize_watch_expression(it->expression) != "$exception") {
                ++it;
                continue;
            }

            const int index = static_cast<int>(std::distance(slot.watches_data.begin(), it));
            if (watches_panel_ != nullptr && active_slot_for_focus() == &slot && editing_watch_index_ == index) {
                finish_watch_input();
            } else if (watches_panel_ != nullptr && active_slot_for_focus() == &slot &&
                       editing_watch_index_ > index) {
                --editing_watch_index_;
            }

            it = slot.watches_data.erase(it);
            changed = true;
        }
    });

    if (changed) {
        sync_watches_panel();
    }
}

void DebugApp::handle_exception_info_from_snapshot() {
    if (model_.stop_reason != "exception") {
        return;
    }

    remove_dollar_exception_watches();

    if (!model_.exception_info.has_value()) {
        return;
    }

    const ExceptionInfo& info = *model_.exception_info;
    const std::string key = info.exception_id + "|" + info.description + "|" + info.message + "|" +
                            info.type_name + "|" + std::to_string(model_.stopped_thread_id);
    if (key == last_logged_exception_key_) {
        return;
    }
    last_logged_exception_key_ = key;

    std::string output = "[exception]";
    if (!info.description.empty()) {
        output += " " + info.description;
    } else if (!info.exception_id.empty()) {
        output += " " + info.exception_id;
    }
    if (!info.break_mode.empty()) {
        output += " (" + info.break_mode + ")";
    }
    output += "\n";
    if (!info.type_name.empty()) {
        output += "  type: " + info.type_name + "\n";
    }
    if (!info.message.empty()) {
        output += "  message: " + info.message + "\n";
    }
    if (!info.stack_trace.empty()) {
        output += "  stack:\n";
        std::string trace = info.stack_trace;
        std::size_t start = 0;
        while (start < trace.size()) {
            const std::size_t end = trace.find('\n', start);
            const std::string_view line(trace.data() + start,
                                        end == std::string::npos ? trace.size() - start : end - start);
            output += "    ";
            output.append(line.begin(), line.end());
            output += "\n";
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }

    append_console_text(output, "exception");

    model_.focus = Focus::Console;
    apply_focus();
}

}  // namespace tui_debug_ui
