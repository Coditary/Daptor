#include "tui_debug_ui/app.hpp"

#include "tui_debug_ui/background_widget.hpp"
#include "tui_debug_ui/console_panel.hpp"
#include "tui_debug_ui/controls_bar.hpp"
#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/highlight_bridge.hpp"
#include "tui_debug_ui/resizable_split_pane.hpp"
#include "tui_debug_ui/scopes_panel.hpp"
#include "tui_debug_ui/snapshot_parser.hpp"
#include "tui_debug_ui/source_panel.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/breakpoints_panel.hpp"
#include "tui_debug_ui/context_menu.hpp"
#include "tui_debug_ui/stacks_panel.hpp"
#include "tui_debug_ui/watches_panel.hpp"
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
                                          std::vector<tui_debug_ui::ScopeVariableRowMeta>& meta) {
    std::vector<std::string> scope_rows;
    meta.clear();
    for (const tui_debug_ui::ScopeInfo& scope : model.scopes) {
        scope_rows.push_back(scope.name + ":");
        meta.push_back({});
        if (scope.variables_reference > 0) {
            const std::string path_prefix = scope.name + std::string(1, kScopePathSeparator);
            append_scope_variables(model, scope.variables_reference, 1, path_prefix, expanded, pending, scope_rows,
                                   meta);
        }
    }
    return scope_rows;
}

tuinator::Rect union_rect(tuinator::Rect a, tuinator::Rect b) {
    if (a.width <= 0 || a.height <= 0) {
        return b;
    }
    if (b.width <= 0 || b.height <= 0) {
        return a;
    }

    const int x1 = std::min(a.x, b.x);
    const int y1 = std::min(a.y, b.y);
    const int x2 = std::max(a.x + a.width, b.x + b.width);
    const int y2 = std::max(a.y + a.height, b.y + b.height);
    return {x1, y1, x2 - x1, y2 - y1};
}

tuinator::Rect controls_row_rect(const tuinator::Rect& root) {
    return {root.x, root.y, root.width, kControlsBarRows};
}

