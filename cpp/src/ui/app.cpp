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

std::optional<std::string> try_resolve_watch_from_scope_rows(const std::vector<std::string>& rows,
                                                             const std::string& expression) {
    const std::string key = normalize_watch_expression(expression);
    if (!is_simple_watch_identifier(key)) {
        return std::nullopt;
    }

    const std::string prefix = "  " + key + " = ";
    for (const std::string& row : rows) {
        if (row.rfind(prefix, 0) == 0) {
            return row.substr(prefix.size());
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
        if (row.size() >= 2 && row[0] == ' ' && row[1] == ' ') {
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
    for (const tui_debug_ui::ScopeInfo& scope : model.scopes) {
        const auto vars_it = model.scope_variables.find(scope.variables_reference);
        if (vars_it == model.scope_variables.end()) {
            continue;
        }
        for (const tui_debug_ui::VariableInfo& variable : vars_it->second) {
            if (variable.name == name) {
                return VariableEditTarget{scope.variables_reference, variable.value};
            }
        }
    }
    return std::nullopt;
}

std::vector<std::string> build_scope_rows(const tui_debug_ui::DebugUiModel& model) {
    std::vector<std::string> scope_rows;
    for (const tui_debug_ui::ScopeInfo& scope : model.scopes) {
        scope_rows.push_back(scope.name + ":");
        const auto vars_it = model.scope_variables.find(scope.variables_reference);
        if (vars_it != model.scope_variables.end()) {
            for (const tui_debug_ui::VariableInfo& variable : vars_it->second) {
                scope_rows.push_back("  " + variable.name + " = " + variable.value);
            }
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

            // Status row before content so the bottom chrome row is not shadowed by content hit tests.
            bool handled = false;
            for (tuinator::Widget* child : {controls_.get(), status_.get(), content_.get()}) {
                if (child != nullptr && child->bounds().contains(mouse->position) && child->handle_event(event)) {
                    handled = true;
                }
            }
            if (handled && (mouse->action == tuinator::MouseAction::Click ||
                            mouse->action == tuinator::MouseAction::Release) &&
                debug_app_ != nullptr) {
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

int watches_first_size(int terminal_width, std::uint16_t watches_pct) {
    const int pct = static_cast<int>(watches_pct);
    return std::max(16, terminal_width * pct / 100);
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
    if (ext == "js" || ext == "ts") {
        return "javascript";
    }
    return ext;
}

}  // namespace

namespace tui_debug_ui {

DebugApp::DebugApp(const std::string& program_path, SessionMode mode, DebugAdapter adapter)
    : mode_(mode),
      adapter_(adapter),
      program_path_(program_path),
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
    if (!scope_input_draft_.empty()) {
        scopes_panel_->set_input_value(scope_input_draft_);
    }
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
    breakpoints_panel_->set_on_activate([this](const BreakpointRow& row) {
        open_source_file(row.path, row.line, true);
        model_.status_message =
            "Opened " + panel_title_from_path(row.path) + ":" + std::to_string(row.line);
        sync_status_bar();
    });
    breakpoints_panel_->set_on_remove([this](const BreakpointRow& row) { remove_breakpoint_at(row.path, row.line); });
    breakpoints_panel_->set_on_add_condition([this](const BreakpointRow& row) {
        begin_edit_breakpoint_condition(row.path, row.line);
    });
    breakpoints_panel_->set_on_submit([this](const std::string& condition) { submit_breakpoint_condition(condition); });
    breakpoints_panel_->set_on_change([this](const std::string& condition) {
        breakpoint_input_draft_ = condition;
        breakpoint_input_focused_ = true;
        model_.focus = Focus::Breakpoints;
    });
    if (!breakpoint_input_draft_.empty()) {
        breakpoints_panel_->set_input_value(breakpoint_input_draft_);
    }

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

    auto sidebar = std::make_unique<ResizableSplitPane>(
        std::move(scopes_widget), std::move(stacks_breakpoints),
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
    breakpoint_prompt_input_ = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.placeholder = "> condition (when)"}, dap_theme_.label, dap_theme_.selection);
    breakpoint_prompt_input_->set_on_submit([this](const std::string& value) { submit_breakpoint_condition(value); });
    breakpoint_prompt_input_->set_on_change([this](const std::string& value) {
        breakpoint_input_draft_ = value;
        breakpoint_input_focused_ = true;
        if (breakpoints_panel_ != nullptr) {
            breakpoints_panel_->set_input_value(value);
        }
    });

    source_panel_->set_on_toggle_breakpoint([this](int line) { toggle_breakpoint_at_line(line); });
    source_panel_->set_on_breakpoint_context([this](int line, int code_column, tuinator::Point anchor) {
        const std::string path = effective_source_path();
        std::optional<std::string> seed;
        if (!path.empty()) {
            if (cached_source_path_ != path) {
                cached_source_path_ = path;
                cached_source_text_ = read_file_or_empty(path);
            }
            const std::string line_text =
                cached_source_text_.empty() ? highlighted_line_text(source_panel_, line)
                                            : line_text_at(cached_source_text_, line);
            seed = identifier_at_line_column(line_text, code_column);
        }
        begin_source_context_menu(goto_probe_source_path(), line, code_column, anchor, seed);
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

    auto watches_widget = watches_panel_->release_widget();
    watches_widget->set_flex(0);

    auto console_panel = std::make_unique<ConsolePanel>(dap_theme_);
    console_panel_ = console_panel.get();

    auto console_section = std::make_unique<TitledScrollPane>("Console", std::move(console_panel),
                                                              dap_theme_.title_console, dap_theme_.panel_background,
                                                              scroll_options);
    console_scroll_view_ = console_section->scroll_view();
    auto console_shell = console_section->release_widget();
    console_shell->set_flex(1);

    auto bottom_tray = std::make_unique<ResizableSplitPane>(
        std::move(watches_widget), std::move(console_shell),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Horizontal,
            .first_size = watches_first_size(term_size.width, model_.layout.watches_pct),
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    bottom_tray_split_ = bottom_tray.get();
    bind_split_pane(bottom_tray_split_);
    bottom_tray_split_->set_on_first_size_changed([this](int /*first*/) {
        persist_split_size_as_pct(bottom_tray_split_, model_.layout.watches_pct, true);
        if (!divider_drag_active_) {
            model_.status_message = "Watches " + std::to_string(model_.layout.watches_pct) + "%";
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
    session_io_->start_launch(program_path_);
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
    if (is_session_stopped()) {
        for (const ScopeInfo& scope : model_.scopes) {
            if (scope.variables_reference > 0 && is_locals_scope_name(scope.name) && !model_.variables.empty()) {
                model_.scope_variables[scope.variables_reference] = model_.variables;
            }
        }
    }
    if (!model_.execution_path.empty() && model_.execution_path.rfind("dap:source:", 0) != 0) {
        const std::string resolved = resolve_debugger_source_path(model_.execution_path, program_path_);
        if (!resolved.empty()) {
            model_.execution_path = resolved;
        }
    }
    ++snapshot_generation_;
    scope_variables_fetch_pending_ = false;
    scope_variables_fetch_signature_.clear();
    scope_variables_signature_.clear();
    if (const std::string preview_path = preferred_program_source_path(program_path_);
        model_.source_path.empty() && !preview_path.empty() && model_.execution_path.empty()) {
        open_source_file(preview_path, 1, false);
    }
    apply_breakpoint_hits_from_snapshot_json(json);
    if (is_session_stopped()) {
        maybe_follow_execution();
        record_breakpoint_hit();
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
        const std::vector<ConsoleLine> new_entries(model_.console_lines.begin() + static_cast<std::ptrdiff_t>(before),
                                                   model_.console_lines.end());
        console_panel_->append_lines(format_console_display_lines(new_entries));
        if (console_scroll_view_ != nullptr) {
            console_scroll_view_->refresh_content();
            console_scroll_view_->scroll_to(0, console_scroll_view_->max_scroll_y());
        }
    }
}

void DebugApp::apply_scope_variables_payload(const std::string& signature, const std::string& json) {
    if (!apply_scope_variables_batch(model_, signature, json)) {
        scope_variables_fetch_pending_ = false;
        return;
    }
    scope_variables_signature_ = signature;
    scope_variables_fetch_signature_ = signature;
    scope_variables_fetch_pending_ = false;
    sync_ui_from_model();
    resolve_watches_from_locals();
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
            model_.status_message = event.detail.empty() ? "Command failed" : event.detail;
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
        }
        break;
    case SessionIoEventKind::EvaluateFinished:
        if (is_session_stopped()) {
            resolve_watches_from_locals();
        } else {
            sync_watches_panel();
        }
        break;
    case SessionIoEventKind::SetVariableFinished:
        if (event.success) {
            if (!event.detail.empty() && !pending_variable_value_.empty()) {
                patch_local_variable_value(event.detail, pending_variable_value_);
            }
            request_scope_variables_refresh();
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
            if (scopes_panel_->input_widget() != nullptr) {
                scopes_panel_->input_widget()->set_value("");
                scopes_panel_->input_widget()->set_focused(false);
            }
            if (scopes_panel_->list_widget() != nullptr) {
                scopes_panel_->list_widget()->set_focused(true);
            }
        }
        model_.focus = Focus::Scopes;
        apply_focus();
        sync_status_bar();
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

    if (model_.connection_state == ConnectionState::Connected) {
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
    if (scopes_panel_ != nullptr) {
        std::vector<std::string> scope_rows = build_scope_rows(model_);
        const bool next_has_values = scope_rows_include_variables(scope_rows);
        const bool cached_has_values = scope_rows_include_variables(cached_scope_rows_);
        const bool keep_stale_values =
            !next_has_values && cached_has_values &&
            (scope_variables_fetch_pending_ || !is_session_stopped() || !model_.scope_variables.empty());

        if (!keep_stale_values && scope_rows != cached_scope_rows_) {
            cached_scope_rows_ = scope_rows;
            scopes_panel_->set_scope_names(scope_rows);
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
    if (console_panel_ != nullptr && console_panel_->lines().empty() && !model_.console_lines.empty()) {
        console_panel_->append_lines(format_console_display_lines(model_.console_lines));
        if (console_scroll_view_ != nullptr) {
            console_scroll_view_->refresh_content();
            console_scroll_view_->scroll_to(0, console_scroll_view_->max_scroll_y());
        }
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

    cached_scope_rows_ = build_scope_rows(model_);
    if (scopes_panel_ != nullptr) {
        scopes_panel_->set_scope_names(cached_scope_rows_);
    }
}

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
        if (!is_locals_scope_name(scope.name) || scope.variables_reference <= 0) {
            continue;
        }
        scopes.emplace_back(scope.variables_reference, scope.name);
    }
    if (scopes.empty()) {
        for (const ScopeInfo& scope : model_.scopes) {
            if (scope.variables_reference > 0) {
                scopes.emplace_back(scope.variables_reference, scope.name);
                break;
            }
        }
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
        model_.layout.narrow_watches();
        model_.status_message = "Watches " + std::to_string(model_.layout.watches_pct) + "%";
        build_ui();
        return true;
    }
    if (key.character == ']') {
        model_.layout.widen_watches();
        model_.status_message = "Watches " + std::to_string(model_.layout.watches_pct) + "%";
        build_ui();
        return true;
    }

    return false;
}

bool DebugApp::is_watch_input_focused() const {
    return watches_panel_ != nullptr && watches_panel_->input_widget() != nullptr &&
           watches_panel_->input_widget()->is_focused();
}

bool DebugApp::is_breakpoint_input_focused() const {
    if (breakpoint_prompt_active() && breakpoint_prompt_input_ != nullptr &&
        breakpoint_prompt_input_->is_focused()) {
        return true;
    }
    return breakpoints_panel_ != nullptr && breakpoints_panel_->input_widget() != nullptr &&
           breakpoints_panel_->input_widget()->is_focused();
}

bool DebugApp::is_scope_input_focused() const {
    return scopes_panel_ != nullptr && scopes_panel_->input_widget() != nullptr &&
           scopes_panel_->input_widget()->is_focused();
}

bool DebugApp::should_block_app_quit_key(const tuinator::KeyPress& key) const {
    if (step_in_selection_active()) {
        return true;
    }
    if (context_menu_open()) {
        return true;
    }
    if (is_watch_input_focused() || is_breakpoint_input_focused() || is_scope_input_focused()) {
        return true;
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
    editing_variable_name_.clear();
    editing_variables_reference_ = 0;
    scope_input_draft_.clear();
    scope_input_focused_ = false;
    if (scopes_panel_ == nullptr) {
        return;
    }
    if (scopes_panel_->input_widget() != nullptr) {
        scopes_panel_->input_widget()->set_value("");
        scopes_panel_->input_widget()->set_focused(false);
    }
    if (scopes_panel_->list_widget() != nullptr) {
        scopes_panel_->list_widget()->set_focused(true);
    }
    model_.focus = Focus::Scopes;
}

bool DebugApp::handle_global_key(const tuinator::KeyPress& key) {
    if (handle_step_in_selection_key(key)) {
        return true;
    }

    if (key.ctrl && key.character == 'c') {
        if (app_ != nullptr) {
            app_->quit();
        }
        return true;
    }

    if (is_watch_input_focused() || is_breakpoint_input_focused() || is_scope_input_focused()) {
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
                std::optional<std::string> seed;
                if (cached_source_path_ != path) {
                    cached_source_path_ = path;
                    cached_source_text_ = read_file_or_empty(path);
                }
                const std::string line_text =
                    cached_source_text_.empty() ? highlighted_line_text(source_panel_, line)
                                                : line_text_at(cached_source_text_, line);
                seed = identifier_at_line_column(line_text, 0);
                show_breakpoint_context_menu(path, line, anchor, seed);
                return true;
            }
        } else if (model_.focus == Focus::Breakpoints && breakpoints_panel_ != nullptr) {
            if (const BreakpointRow* row = breakpoints_panel_->selected_row()) {
                tuinator::Point anchor{0, 0};
                if (breakpoints_panel_->list_widget() != nullptr) {
                    const tuinator::Rect list_bounds = breakpoints_panel_->list_widget()->bounds();
                    anchor = {list_bounds.x + 2, list_bounds.y + 2};
                }
                std::optional<std::string> seed;
                const int line_width = tuinator::text_display_width(row->source_text);
                for (int column = 0; column < line_width; ++column) {
                    seed = identifier_at_line_column(row->source_text, column);
                    if (seed.has_value()) {
                        break;
                    }
                }
                show_breakpoint_context_menu(row->path, row->line, anchor, seed);
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
                begin_edit_breakpoint_condition(path, line);
                return true;
            }
        } else if (model_.focus == Focus::Breakpoints && breakpoints_panel_ != nullptr) {
            if (const BreakpointRow* row = breakpoints_panel_->selected_row()) {
                begin_edit_breakpoint_condition(row->path, row->line);
                return true;
            }
        }
    }

    if (model_.focus == Focus::Breakpoints && breakpoints_panel_ != nullptr) {
        if (const BreakpointRow* row = breakpoints_panel_->selected_row()) {
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
    if (scopes_panel_ != nullptr) {
        if (scopes_panel_->list_widget() != nullptr) {
            widgets.push_back(scopes_panel_->list_widget());
        }
        if (scopes_panel_->input_widget() != nullptr) {
            widgets.push_back(scopes_panel_->input_widget());
        }
    }
    if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr) {
        widgets.push_back(stacks_panel_->list_widget());
    }
    if (breakpoints_panel_ != nullptr) {
        if (breakpoints_panel_->list_widget() != nullptr) {
            widgets.push_back(breakpoints_panel_->list_widget());
        }
        if (breakpoints_panel_->input_widget() != nullptr) {
            widgets.push_back(breakpoints_panel_->input_widget());
        }
    }
    if (watches_panel_ != nullptr) {
        if (watches_panel_->list_widget() != nullptr) {
            widgets.push_back(watches_panel_->list_widget());
        }
        if (watches_panel_->input_widget() != nullptr) {
            widgets.push_back(watches_panel_->input_widget());
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
        if (scope_input_focused_ && scopes_panel_ != nullptr && scopes_panel_->input_widget() != nullptr) {
            target = scopes_panel_->input_widget();
        } else {
            target = scopes_panel_ != nullptr ? scopes_panel_->list_widget() : nullptr;
        }
        break;
    case Focus::Stacks:
        target = stacks_panel_ != nullptr ? stacks_panel_->list_widget() : nullptr;
        break;
    case Focus::Breakpoints:
        if (breakpoint_input_focused_ && breakpoints_panel_ != nullptr &&
            breakpoints_panel_->input_widget() != nullptr) {
            target = breakpoints_panel_->input_widget();
        } else {
            target = breakpoints_panel_ != nullptr ? breakpoints_panel_->list_widget() : nullptr;
        }
        break;
    case Focus::Watches:
        if (watch_input_focused_ && watches_panel_ != nullptr && watches_panel_->input_widget() != nullptr) {
            target = watches_panel_->input_widget();
        } else {
            target = watches_panel_ != nullptr ? watches_panel_->list_widget() : nullptr;
        }
        break;
    case Focus::Console:
        target = console_panel_;
        break;
    default:
        target = source_panel_;
        break;
    }

    if (breakpoint_prompt_active() && breakpoint_prompt_input_ != nullptr) {
        target = breakpoint_prompt_input_.get();
    }

    if (target != nullptr) {
        target->set_focused(true);
    }
    mark_all_panels_dirty();
}

void DebugApp::sync_focus_from_ui() {
    Focus detected = model_.focus;

    if (breakpoints_panel_ != nullptr && breakpoints_panel_->input_widget() != nullptr &&
        breakpoints_panel_->input_widget()->is_focused()) {
        detected = Focus::Breakpoints;
        breakpoint_input_focused_ = true;
    } else if (breakpoints_panel_ != nullptr && breakpoints_panel_->list_widget() != nullptr &&
               breakpoints_panel_->list_widget()->is_focused()) {
        detected = Focus::Breakpoints;
        breakpoint_input_focused_ = false;
    } else if (watches_panel_ != nullptr && watches_panel_->input_widget() != nullptr &&
        watches_panel_->input_widget()->is_focused()) {
        detected = Focus::Watches;
        watch_input_focused_ = true;
    } else if (watches_panel_ != nullptr && watches_panel_->list_widget() != nullptr &&
               watches_panel_->list_widget()->is_focused()) {
        detected = Focus::Watches;
        watch_input_focused_ = false;
    } else if (scopes_panel_ != nullptr && scopes_panel_->input_widget() != nullptr &&
               scopes_panel_->input_widget()->is_focused()) {
        detected = Focus::Scopes;
        scope_input_focused_ = true;
    } else if (scopes_panel_ != nullptr && scopes_panel_->list_widget() != nullptr &&
               scopes_panel_->list_widget()->is_focused()) {
        detected = Focus::Scopes;
        scope_input_focused_ = false;
    } else if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr &&
               stacks_panel_->list_widget()->is_focused()) {
        detected = Focus::Stacks;
    } else if (console_panel_ != nullptr && console_panel_->is_focused()) {
        detected = Focus::Console;
    } else if (source_panel_ != nullptr && source_panel_->is_focused()) {
        detected = Focus::Source;
    }

    if (detected == model_.focus) {
        return;
    }

    model_.focus = detected;
    cached_status_bar_text_.clear();
    mark_all_panels_dirty();
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
    if (scopes_panel_ != nullptr) {
        if (scopes_panel_->list_widget() != nullptr) {
            scopes_panel_->list_widget()->mark_dirty();
        }
        if (scopes_panel_->input_widget() != nullptr) {
            scopes_panel_->input_widget()->mark_dirty();
        }
    }
    refresh_scroll_views();
    if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr) {
        stacks_panel_->list_widget()->mark_dirty();
    }
    if (breakpoints_panel_ != nullptr) {
        if (breakpoints_panel_->list_widget() != nullptr) {
            breakpoints_panel_->list_widget()->mark_dirty();
        }
        if (breakpoints_panel_->input_widget() != nullptr) {
            breakpoints_panel_->input_widget()->mark_dirty();
        }
    }
    if (watches_panel_ != nullptr) {
        if (watches_panel_->list_widget() != nullptr) {
            watches_panel_->list_widget()->mark_dirty();
        }
        if (watches_panel_->input_widget() != nullptr) {
            watches_panel_->input_widget()->mark_dirty();
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
    for (const auto& [path, breakpoints] : breakpoints_by_path_) {
        const std::string normalized = normalize_source_path(path);
        const std::string file_text = read_file_or_empty(normalized);
        for (const auto& [bp_line, info] : breakpoints) {
            rows.push_back(BreakpointRow{normalized, bp_line, trimmed_line_text_at(file_text, bp_line),
                                         info.condition, info.hit_condition, info.hit_count});
        }
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
}

void DebugApp::sync_breakpoints_to_panel() {
    if (source_panel_ == nullptr) {
        return;
    }

    const std::string path = effective_source_path();
    if (path.empty()) {
        return;
    }

    std::unordered_map<int, std::string> breakpoints;
    const auto it = breakpoints_by_path_.find(path);
    if (it != breakpoints_by_path_.end()) {
        for (const auto& [line, info] : it->second) {
            breakpoints[line] = info.condition;
        }
    }
    if (source_panel_->breakpoints() != breakpoints) {
        source_panel_->set_breakpoints(std::move(breakpoints));
    }
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

void DebugApp::begin_edit_breakpoint_condition(const std::string& path, int line) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    const auto it = breakpoints_by_path_.find(normalized);
    if (it == breakpoints_by_path_.end() || !it->second.contains(line)) {
        model_.status_message = "No breakpoint on this line";
        sync_status_bar();
        return;
    }

    editing_breakpoint_path_ = normalized;
    editing_breakpoint_line_ = line;
    editing_breakpoint_hit_ = false;
    breakpoint_input_draft_ = it->second.at(line).condition;
    breakpoint_input_focused_ = true;
    model_.focus = Focus::Breakpoints;

    sync_breakpoint_prompt();

    model_.status_message =
        "Condition for " + panel_title_from_path(normalized) + ":" + std::to_string(line) + " — type below, Enter to save";
    apply_focus();
    sync_status_bar();
    request_full_screen_refresh();
}

void DebugApp::begin_edit_breakpoint_hit_condition(const std::string& path, int line) {
    const std::string normalized = normalize_source_path(path);
    if (normalized.empty() || line <= 0) {
        return;
    }

    const auto it = breakpoints_by_path_.find(normalized);
    if (it == breakpoints_by_path_.end() || !it->second.contains(line)) {
        model_.status_message = "No breakpoint on this line";
        sync_status_bar();
        return;
    }

    editing_breakpoint_path_ = normalized;
    editing_breakpoint_line_ = line;
    editing_breakpoint_hit_ = true;
    breakpoint_input_draft_ = it->second.at(line).hit_condition;
    breakpoint_input_focused_ = true;
    model_.focus = Focus::Breakpoints;

    sync_breakpoint_prompt();

    model_.status_message = "Hit condition for " + panel_title_from_path(normalized) + ":" +
                            std::to_string(line) + " — type below, Enter to save";
    apply_focus();
    sync_status_bar();
    request_full_screen_refresh();
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
    breakpoint_input_draft_ = breakpoints_panel_->input_value();
    if (breakpoints_panel_->input_widget() != nullptr && breakpoints_panel_->input_widget()->is_focused()) {
        breakpoint_input_focused_ = true;
        model_.focus = Focus::Breakpoints;
    }
}

void DebugApp::restore_breakpoint_input_state() {
    if (breakpoints_panel_ == nullptr) {
        return;
    }
    if (!breakpoint_input_draft_.empty()) {
        breakpoints_panel_->set_input_value(breakpoint_input_draft_);
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
        scopes_panel_->set_input_value(scope_input_draft_);
        scopes_panel_->focus_input();
    }

    model_.status_message = "Edit " + variable_name;
    apply_focus();
    sync_status_bar();
}

void DebugApp::submit_variable_value(const std::string& value) {
    if (editing_variable_name_.empty() || editing_variables_reference_ <= 0) {
        scope_input_draft_.clear();
        scope_input_focused_ = false;
        if (scopes_panel_ != nullptr && scopes_panel_->input_widget() != nullptr) {
            scopes_panel_->input_widget()->set_value("");
        }
        return;
    }

    if (session_io_ == nullptr) {
        model_.status_message = "No debug session";
        sync_status_bar();
        return;
    }

    pending_variable_value_ = value;
    session_io_->post_set_variable(editing_variables_reference_, editing_variable_name_, value);
    model_.status_message = "Setting " + editing_variable_name_ + "…";
    sync_status_bar();
}

void DebugApp::capture_scope_input_state() {
    if (scopes_panel_ == nullptr) {
        return;
    }
    scope_input_draft_ = scopes_panel_->input_value();
    if (scopes_panel_->input_widget() != nullptr && scopes_panel_->input_widget()->is_focused()) {
        scope_input_focused_ = true;
        model_.focus = Focus::Scopes;
    }
}

void DebugApp::restore_scope_input_state() {
    if (scopes_panel_ == nullptr) {
        return;
    }
    if (!scope_input_draft_.empty()) {
        scopes_panel_->set_input_value(scope_input_draft_);
    }
    if (!scope_input_focused_) {
        return;
    }
    model_.focus = Focus::Scopes;
    scopes_panel_->focus_input();
}

bool DebugApp::context_menu_open() const {
    return context_menu_ != nullptr && context_menu_->is_open();
}

bool DebugApp::breakpoint_prompt_active() const {
    return breakpoint_input_focused_ && !editing_breakpoint_path_.empty() && editing_breakpoint_line_ > 0;
}

bool DebugApp::overlay_intercepts_events() const {
    return context_menu_open() || breakpoint_prompt_active();
}

tuinator::Rect DebugApp::breakpoint_prompt_bounds() const {
    constexpr int kStatusBarRows = 1;
    constexpr int kPromptRows = 1;
    const tuinator::Size term = app_ != nullptr ? app_->terminal_size() : tuinator::Size{80, 24};
    const int y = std::max(0, term.height - kStatusBarRows - kPromptRows);
    return {0, y, term.width, kPromptRows};
}

void DebugApp::layout_breakpoint_prompt() {
    if (breakpoint_prompt_input_ == nullptr) {
        return;
    }
    breakpoint_prompt_input_->layout(breakpoint_prompt_bounds());
}

void DebugApp::sync_breakpoint_prompt() {
    if (breakpoint_prompt_input_ == nullptr) {
        return;
    }
    breakpoint_prompt_input_->set_value(breakpoint_input_draft_);
    layout_breakpoint_prompt();
    breakpoint_prompt_input_->set_focused(true);
    if (breakpoints_panel_ != nullptr) {
        breakpoints_panel_->set_input_value(breakpoint_input_draft_);
    }
}

void DebugApp::blur_breakpoint_input(bool cancelled) {
    editing_breakpoint_path_.clear();
    editing_breakpoint_line_ = 0;
    editing_breakpoint_hit_ = false;
    breakpoint_input_draft_.clear();
    breakpoint_input_focused_ = false;
    if (breakpoint_prompt_input_ != nullptr) {
        breakpoint_prompt_input_->set_value("");
        breakpoint_prompt_input_->set_focused(false);
    }
    if (breakpoints_panel_ != nullptr) {
        breakpoints_panel_->set_input_value("");
        if (breakpoints_panel_->input_widget() != nullptr) {
            breakpoints_panel_->input_widget()->set_focused(false);
        }
        if (breakpoints_panel_->list_widget() != nullptr) {
            breakpoints_panel_->list_widget()->set_focused(true);
        }
    }
    apply_focus();
    if (cancelled) {
        model_.status_message = "Breakpoint edit cancelled";
    }
    sync_status_bar();
    request_full_screen_refresh();
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
            request_full_screen_refresh();
        }
        return handled || static_cast<bool>(pending_action);
    }

    if (!breakpoint_prompt_active() || breakpoint_prompt_input_ == nullptr) {
        return false;
    }

    layout_breakpoint_prompt();
    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (key->key == tuinator::Key::Escape) {
            blur_breakpoint_input();
            return true;
        }
    }

    const bool handled = breakpoint_prompt_input_->handle_event(event);
    if (handled) {
        request_full_screen_refresh();
    }
    return handled || breakpoint_prompt_active();
}

void DebugApp::paint_overlay(tuinator::PaintContext& ctx) const {
    if (context_menu_ != nullptr && context_menu_->is_open()) {
        context_menu_->layout(overlay_clip_bounds());
        context_menu_->paint(ctx);
    }

    if (!breakpoint_prompt_active() || breakpoint_prompt_input_ == nullptr) {
        return;
    }

    const tuinator::Rect bounds = breakpoint_prompt_bounds();
    if (bounds.width <= 0 || bounds.height <= 0) {
        return;
    }

    ctx.canvas.fill_rect(bounds, ' ', dap_theme_.panel_background);
    const tuinator::Rect local{0, 0, bounds.width, bounds.height};
    ctx.with_clip(local, [&](tuinator::PaintContext& child_ctx) {
        breakpoint_prompt_input_->layout(local);
        breakpoint_prompt_input_->paint(child_ctx);
    });
}

tuinator::Rect DebugApp::overlay_clip_bounds() const {
    const tuinator::Size term = app_->terminal_size();
    return {0, 0, term.width, term.height};
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

std::optional<std::string> DebugApp::identifier_at_line_column(const std::string& line_text, int column) const {
    if (line_text.empty() || column < 0) {
        return std::nullopt;
    }

    const auto is_ident_start = [](unsigned char ch) { return std::isalpha(ch) != 0 || ch == '_'; };
    const auto is_ident_part = [](unsigned char ch) { return std::isalnum(ch) != 0 || ch == '_'; };

    const std::size_t byte_at =
        std::min(tuinator::text_byte_length_for_width(line_text, column), line_text.size());
    std::size_t pos = byte_at;
    if (pos >= line_text.size() || !is_ident_part(static_cast<unsigned char>(line_text[pos]))) {
        if (pos == 0 || !is_ident_part(static_cast<unsigned char>(line_text[pos - 1]))) {
            return std::nullopt;
        }
        pos -= 1;
    }

    std::size_t start = pos;
    while (start > 0 && is_ident_part(static_cast<unsigned char>(line_text[start - 1]))) {
        --start;
    }
    std::size_t end = pos + 1;
    while (end < line_text.size() && is_ident_part(static_cast<unsigned char>(line_text[end]))) {
        ++end;
    }

    if (start >= line_text.size() || !is_ident_start(static_cast<unsigned char>(line_text[start]))) {
        return std::nullopt;
    }

    return line_text.substr(start, end - start);
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
                                            const std::optional<std::string>& seed_identifier) {
    open_source_context_menu(path, line, anchor, seed_identifier, {}, false);
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
                                         const std::optional<std::string>& seed_identifier) {
    if (context_menu_ == nullptr || path.empty() || line <= 0) {
        return;
    }

    const bool probe_goto =
        adapter_ != DebugAdapter::Rr && is_session_stopped() && session_io_ != nullptr && session_io_->is_active();

    if (!probe_goto) {
        open_source_context_menu(path, line, anchor, seed_identifier, {}, false);
        return;
    }

    pending_source_context_menu_ = PendingSourceContextMenu{path, line, anchor, seed_identifier};
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
        open_source_context_menu(pending.path, pending.line, pending.anchor, pending.seed_identifier, {}, false);
        return;
    }

    std::vector<std::pair<std::int64_t, std::string>> goto_targets;
    if (event.success) {
        goto_targets = parse_goto_targets_json(event.payload);
    }

    const bool offer_lldb_line_jump =
        goto_targets.empty() && adapter_ == DebugAdapter::Lldb && pending.line > 0 &&
        static_cast<std::uint32_t>(pending.line) != model_.execution_line;

    open_source_context_menu(pending.path, pending.line, pending.anchor, pending.seed_identifier,
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
    const std::optional<std::string>& seed_identifier,
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
            "Edit condition",
            [this, normalized, line]() { begin_edit_breakpoint_condition(normalized, line); },
        });
        if (has_condition) {
            items.push_back(ContextMenu::Item{
                "Clear condition",
                [this, normalized, line]() { set_breakpoint_condition(normalized, line, ""); },
            });
        }
        items.push_back(ContextMenu::Item{
            "Edit hit condition",
            [this, normalized, line]() { begin_edit_breakpoint_hit_condition(normalized, line); },
        });
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
            [this, ensure_breakpoint, normalized, line]() {
                ensure_breakpoint();
                begin_edit_breakpoint_condition(normalized, line);
            },
        });
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