tuinator::Rect status_row_rect(const tuinator::Rect& root) {
    return {root.x, root.y + root.height - kStatusBarRows, root.width, kStatusBarRows};
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
        }

        if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
            if (is_mouse_position_tracking_action(mouse->action) && bounds_.contains(mouse->position)) {
                last_mouse_position_ = mouse->position;
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

    void set_on_dirty(std::function<void(tuinator::Rect)> callback) override {
        // Keep the fixed chrome rows in sync on partial redraws without repainting the whole terminal.
        const auto include_chrome_rows = [this, callback = std::move(callback)](tuinator::Rect region) {
            if (!callback) {
                return;
            }
            if (region.width <= 0 || region.height <= 0) {
                callback({});
                return;
            }
            if (bounds_.width <= 0 || bounds_.height <= 0) {
                callback(region);
                return;
            }

            tuinator::Rect expanded = union_rect(region, controls_row_rect(bounds_));
            expanded = union_rect(expanded, status_row_rect(bounds_));
            callback(expanded);
        };
        Widget::set_on_dirty(include_chrome_rows);
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

int sidebar_first_size(int terminal_width, std::uint16_t sidebar_pct) {
    const int pct = static_cast<int>(sidebar_pct);
    return std::max(24, terminal_width * pct / 100);
}

int repl_first_size(int terminal_width, std::uint16_t repl_pct) {
    const int pct = static_cast<int>(repl_pct);
    return std::max(16, terminal_width * pct / 100);
}

int watches_sidebar_size(int sidebar_height, std::uint16_t watches_pct) {
    const int pct = static_cast<int>(watches_pct);
    return std::max(5, sidebar_height * pct / 100);
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

std::string language_from_path(const std::string& path) {
    if (path.size() >= 3 && path.compare(path.size() - 3, 3, ".py") == 0) {
        return "python";
    }
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return "python";
    }
    const std::string ext = path.substr(dot + 1);
    if (ext == "py" || ext == "pyw") {
        return "python";
    }
    if (ext == "rs") {
        return "rust";
    }
    if (ext == "cc" || ext == "cxx") {
        return "cpp";
    }
    if (ext == "js" || ext == "ts") {
        return "javascript";
    }
    return ext;
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

DebugApp::DebugApp(const std::string& program_path, SessionMode mode, DebugAdapter adapter,
                   std::vector<std::string> program_args)
    : mode_(mode),
      adapter_(adapter),
      program_path_(program_path),
      program_args_(std::move(program_args)),
      session_io_(std::make_unique<SessionIoThread>(mode, adapter)),
      app_(std::make_unique<tuinator::Application>()) {
    tuinator::Theme theme = app_->theme();
    dap_theme_.apply_to(theme);
    app_->set_theme(theme);

    if (mode_ == SessionMode::Mock) {
        model_.status_message = "Mock UI mode (no Rust backend)";
    } else if (adapter_ == DebugAdapter::Lldb) {
        model_.status_message = "Connecting to lldb-dap…";
    } else if (adapter_ == DebugAdapter::Rr) {
        model_.status_message = "Recording with rr…";
    } else {
        model_.status_message = "Connecting to debugpy…";
    }

}

DebugApp::~DebugApp() = default;

int DebugApp::run() {
    sync_terminal_size_from_tty();
    set_terminal_theme_background(DapUiTheme::kBackground);
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
    cached_scope_rows_.clear();
    cached_stack_lines_.clear();
    cached_source_title_.clear();
    cached_highlight_first_line_ = -1;
    cached_highlight_line_count_ = -1;

    const tuinator::Size term_size = app_->terminal_size();
    const int tray_body = bottom_tray_height(term_size.height, model_.layout.bottom_pct);
    const int main_h = main_area_height(term_size.height, tray_body);

    auto controls = std::make_unique<ControlsBar>(dap_theme_);
    controls_bar_ = controls.get();
    controls->set_on_action([this](const std::string& op) { send_command(op.c_str()); });

    const auto scroll_options = dap_theme_.scroll_view_options();
    scopes_panel_ = std::make_unique<ScopesPanel>(dap_theme_, scroll_options);
    scopes_panel_->set_on_watch([this](const std::string& variable_name) { add_watch(variable_name); });
    scopes_panel_->set_on_edit_variable([this](const std::string& variable_name) {
        begin_edit_variable(variable_name);
    });
    scopes_panel_->set_on_submit([this](const std::string& value) { submit_variable_value(value); });
    scopes_panel_->set_on_change([this](const std::string& value) {
        scope_input_draft_ = value;
        scope_input_focused_ = true;
        model_.focus = Focus::Scopes;
    });
    scopes_panel_->set_on_inline_edit_cancel([this]() { blur_scope_input(); });
    scopes_panel_->set_on_activate([this](int index) { toggle_scope_row_expand(index); });
    scopes_panel_->set_on_context([this](int index, tuinator::Point anchor) {
        show_scope_variable_context_menu(index, anchor);
    });
    stacks_panel_ = std::make_unique<StacksPanel>(dap_theme_, scroll_options);
    breakpoints_panel_ = std::make_unique<BreakpointsPanel>(dap_theme_, scroll_options);
    const auto jump_to_stack_frame = [this](const StackFrameRow& frame) {
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
    };
    stacks_panel_->set_on_continue([this]() { send_command("continue"); });
    stacks_panel_->set_on_activate(jump_to_stack_frame);
    stacks_panel_->set_on_context([this](const StackFrameRow& frame, tuinator::Point anchor) {
        show_stack_frame_context_menu(frame, anchor);
    });
    breakpoints_panel_->set_on_activate([this](const BreakpointRow& row) {
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
    breakpoints_panel_->set_on_remove([this](const BreakpointRow& row) {
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
    breakpoints_panel_->set_on_add_condition([this](const BreakpointRow& row, int display_index,
                                                    tuinator::Point action_anchor) {
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
    breakpoints_panel_->set_on_edit_when_condition([this](const BreakpointRow& row, tuinator::Point /*action_anchor*/) {
        if (row.kind == BreakpointRowKind::Exception) {
            begin_edit_exception_condition(row.data_id);
            return;
        }
        begin_edit_breakpoint_condition(row.path, row.line);
    });
    breakpoints_panel_->set_on_edit_hit_condition([this](const BreakpointRow& row, tuinator::Point /*action_anchor*/) {
        begin_edit_breakpoint_hit_condition(row.path, row.line);
    });
    breakpoints_panel_->set_on_clear_when_condition([this](const BreakpointRow& row) {
        if (row.kind == BreakpointRowKind::Exception) {
            set_exception_breakpoint_condition(row.data_id, "");
            return;
        }
        set_breakpoint_condition(row.path, row.line, "");
    });
    breakpoints_panel_->set_on_clear_hit_condition([this](const BreakpointRow& row) {
        set_breakpoint_hit_condition(row.path, row.line, "");
    });
    breakpoints_panel_->set_on_submit([this](const std::string& condition) {
        if (!editing_exception_filter_.empty()) {
            set_exception_breakpoint_condition(editing_exception_filter_, condition);
            blur_breakpoint_input(false);
            return;
        }
        if (!editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0) {
            submit_breakpoint_condition(condition);
        }
    });
    breakpoints_panel_->set_on_change([this](const std::string& condition) {
        if (!editing_exception_filter_.empty() ||
            (!editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0)) {
            breakpoint_input_draft_ = condition;
            breakpoint_input_focused_ = true;
            model_.focus = Focus::Breakpoints;
        }
    });
    breakpoints_panel_->set_on_inline_edit_cancel([this]() { blur_breakpoint_input(true); });

    watches_panel_ = std::make_unique<WatchesPanel>(dap_theme_, scroll_options);
    watches_panel_->set_on_submit([this](const std::string& expression) { submit_watch_expression(expression); });
    watches_panel_->set_on_change([this](const std::string& expression) {
        watch_input_draft_ = expression;
        watch_input_focused_ = true;
        model_.focus = Focus::Watches;
    });
    watches_panel_->set_on_remove([this](int index) { remove_watch_at(static_cast<std::size_t>(index)); });
    watches_panel_->set_on_edit([this](int index) { begin_edit_watch_at(index); });
    if (!watch_input_draft_.empty()) {
        watches_panel_->set_input_value(watch_input_draft_);
    }

    auto scopes_widget = scopes_panel_->release_widget();
    auto stacks_widget = stacks_panel_->release_widget();
    auto breakpoints_widget = breakpoints_panel_->release_widget();

    const int scopes_first = std::max(6, main_h * model_.layout.scopes_pct / 100);
    const int lower_half = std::max(6, (main_h - scopes_first) / 2);

    auto stacks_breakpoints = std::make_unique<ResizableSplitPane>(
        std::move(stacks_widget), std::move(breakpoints_widget),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Vertical,
            .first_size = lower_half,
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    bind_split_pane(stacks_breakpoints.get());

    auto watches_widget = watches_panel_->release_widget();
    watches_widget->set_flex(0);

    auto watches_stacks = std::make_unique<ResizableSplitPane>(
        std::move(watches_widget), std::move(stacks_breakpoints),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Vertical,
            .first_size = watches_sidebar_size(main_h, model_.layout.watches_pct),
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    sidebar_watches_split_ = watches_stacks.get();
    bind_split_pane(sidebar_watches_split_);
    sidebar_watches_split_->set_on_first_size_changed([this](int /*first*/) {
        persist_split_size_as_pct(sidebar_watches_split_, model_.layout.watches_pct, false);
        if (!divider_drag_active_) {
            model_.status_message = "Watches " + std::to_string(model_.layout.watches_pct) + "%";
            if (status_bar_ != nullptr) {
                status_bar_->set_text(format_status_bar_text());
            }
        }
    });

    auto sidebar = std::make_unique<ResizableSplitPane>(
        std::move(scopes_widget), std::move(watches_stacks),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Vertical,
            .first_size = scopes_first,
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    sidebar_split_ = sidebar.get();
    bind_split_pane(sidebar_split_);
    sidebar_split_->set_on_first_size_changed([this](int /*first*/) {
        persist_split_size_as_pct(sidebar_split_, model_.layout.scopes_pct, false);
        if (!divider_drag_active_) {
            model_.status_message = "Scopes " + std::to_string(model_.layout.scopes_pct) + "%";
            if (status_bar_ != nullptr) {
                status_bar_->set_text(format_status_bar_text());
            }
        }
    });

    auto source_panel = std::make_unique<SourcePanel>();
    source_panel_ = source_panel.get();
    context_menu_ = std::make_unique<ContextMenu>(dap_theme_.panel_background, dap_theme_.label, dap_theme_.selection,
                                                  dap_theme_.border_focused);

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

    source_section_ = std::make_unique<TitledScrollPane>(panel_title_from_path(model_.source_path),
                                                         std::move(source_panel), dap_theme_.title_source,
                                                         dap_theme_.panel_background, scroll_options);
    source_scroll_view_ = source_section_->scroll_view();
    if (source_panel_ != nullptr && source_scroll_view_ != nullptr) {
        source_panel_->set_scroll_parent(source_scroll_view_);
    }
    auto source_shell = source_section_->release_widget();
    source_shell->set_flex(1);

    auto main_row = std::make_unique<ResizableSplitPane>(
        std::move(sidebar), std::move(source_shell),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Horizontal,
            .first_size = sidebar_first_size(term_size.width, model_.layout.sidebar_pct),
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    main_row_split_ = main_row.get();
    bind_split_pane(main_row_split_);
    main_row_split_->set_on_first_size_changed([this](int /*first*/) {
        persist_split_size_as_pct(main_row_split_, model_.layout.sidebar_pct, true);
        if (!divider_drag_active_) {
            model_.status_message = "Sidebar " + std::to_string(model_.layout.sidebar_pct) + "%";
            if (status_bar_ != nullptr) {
                status_bar_->set_text(format_status_bar_text());
            }
        }
    });
    main_row->set_flex(1);

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
    auto repl_shell = repl_panel_->release_widget();
    repl_shell->set_flex(1);

    auto console_panel = std::make_unique<ConsolePanel>(dap_theme_);
    console_panel_ = console_panel.get();

    console_panel_->set_line_buffered_input(adapter_ == DebugAdapter::Lldb);
    console_panel_->set_on_activate([this]() {
        model_.focus = Focus::Console;
        apply_focus();
    });
    console_panel_->set_on_input([this](const std::string& bytes) {
        if (session_io_ == nullptr) {
            return;
        }
        session_io_->post_terminal_input(bytes);
        if (adapter_ == DebugAdapter::Lldb && is_session_stopped() && model_.session_state != "running" &&
            !bytes.empty() && bytes.back() == '\n') {
            session_io_->post_command("continue");
        }
    });

    auto console_section = std::make_unique<TitledScrollPane>("Console", std::move(console_panel),
                                                              dap_theme_.title_console, dap_theme_.panel_background,
                                                              scroll_options, false);
    console_scroll_view_ = console_section->scroll_view();
    auto console_shell = console_section->release_widget();
    console_shell->set_flex(1);

    auto bottom_tray = std::make_unique<ResizableSplitPane>(
        std::move(repl_shell), std::move(console_shell),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Horizontal,
            .first_size = repl_first_size(term_size.width, model_.layout.repl_pct),
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    repl_console_split_ = bottom_tray.get();
    bind_split_pane(repl_console_split_);
    repl_console_split_->set_on_first_size_changed([this](int /*first*/) {
        persist_split_size_as_pct(repl_console_split_, model_.layout.repl_pct, true);
        if (!divider_drag_active_) {
            model_.status_message = "REPL " + std::to_string(model_.layout.repl_pct) + "%";
            if (status_bar_ != nullptr) {
                status_bar_->set_text(format_status_bar_text());
            }
        }
    });

    auto content_split = std::make_unique<ResizableSplitPane>(
        std::move(main_row), std::move(bottom_tray),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Vertical,
            .first_size = main_h,
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    content_split_ = content_split.get();
    bind_split_pane(content_split_);
    content_split_->set_on_first_size_changed([this](int /*first*/) {
        persist_split_size_as_pct(content_split_, model_.layout.bottom_pct, false, true);
        if (!divider_drag_active_) {
            model_.status_message = "Bottom " + std::to_string(model_.layout.bottom_pct) + "%";
            if (status_bar_ != nullptr) {
                status_bar_->set_text(format_status_bar_text());
            }
        }
    });
    auto status = std::make_unique<tuinator::StatusBar>(format_status_bar_text(), dap_theme_.status_bar);
    status_bar_ = status.get();

    auto root = std::make_unique<DebugChromeRoot>(app_.get(), this, std::move(controls), std::move(content_split),
                                                  std::move(status), dap_theme_.panel_background);

    app_->set_root(std::move(root));
    sync_ui_from_model();
    sync_controls_bar();
    sync_breakpoints_list_panel();
    if (const std::string preview_path = preferred_program_source_path(program_path_); !preview_path.empty()) {
        open_source_file(preview_path, 1, false);
        if (!launch_posted_) {
            prefetch_program_source_highlight();
        }
    }
    refresh_scroll_views();
    restore_watch_input_state();
    restore_breakpoint_input_state();
    restore_scope_input_state();
    request_full_screen_refresh();
}

void DebugApp::refresh_scroll_views() {
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
    refresh(source_scroll_view_);
    refresh(console_scroll_view_);
}

void DebugApp::maybe_start_launch() {
    if (!terminal_ready_for_session_ || launch_complete_handled_ || program_path_.empty()) {
        return;
    }
    if (launch_posted_) {
        return;
    }
    launch_posted_ = true;
    prefetch_program_source_highlight();
    session_io_->start_launch(program_path_, program_args_);
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
    const char* adapter_label = "debugpy";
    if (mode_ == SessionMode::Mock) {
        adapter_label = "mock session";
    } else if (adapter_ == DebugAdapter::Lldb) {
        adapter_label = "lldb-dap";
    } else if (adapter_ == DebugAdapter::Rr) {
        adapter_label = "rr replay";
    }
    const std::string message = std::string("Connecting to ") + adapter_label + "… "
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
        maybe_request_source_highlight();
        normalize_breakpoint_path_keys();
        apply_exception_filter_defaults();
        if (is_session_stopped()) {
            flush_breakpoints_to_session();
            breakpoints_flushed_after_launch_ = true;
            maybe_request_scope_variables();
            resolve_watches_from_locals();
        } else if (model_.session_state == "exited" || model_.session_state == "Exited") {
            model_.status_message =
                "Program exited before stopping — press Restart, or rebuild: "
                "gcc -g -O0 -o fixtures/reverse_demo fixtures/reverse_demo.c";
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
    if (adapter_ == DebugAdapter::Lldb) {
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
        sync_execution_location_ui();
        record_breakpoint_hit();
        maybe_finish_ephemeral_catch_skip();
        refresh_breakpoint_hit_counts_from_session();
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
    cached_scope_rows_ =
        build_scope_rows(model_, expanded_scope_paths_, pending_scope_paths_, cached_scope_row_meta_);
    apply_scope_value_overrides();
    sync_scopes_list_panel();
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

void DebugApp::toggle_scope_row_expand(int row_index) {
    if (scopes_panel_ != nullptr && scopes_panel_->has_active_inline_edit()) {
        return;
    }
    if (row_index < 0 || row_index >= static_cast<int>(cached_scope_row_meta_.size())) {
        return;
    }

    const ScopeVariableRowMeta& row_meta = cached_scope_row_meta_[static_cast<std::size_t>(row_index)];
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
    case SessionIoEventKind::SourceReady:
        if (event.success && event.detail == pending_source_fetch_key_) {
            cached_source_text_ = event.payload;
            pending_source_fetch_key_.clear();
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
        if (!event.detail.empty()) {
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
    if (scopes_panel_ != nullptr && !variable_set_in_flight_) {
        std::vector<ScopeVariableRowMeta> meta;
        std::vector<std::string> scope_rows =
            build_scope_rows(model_, expanded_scope_paths_, pending_scope_paths_, meta);
        const bool next_has_values = scope_rows_include_variables(scope_rows);
        const bool cached_has_values = scope_rows_include_variables(cached_scope_rows_);
        const bool keep_stale_values = cached_has_values && !next_has_values &&
                                       (scope_variables_fetch_pending_ || !is_session_stopped() ||
                                        !model_.scope_variables.empty());

        if (!keep_stale_values && scope_rows != cached_scope_rows_) {
            cached_scope_rows_ = std::move(scope_rows);
            cached_scope_row_meta_ = std::move(meta);
            apply_scope_value_overrides();
            sync_scopes_list_panel();
        }
    }
    if (stacks_panel_ != nullptr) {
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
            model_.stopped_thread_id > 0 &&
            (is_session_stopped() || model_.session_state == "running");

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

        stacks_panel_->set_thread_stacks(std::move(thread_contents));
    }
    if (is_session_stopped() && !model_.watches.empty()) {
        resolve_watches_from_locals();
    } else {
        sync_watches_panel();
    }
    if (source_section_ != nullptr) {
        const std::string title = panel_title_from_path(model_.source_path);
        if (title != cached_source_title_) {
            cached_source_title_ = title;
            source_section_->set_title(title);
        }
    }
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
    if (source_panel_ != nullptr) {
        return source_panel_->viewport_height();
    }
    if (source_scroll_view_ != nullptr && source_scroll_view_->bounds().height > 0) {
        return source_scroll_view_->bounds().height;
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
    cached_scope_rows_.clear();
    cached_scope_row_meta_.clear();
    expanded_scope_paths_.clear();
    pending_scope_paths_.clear();
}

void DebugApp::request_scope_variables_refresh() {
    scope_variables_signature_.clear();
    scope_variables_fetch_signature_.clear();
    scope_variables_fetch_pending_ = false;
    maybe_request_scope_variables();
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

    patch_scope_row_value(cached_scope_rows_, name, value);
    sync_scopes_list_panel();
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
    for (const auto& [name, value] : scope_value_overrides_) {
        patch_scope_row_value(cached_scope_rows_, name, value);
    }
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
            highlight_request_first_line_ = -1;
            cached_highlight_first_line_ = -1;
            maybe_request_source_highlight();
            return;
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
        if (!divider_drag_active_) {
            request_full_screen_refresh();
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
        const int scroll_y = std::max(0, line - viewport / 2 - 1);
        source_scroll_view_->scroll_to(0, scroll_y);
        source_scroll_view_->refresh_content();
        cached_highlight_scroll_y_ = scroll_y;
    } else {
        source_panel_->ensure_cursor_visible();
    }
}

void DebugApp::maybe_refresh_source_highlight_for_scroll() {
    if (divider_drag_active_ || source_scroll_view_ == nullptr || !uses_full_file_source()) {
        return;
    }

    const int scroll_y = source_scroll_view_->scroll_y();
    if (scroll_y == cached_highlight_scroll_y_) {
        return;
    }

    cached_highlight_scroll_y_ = scroll_y;
    cached_highlight_first_line_ = -1;
    highlight_request_first_line_ = -1;
    highlight_request_scroll_y_ = -1;

    if (source_panel_ != nullptr && !cached_source_text_.empty()) {
        const int first_line = std::max(1, scroll_y + 1);
        const int line_count = highlight_line_count();
        source_panel_->reset_plain_spans_for_line_range(first_line, line_count, cached_source_text_);
    }

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
    if (divider_drag_active_ || source_panel_ == nullptr) {
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
    if (reverse_continue_hint_shown_ || mode_ == SessionMode::Mock || adapter_ != DebugAdapter::Lldb ||
        model_.supports_step_back || !is_session_stopped()) {
        return;
    }

    reverse_continue_hint_shown_ = true;
    model_.status_message =
        "Reverse continue unavailable on this system (LLDB trace). Step Over/Into/Out still work.";
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

    split->set_on_screen_refresh([this]() { request_full_screen_refresh(); });
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

    app_->present();
}

void DebugApp::request_repaint() {
    if (!ui_built_) {
        return;
    }

    mark_all_panels_dirty();
}

void DebugApp::on_split_drag_ended() {
    apply_focus();
    refresh_scroll_views();
    cached_highlight_first_line_ = -1;
    cached_highlight_line_count_ = -1;
    highlight_request_first_line_ = -1;
    highlight_request_line_count_ = -1;

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
    maybe_request_source_highlight();
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

bool DebugApp::handle_layout_resize_key(const tuinator::KeyPress& key) {
    if (!key.alt) {
        if (key.ctrl && model_.focus == Focus::Scopes) {
            if (key.key == tuinator::Key::Up) {
                model_.layout.grow_scopes();
                model_.status_message = "Scopes pane enlarged";
                build_ui();
                return true;
            }
            if (key.key == tuinator::Key::Down) {
                model_.layout.shrink_scopes();
                model_.status_message = "Scopes pane shrunk";
                build_ui();
                return true;
            }
        }
        return false;
    }

    switch (key.key) {
    case tuinator::Key::Left:
        model_.layout.narrow_sidebar();
        model_.status_message = "Sidebar " + std::to_string(model_.layout.sidebar_pct) + "%";
        build_ui();
        return true;
    case tuinator::Key::Right:
        model_.layout.widen_sidebar();
        model_.status_message = "Sidebar " + std::to_string(model_.layout.sidebar_pct) + "%";
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

    if (key.character == '[') {
        if (model_.focus == Focus::Watches) {
            model_.layout.narrow_watches();
            model_.status_message = "Watches " + std::to_string(model_.layout.watches_pct) + "%";
        } else {
            model_.layout.narrow_repl();
            model_.status_message = "REPL " + std::to_string(model_.layout.repl_pct) + "%";
        }
        build_ui();
        return true;
    }
    if (key.character == ']') {
        if (model_.focus == Focus::Watches) {
            model_.layout.widen_watches();
            model_.status_message = "Watches " + std::to_string(model_.layout.watches_pct) + "%";
        } else {
            model_.layout.widen_repl();
            model_.status_message = "REPL " + std::to_string(model_.layout.repl_pct) + "%";
        }
        build_ui();
        return true;
    }

    return false;
}

bool DebugApp::is_watch_input_focused() const {
    return watches_panel_ != nullptr && watches_panel_->input_widget() != nullptr &&
           watches_panel_->input_widget()->is_focused();
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

bool DebugApp::should_block_app_quit_key(const tuinator::KeyPress& key) const {
    if (step_in_selection_active()) {
        return true;
    }
    if (context_menu_open()) {
        return true;
    }
    if (is_watch_input_focused() || is_repl_input_focused() || is_breakpoint_input_focused() ||
        is_scope_input_focused()) {
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
    if (watches_panel_ == nullptr) {
        return;
    }
    if (watches_panel_->input_widget() != nullptr) {
        watches_panel_->input_widget()->set_value("");
        watches_panel_->input_widget()->set_focused(false);
    }
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

bool DebugApp::handle_global_key(const tuinator::KeyPress& key) {
    if (handle_step_in_selection_key(key)) {
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
        is_scope_input_focused()) {
        if (key.alt && handle_layout_resize_key(key)) {
            return true;
        }
        return false;
    }

    if (handle_layout_resize_key(key)) {
        return true;
    }

    if (key.ctrl || key.alt) {
        return false;
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

    if (key.key == tuinator::Key::Tab) {
        cycle_focus_next();
        return true;
    }

    if (key.character == 'r' || key.character == 'R') {
        model_.focus = Focus::Repl;
        repl_input_focused_ = true;
        apply_focus();
        if (repl_panel_ != nullptr) {
            repl_panel_->focus_input();
        }
        model_.status_message = "REPL — type expression, Enter to evaluate";
        sync_status_bar();
        return true;
    }

    if (key.character == 'f') {
        follow_execution_ = !follow_execution_;
        if (follow_execution_) {
            maybe_follow_execution();
        }
        model_.status_message = follow_execution_ ? "Follow execution on" : "Follow execution off";
        sync_status_bar();
        return true;
    }

    if (is_breakpoint_input_focused() || is_scope_input_focused()) {
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

    if ((key.character == 'b' || key.character == ' ') && !key.ctrl && !key.alt && source_panel_ != nullptr &&
        model_.focus == Focus::Source) {
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
            const int index = watches_panel_->selected_index();
            if (index >= 0) {
                remove_watch_at(static_cast<std::size_t>(index));
            }
            return true;
        }
        if (key.character == 'w' || key.key == tuinator::Key::Enter) {
            watch_input_focused_ = true;
            watches_panel_->focus_input();
            return true;
        }
    }

    if (!has_active_session()) {
        return false;
    }

    if (key.character == 'c') {
        send_command("continue");
        return true;
    }
    if (key.character == 'n') {
        send_command("step_over");
        return true;
    }
    if (key.character == 'i') {
        send_command("step_into");
        return true;
    }
    if (key.character == 'u') {
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

    std::vector<tuinator::Widget*> widgets;
    if (source_panel_ != nullptr) {
        widgets.push_back(source_panel_);
    }
    if (scopes_panel_ != nullptr && scopes_panel_->list_widget() != nullptr) {
        widgets.push_back(scopes_panel_->list_widget());
    }
    if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr) {
        widgets.push_back(stacks_panel_->list_widget());
    }
    if (breakpoints_panel_ != nullptr && breakpoints_panel_->list_widget() != nullptr) {
        widgets.push_back(breakpoints_panel_->list_widget());
    }
    if (watches_panel_ != nullptr) {
        if (watches_panel_->list_widget() != nullptr) {
            widgets.push_back(watches_panel_->list_widget());
        }
        if (watches_panel_->input_widget() != nullptr) {
            widgets.push_back(watches_panel_->input_widget());
        }
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
        if (watch_input_focused_ && watches_panel_ != nullptr && watches_panel_->input_widget() != nullptr) {
            target = watches_panel_->input_widget();
        } else {
            target = watches_panel_ != nullptr ? watches_panel_->list_widget() : nullptr;
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
    } else if (watches_panel_ != nullptr && watches_panel_->input_widget() != nullptr &&
        watches_panel_->input_widget()->is_focused()) {
        detected = Focus::Watches;
        watch_input_focused_ = true;
    } else if (watches_panel_ != nullptr && watches_panel_->list_widget() != nullptr &&
               watches_panel_->list_widget()->is_focused()) {
        detected = Focus::Watches;
        watch_input_focused_ = false;
    } else if (scopes_panel_ != nullptr && scopes_panel_->list_widget() != nullptr &&
               scopes_panel_->list_widget()->is_focused()) {
        detected = Focus::Scopes;
        scope_input_focused_ = scopes_panel_->has_inline_edit();
    } else if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr &&
               stacks_panel_->list_widget()->is_focused()) {
        detected = Focus::Stacks;
    } else if (repl_panel_ != nullptr && repl_panel_->input_widget() != nullptr &&
               repl_panel_->input_widget()->is_focused()) {
        detected = Focus::Repl;
        repl_input_focused_ = repl_panel_->input_active();
    } else if (repl_panel_ != nullptr && repl_panel_->shell_widget() != nullptr &&
               repl_panel_->shell_widget()->is_focused()) {
        detected = Focus::Repl;
        repl_input_focused_ = repl_panel_->input_active();
    } else if (repl_panel_ != nullptr && repl_panel_->history_widget() != nullptr &&
               repl_panel_->history_widget()->is_focused()) {
        detected = Focus::Repl;
        repl_input_focused_ = false;
    } else if (console_panel_ != nullptr && console_panel_->is_focused()) {
        detected = Focus::Console;
    } else if (source_panel_ != nullptr && source_panel_->is_focused()) {
        detected = Focus::Source;
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
        source_scroll_view_->refresh_content();
        source_scroll_view_->mark_dirty();
    }
    if (scopes_panel_ != nullptr && scopes_panel_->list_widget() != nullptr) {
        scopes_panel_->list_widget()->mark_dirty();
    }
    refresh_scroll_views();
    if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr) {
        stacks_panel_->list_widget()->mark_dirty();
    }
    if (breakpoints_panel_ != nullptr && breakpoints_panel_->list_widget() != nullptr) {
        breakpoints_panel_->list_widget()->mark_dirty();
    }
    if (watches_panel_ != nullptr) {
        if (watches_panel_->list_widget() != nullptr) {
            watches_panel_->list_widget()->mark_dirty();
        }
        if (watches_panel_->input_widget() != nullptr) {
            watches_panel_->input_widget()->mark_dirty();
        }
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
    const bool same_view =
        viewing_same_source(model_.source_path, model_.source_reference, normalized_display_path, source_reference) &&
        source_panel_ != nullptr && line > 0 && source_panel_->cursor_line() == line &&
        !source_panel_->lines().empty();
    if (same_view) {
        if (source_panel_ != nullptr && is_session_stopped() && model_.execution_line > 0) {
            source_panel_->set_execution_line(static_cast<int>(model_.execution_line));
        }
        return;
    }

    model_.source_path = normalized_display_path;
    model_.source_reference = source_reference;

    const bool cache_hit =
        cached_source_path_ == cache_key && cached_source_reference_ == source_reference;
    if (!cache_hit) {
        cached_source_path_ = cache_key;
        cached_source_reference_ = source_reference;
        cached_source_text_ = disk_path.empty() ? std::string{} : read_file_or_empty(disk_path);
        cached_highlight_first_line_ = -1;
        cached_highlight_line_count_ = -1;
        highlight_request_first_line_ = -1;
        highlight_request_line_count_ = -1;
        if (source_panel_ != nullptr) {
            source_panel_->set_file_line_count(std::max(1, count_file_lines(cached_source_text_)));
            source_panel_->set_lines({});
        }
        cached_highlight_scroll_y_ = -1;
    }

    if (cached_source_text_.empty() && source_reference > 0 && session_io_ != nullptr &&
        session_io_->is_active()) {
        pending_source_fetch_key_ = cache_key;
        session_io_->request_source_fetch(source_reference, cache_key);
    }

    if (source_panel_ != nullptr && line > 0) {
        source_panel_->set_cursor_line(line);
    }

    if (source_section_ != nullptr) {
        const std::string title = panel_title_from_path(model_.source_path);
        if (title != cached_source_title_) {
            cached_source_title_ = title;
            source_section_->set_title(title);
        }
    }

    sync_breakpoints_to_panel();

    if (!cached_source_text_.empty() && source_panel_ != nullptr && source_panel_->lines().empty()) {
        if (uses_full_file_source()) {
            ensure_source_plain_lines();
        } else {
            const int line_count = std::max(1, source_viewport_height());
            const int first_line = std::max(1, line - line_count / 2);
            apply_instant_source_viewport(first_line, line_count);
        }
    }

    if (line > 0) {
        scroll_source_to_line(line);
    }

    cached_highlight_first_line_ = -1;
    highlight_request_first_line_ = -1;
    maybe_request_source_highlight();

    if (source_panel_ != nullptr) {
        source_panel_->mark_dirty();
    }
    if (source_scroll_view_ != nullptr) {
        source_scroll_view_->mark_dirty();
    }
    sync_status_bar();
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

void DebugApp::sync_breakpoints_list_panel() {
    if (breakpoints_panel_ == nullptr) {
        return;
    }

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

    breakpoints_panel_->set_breakpoints(std::move(rows));
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
    return model_.supports_function_breakpoints || adapter_ == DebugAdapter::Lldb;
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
    if (context_menu_ == nullptr || row_index < 0 || row_index >= static_cast<int>(cached_scope_row_meta_.size())) {
        return;
    }

    const ScopeVariableRowMeta& row_meta = cached_scope_row_meta_[static_cast<std::size_t>(row_index)];
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
    std::unordered_map<int, std::string> breakpoints;
    const auto it = breakpoints_by_path_.find(path);
    if (it == breakpoints_by_path_.end()) {
        const auto normalized_it = breakpoints_by_path_.find(normalized);
        if (normalized_it != breakpoints_by_path_.end()) {
            for (const auto& [line, info] : normalized_it->second) {
                breakpoints[line] = info.condition;
            }
        }
    } else {
        for (const auto& [line, info] : it->second) {
            breakpoints[line] = info.condition;
        }
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
        std::unordered_map<int, std::string> breakpoints;
        const auto current = breakpoints_by_path_.find(normalized);
        if (current != breakpoints_by_path_.end()) {
            for (const auto& [bp_line, info] : current->second) {
                breakpoints[bp_line] = info.condition;
            }
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
    std::unordered_map<int, std::string> source_breakpoints;
    for (const auto& [bp_line, info] : breakpoints) {
        source_breakpoints[bp_line] = info.condition;
    }
    source_panel_->set_breakpoints(std::move(source_breakpoints));

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
    if (scopes_panel_ == nullptr) {
        return;
    }
    std::vector<bool> show_edit;
    show_edit.reserve(cached_scope_row_meta_.size());
    for (const ScopeVariableRowMeta& row : cached_scope_row_meta_) {
        show_edit.push_back(row.show_edit);
    }
    scopes_panel_->set_scope_names(cached_scope_rows_, std::move(show_edit));
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

bool DebugApp::breakpoint_prompt_active() const {
    return breakpoint_input_focused_ &&
           ((!editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0) || !editing_exception_filter_.empty());
}

bool DebugApp::overlay_intercepts_events() const {
    return context_menu_open();
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
        return handled || static_cast<bool>(pending_action);
    }

    return false;
}

void DebugApp::paint_overlay(tuinator::PaintContext& ctx) const {
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

    for (const tui_debug_ui::ScopeVariableRowMeta& row : cached_scope_row_meta_) {
        if (row.variable_name == name && row.container_reference > 0) {
            return row.container_reference;
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

void DebugApp::begin_watch_expression(const std::string& seed) {
    editing_watch_index_ = -1;
    watch_input_draft_ = seed;
    watch_input_focused_ = true;
    model_.focus = Focus::Watches;

    if (watches_panel_ != nullptr) {
        watches_panel_->set_input_value(seed);
        watches_panel_->focus_input();
    }

    model_.status_message = seed.empty() ? "Add watch expression" : "Watch expression";
    apply_focus();
    sync_status_bar();
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
        adapter_ != DebugAdapter::Rr && is_session_stopped() && session_io_ != nullptr && session_io_->is_active();

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
        goto_targets.empty() && adapter_ == DebugAdapter::Lldb && pending.line > 0 &&
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
    watch_input_draft_ = watches_panel_->input_value();
    if (watches_panel_->input_widget() != nullptr && watches_panel_->input_widget()->is_focused()) {
        watch_input_focused_ = true;
        model_.focus = Focus::Watches;
    }
}

void DebugApp::restore_watch_input_state() {
    if (watches_panel_ == nullptr) {
        return;
    }
    if (!watch_input_draft_.empty()) {
        watches_panel_->set_input_value(watch_input_draft_);
    }
    if (!watch_input_focused_) {
        apply_focus();
        return;
    }
    model_.focus = Focus::Watches;
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
    if (watches_panel_ == nullptr) {
        return;
    }

    std::vector<std::string> lines;
    lines.reserve(model_.watches.size());
    for (const WatchEntry& watch : model_.watches) {
        if (!watch.error.empty()) {
            lines.push_back(watch.expression + " = <error: " + watch.error + ">");
        } else if (watch.value.empty()) {
            lines.push_back(watch.expression + " = ?");
        } else {
            lines.push_back(watch.expression + " = " + watch.value);
        }
    }
    watches_panel_->set_lines(std::move(lines));
}

void DebugApp::begin_edit_watch_at(int index) {
    if (watches_panel_ == nullptr || index < 0 || index >= static_cast<int>(model_.watches.size())) {
        return;
    }

    editing_watch_index_ = index;
    watch_input_draft_ = model_.watches[static_cast<std::size_t>(index)].expression;
    watch_input_focused_ = true;
    model_.focus = Focus::Watches;
    watches_panel_->set_input_value(watch_input_draft_);
    watches_panel_->focus_input();
    model_.status_message = "Edit watch expression";
    apply_focus();
    sync_status_bar();
}

void DebugApp::submit_watch_expression(const std::string& expression) {
    std::string trimmed = expression;
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.front()))) {
        trimmed.erase(trimmed.begin());
    }
    while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) {
        trimmed.pop_back();
    }

    if (editing_watch_index_ >= 0 && editing_watch_index_ < static_cast<int>(model_.watches.size())) {
        if (trimmed.empty()) {
            editing_watch_index_ = -1;
            finish_watch_input();
            return;
        }

        WatchEntry& watch = model_.watches[static_cast<std::size_t>(editing_watch_index_)];
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
    model_.watches.push_back(std::move(entry));

    finish_watch_input();
    sync_watches_panel();
    model_.status_message = "Added watch";
    sync_status_bar();

    resolve_watches_from_locals();
}

void DebugApp::remove_watch_at(std::size_t index) {
    if (index >= model_.watches.size()) {
        return;
    }
    if (editing_watch_index_ == static_cast<int>(index)) {
        editing_watch_index_ = -1;
        watch_input_draft_.clear();
        watch_input_focused_ = false;
        if (watches_panel_ != nullptr) {
            watches_panel_->set_input_value("");
        }
    } else if (editing_watch_index_ > static_cast<int>(index)) {
        --editing_watch_index_;
    }
    model_.watches.erase(model_.watches.begin() + static_cast<std::ptrdiff_t>(index));
    sync_watches_panel();
    model_.status_message = "Removed watch";
    sync_status_bar();
}

void DebugApp::resolve_watches_from_locals() {
    if (!launch_complete_handled_ || model_.watches.empty() || !is_session_stopped()) {
        return;
    }

    const bool locals_visible = !model_.variables.empty() || !model_.scope_variables.empty() ||
                                scope_rows_include_variables(cached_scope_rows_);

    bool changed = false;
    for (WatchEntry& watch : model_.watches) {
        watch.expression = normalize_watch_expression(watch.expression);

        std::optional<std::string> resolved = try_resolve_watch_from_model(model_, watch.expression);
        if (!resolved) {
            resolved = try_resolve_watch_from_scope_rows(cached_scope_rows_, watch.expression);
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

    if (changed) {
        sync_watches_panel();
    }
}

}  // namespace tui_debug_ui
