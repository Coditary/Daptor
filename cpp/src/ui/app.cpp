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
#include "tui_debug_ui/stacks_panel.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"
#include "tui_debug_ui/tty_setup.hpp"

#include <tuinator/backend/terminal_backend.hpp>
#include <tuinator/render/canvas.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>
#include <tuinator/tuinator.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
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

bool scope_rows_include_variables(const std::vector<std::string>& rows) {
    for (const std::string& row : rows) {
        if (row.size() >= 2 && row[0] == ' ' && row[1] == ' ') {
            return true;
        }
    }
    return false;
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
    }

    bool handle_event(const tuinator::Event& event) override {
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
            if (key->key == tuinator::Key::Escape || key->character == 'q' || key->character == 'Q') {
                if (app_ != nullptr) {
                    app_->quit();
                }
                return true;
            }
            if (debug_app_ != nullptr && debug_app_->handle_global_key(*key)) {
                return true;
            }
        }

        if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
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
};

std::unique_ptr<tuinator::Widget> make_repl_panel(const tui_debug_ui::DapUiTheme& theme,
                                                  tui_debug_ui::DebugApp* debug_app, tuinator::TextInput** input_out,
                                                  tuinator::ListView** history_out) {
    auto root = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});

    auto input = std::make_unique<tuinator::TextInput>(tuinator::TextInputOptions{.placeholder = "> expression"},
                                                       theme.label, theme.selection);
    input->set_on_submit([debug_app](const std::string& value) {
        if (debug_app != nullptr) {
            debug_app->submit_repl(value);
        }
    });
    input->set_flex(0);

    auto history = std::make_unique<tui_debug_ui::NavigableListView>(theme.label, theme.selection, theme.panel_background);
    history->set_items({});
    history->set_flex(1);

    if (input_out != nullptr) {
        *input_out = input.get();
    }
    if (history_out != nullptr) {
        *history_out = history.get();
    }

    root->add_child(std::move(input));
    root->add_child(std::move(history));

    auto pane = std::make_unique<tui_debug_ui::TitledScrollPane>("REPL", std::move(root), theme.title_normal,
                                                                 theme.panel_background, theme.scroll_view_options(),
                                                                 false);
    return pane->release_widget();
}

int sidebar_first_size(int terminal_width, std::uint16_t sidebar_pct) {
    const int pct = static_cast<int>(sidebar_pct);
    return std::max(24, terminal_width * pct / 100);
}

int repl_first_size(int terminal_width, std::uint16_t repl_pct) {
    const int pct = static_cast<int>(repl_pct);
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

std::string panel_title_from_path(const std::string& path) {
    if (path.empty()) {
        return "Source";
    }
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
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
    const std::size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size()) {
        return "python";
    }
    const std::string ext = path.substr(dot + 1);
    if (ext == "py") {
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

DebugApp::DebugApp(const std::string& program_path, SessionMode mode)
    : mode_(mode), program_path_(program_path), session_io_(std::make_unique<SessionIoThread>(mode)),
      app_(std::make_unique<tuinator::Application>()) {
    tuinator::Theme theme = app_->theme();
    dap_theme_.apply_to(theme);
    app_->set_theme(theme);

    if (mode_ == SessionMode::Mock) {
        model_.status_message = "Mock UI mode (no Rust backend)";
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
    scopes_panel_ = std::make_unique<ScopesPanel>(dap_theme_.title_normal, dap_theme_.label,
                                                  dap_theme_.panel_background, scroll_options);
    stacks_panel_ = std::make_unique<StacksPanel>(dap_theme_.title_normal, dap_theme_.label,
                                                dap_theme_.panel_background, scroll_options);

    auto scopes_widget = scopes_panel_->release_widget();
    auto stacks_widget = stacks_panel_->release_widget();

    const int scopes_first = std::max(6, main_h * model_.layout.scopes_pct / 100);

    auto sidebar = std::make_unique<ResizableSplitPane>(
        std::move(scopes_widget), std::move(stacks_widget),
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
    source_panel_->set_on_toggle_breakpoint([this](int line) { toggle_breakpoint_at_line(line); });
    source_panel_->set_on_request_viewport([this](int /*center_line*/) {
        cached_highlight_first_line_ = -1;
        cached_highlight_line_count_ = -1;
        highlight_request_first_line_ = -1;
        highlight_request_line_count_ = -1;
    });
    if (!program_path_.empty()) {
        source_panel_->set_file_line_count(std::max(1, count_file_lines(read_file_or_empty(program_path_))));
    }

    source_section_ = std::make_unique<TitledScrollPane>(panel_title_from_path(model_.source_path),
                                                         std::move(source_panel), dap_theme_.title_normal,
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

    auto repl = make_repl_panel(dap_theme_, this, &repl_input_, &repl_history_);

    auto console_panel = std::make_unique<ConsolePanel>(dap_theme_.label, dap_theme_.panel_background);
    console_panel_ = console_panel.get();

    auto console_section = std::make_unique<TitledScrollPane>("Console", std::move(console_panel),
                                                              dap_theme_.title_normal, dap_theme_.panel_background,
                                                              scroll_options);
    console_scroll_view_ = console_section->scroll_view();
    auto console_shell = console_section->release_widget();
    console_shell->set_flex(1);

    auto bottom_tray = std::make_unique<ResizableSplitPane>(
        std::move(repl), std::move(console_shell),
        tuinator::SplitPaneOptions{
            .orientation = tuinator::SplitOrientation::Horizontal,
            .first_size = repl_first_size(term_size.width, model_.layout.repl_pct),
            .divider_style = dap_theme_.divider,
        },
        dap_theme_.panel_background);
    bottom_tray_split_ = bottom_tray.get();
    bind_split_pane(bottom_tray_split_);
    bottom_tray_split_->set_on_first_size_changed([this](int /*first*/) {
        persist_split_size_as_pct(bottom_tray_split_, model_.layout.repl_pct, true);
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
    app_->refresh_focus();
    apply_focus();
    if (repl_history_ != nullptr && !repl_history_lines_.empty()) {
        repl_history_->set_items(repl_history_lines_);
    }
    sync_ui_from_model();
    sync_controls_bar();
    refresh_scroll_views();
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
    session_io_->start_launch(program_path_);
}

bool DebugApp::update_connecting_spinner() {
    const auto now = std::chrono::steady_clock::now();
    if (now - last_spinner_update_ < std::chrono::milliseconds(200)) {
        return false;
    }
    last_spinner_update_ = now;

    static constexpr char kSpinner[] = "|/-\\";
    const std::string message = std::string("Connecting to debugpy… ")
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
        if (model_.source_path.empty() && !program_path_.empty()) {
            model_.source_path = program_path_;
        }
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
        maybe_request_scope_variables();
        maybe_request_source_highlight();
        request_full_screen_refresh();
    }
}

void DebugApp::apply_snapshot_json_payload(const std::string& json) {
    const std::string previous_state = model_.session_state;
    model_.apply_snapshot_json(json);
    ++snapshot_generation_;
    scope_variables_fetch_pending_ = false;
    scope_variables_fetch_signature_.clear();
    if (model_.source_path.empty() && !program_path_.empty()) {
        model_.source_path = program_path_;
    }
    if (previous_state == "disconnected" && is_session_stopped()) {
        reclaim_terminal_for_ui();
    }
    if (is_session_stopped() &&
        (restart_pending_ || previous_state == "exited" || previous_state == "disconnected" ||
         previous_state == "running" || previous_state == "stopped" || previous_state.empty())) {
        push_breakpoints_to_session(effective_source_path());
    }
    if (restart_pending_ && is_session_stopped()) {
        restart_pending_ = false;
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
    case SessionIoEventKind::HighlightReady:
        if (event.success && source_panel_ != nullptr) {
            try {
                const auto lines = parse_highlight_json(event.payload);
                if (!lines.empty()) {
                    source_panel_->set_lines(std::move(lines));
                    if (!uses_full_file_source()) {
                        source_panel_->set_scroll_offset(0);
                    } else if (source_scroll_view_ != nullptr) {
                        source_scroll_view_->refresh_content();
                    }
                }
            } catch (const std::exception&) {
                break;
            }
            cached_highlight_first_line_ = event.highlight_first_line;
            cached_highlight_line_count_ = event.highlight_line_count;
        }
        highlight_request_first_line_ = -1;
        highlight_request_line_count_ = -1;
        break;
    case SessionIoEventKind::CommandFinished:
        if (!event.success) {
            model_.status_message = event.detail.empty() ? "Command failed" : event.detail;
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
        } else {
            apply_execution_command_started(event.payload.c_str());
            model_.status_message = command_status_message(event.payload.c_str());
        }
        break;
    case SessionIoEventKind::EvaluateFinished:
        if (event.success) {
            repl_history_lines_.push_back("> " + event.detail + " => " + event.payload);
            if (repl_history_ != nullptr) {
                repl_history_->set_items(repl_history_lines_);
            }
            model_.status_message = "REPL ok";
        } else {
            model_.status_message = event.payload.empty() ? "REPL failed" : event.payload;
        }
        break;
    case SessionIoEventKind::BreakpointsFinished:
        model_.status_message =
            event.success ? "Breakpoints updated" : (event.detail.empty() ? "Breakpoint update failed" : event.detail);
        if (source_panel_ != nullptr) {
            source_panel_->mark_dirty();
        }
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
        }
        return;
    }

    if (!needs_sync) {
        return;
    }

    if (model_.connection_state == ConnectionState::Connected) {
        maybe_request_scope_variables();
        maybe_request_source_highlight();
    }

    sync_ui_from_model();
}

void DebugApp::sync_ui_from_model() {
    sync_controls_bar();
    sync_status_bar();
    if (scopes_panel_ != nullptr) {
        const std::vector<std::string> scope_rows = build_scope_rows(model_);
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
        std::vector<std::string> stack_lines;
        stack_lines.reserve(model_.threads.size() + model_.stack_frames.size());
        for (const ThreadInfo& thread : model_.threads) {
            stack_lines.push_back("[" + std::to_string(thread.id) + "] " + thread.name);
        }
        for (std::size_t index = 0; index < model_.stack_frames.size(); ++index) {
            const StackFrameInfo& frame = model_.stack_frames[index];
            const std::string marker = index == 0 ? "\u{eaf0} " : "  ";
            const std::string path = frame.path.empty() ? panel_title_from_path(model_.source_path) : frame.path;
            stack_lines.push_back(marker + "#" + std::to_string(frame.id) + " " + frame.name + " @ " + path + ":" +
                                  std::to_string(std::max<std::int64_t>(0, frame.line)));
        }
        if (stack_lines != cached_stack_lines_) {
            cached_stack_lines_ = stack_lines;
            stacks_panel_->set_lines(std::move(stack_lines));
        }
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
        source_panel_->set_execution_line(static_cast<int>(model_.current_line));
        if (model_.current_line > 0) {
            source_panel_->set_cursor_line(static_cast<int>(model_.current_line));
            if (snapshot_generation_ != cached_follow_generation_ ||
                model_.current_line != cached_follow_line_) {
                cached_follow_generation_ = snapshot_generation_;
                cached_follow_line_ = model_.current_line;
                source_panel_->ensure_cursor_visible();
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
        scopes.emplace_back(scope.variables_reference, scope.name);
    }

    scope_variables_fetch_pending_ = true;
    session_io_->request_scope_variables(signature, scopes);
}

void DebugApp::maybe_request_source_highlight() {
    if (divider_drag_active_ || source_panel_ == nullptr) {
        return;
    }

    if (model_.source_path.empty()) {
        cached_source_path_.clear();
        cached_source_text_.clear();
        source_panel_->set_lines({});
        return;
    }

    if (model_.source_path != cached_source_path_) {
        cached_source_path_ = model_.source_path;
        cached_source_text_ = read_file_or_empty(cached_source_path_);
        source_panel_->set_file_line_count(std::max(1, count_file_lines(cached_source_text_)));
        cached_highlight_first_line_ = -1;
        cached_highlight_line_count_ = -1;
        highlight_request_first_line_ = -1;
        highlight_request_line_count_ = -1;
    }

    if (cached_source_text_.empty()) {
        source_panel_->set_lines({});
        return;
    }

    const int viewport_height = source_viewport_height();
    const int file_lines = source_panel_->file_line_count();
    const bool full_file = uses_full_file_source();
    const int line_count = full_file ? file_lines : std::max(1, viewport_height);

    int first_line = 1;
    if (!full_file) {
        first_line = std::max(1, source_panel_->cursor_line() - line_count / 2);
        const auto& visible_lines = source_panel_->lines();
        if (!visible_lines.empty()) {
            const int scroll = source_panel_->scroll_offset();
            if (scroll >= 0 && scroll < static_cast<int>(visible_lines.size())) {
                first_line = visible_lines[static_cast<std::size_t>(scroll)].line_number;
            }
        }
        const int cursor_line = source_panel_->cursor_line();
        if (cursor_line < first_line || cursor_line >= first_line + line_count) {
            first_line = std::max(1, cursor_line - line_count / 2);
        }
    }
    if (first_line == cached_highlight_first_line_ && line_count == cached_highlight_line_count_ &&
        cached_source_path_ == model_.source_path) {
        return;
    }
    if (first_line == highlight_request_first_line_ && line_count == highlight_request_line_count_ &&
        cached_source_path_ == model_.source_path) {
        return;
    }

    highlight_request_first_line_ = first_line;
    highlight_request_line_count_ = line_count;
    apply_instant_source_viewport(first_line, line_count);
    session_io_->request_highlight(language_from_path(cached_source_path_), cached_source_text_, first_line,
                                    line_count);
}

bool DebugApp::is_session_stopped() const {
    return model_.session_state.find("Stopped") != std::string::npos ||
           model_.session_state.find("stopped") != std::string::npos;
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
        return;
    }
    controls_bar_->set_session_active(active);
    controls_bar_->set_stopped(stopped);
    controls_bar_->set_session_ended(ended);
}

std::string DebugApp::format_status_bar_text() const {
    const int max_cols = app_ != nullptr ? std::max(20, app_->terminal_size().width) : 80;
    const std::string suffix = " | " + model_.connection_label() + " | focus: " + model_.focus_label();
    const int suffix_width = tuinator::text_display_width(suffix);

    std::string prefix = model_.status_message;
    if (!model_.source_path.empty()) {
        prefix += " | ";
        prefix += model_.source_path;
    }
    if (model_.current_line > 0) {
        prefix += " | ln " + std::to_string(model_.current_line);
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
        model_.layout.narrow_repl();
        model_.status_message = "REPL " + std::to_string(model_.layout.repl_pct) + "%";
        build_ui();
        return true;
    }
    if (key.character == ']') {
        model_.layout.widen_repl();
        model_.status_message = "REPL " + std::to_string(model_.layout.repl_pct) + "%";
        build_ui();
        return true;
    }

    return false;
}

bool DebugApp::handle_global_key(const tuinator::KeyPress& key) {
    if (key.ctrl && key.character == 'c') {
        if (app_ != nullptr) {
            app_->quit();
        }
        return true;
    }

    if (handle_layout_resize_key(key)) {
        return true;
    }

    if (key.ctrl || key.alt) {
        return false;
    }

    if (model_.focus != Focus::Repl && key.character >= '1' && key.character <= '8') {
        static constexpr const char* kControlOps[] = {"play_pause", "step_into", "step_over", "step_out",
                                                      "step_back", "restart", "terminate", "disconnect"};
        send_command(kControlOps[key.character - '1']);
        return true;
    }

    if (key.key == tuinator::Key::Tab) {
        cycle_focus_next();
        return true;
    }

    if ((key.character == 'b' || key.character == ' ') && !key.ctrl && !key.alt && source_panel_ != nullptr &&
        model_.focus == Focus::Source) {
        if (!source_panel_->is_focused()) {
            apply_focus();
        }
        toggle_breakpoint();
        return true;
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
            scope_variables_fetch_signature_.clear();
        } else if (std::strcmp(op, "play_pause") == 0) {
            model_.status_message = "Pausing…";
        }
        return;
    }

    if (std::strcmp(op, "step_over") == 0 || std::strcmp(op, "next") == 0 || std::strcmp(op, "step_into") == 0 ||
        std::strcmp(op, "step_in") == 0 || std::strcmp(op, "step_out") == 0 || std::strcmp(op, "step_back") == 0) {
        model_.session_state = "running";
        model_.stop_reason.clear();
        scope_variables_fetch_signature_.clear();
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

void DebugApp::send_command(const char* op) {
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
        model_.focus = Focus::Repl;
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
    if (repl_input_ != nullptr) {
        widgets.push_back(repl_input_);
    }
    if (repl_history_ != nullptr) {
        widgets.push_back(repl_history_);
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
    case Focus::Repl:
        if (repl_input_ != nullptr) {
            target = repl_input_;
        } else {
            target = repl_history_;
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

    if (repl_input_ != nullptr && repl_input_->is_focused()) {
        detected = Focus::Repl;
    } else if (repl_history_ != nullptr && repl_history_->is_focused()) {
        detected = Focus::Repl;
    } else if (scopes_panel_ != nullptr && scopes_panel_->list_widget() != nullptr &&
               scopes_panel_->list_widget()->is_focused()) {
        detected = Focus::Scopes;
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
    if (scopes_panel_ != nullptr && scopes_panel_->list_widget() != nullptr) {
        scopes_panel_->list_widget()->mark_dirty();
    }
    refresh_scroll_views();
    if (stacks_panel_ != nullptr && stacks_panel_->list_widget() != nullptr) {
        stacks_panel_->list_widget()->mark_dirty();
    }
    if (repl_input_ != nullptr) {
        repl_input_->mark_dirty();
    }
    if (repl_history_ != nullptr) {
        repl_history_->mark_dirty();
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

void DebugApp::toggle_breakpoint() {
    if (source_panel_ == nullptr) {
        return;
    }
    toggle_breakpoint_at_line(source_panel_->cursor_line());
}

std::string DebugApp::effective_source_path() const {
    if (!model_.source_path.empty()) {
        return model_.source_path;
    }
    return program_path_;
}

void DebugApp::sync_breakpoints_to_panel() {
    if (source_panel_ == nullptr) {
        return;
    }

    const std::string path = effective_source_path();
    if (path.empty()) {
        return;
    }

    const auto it = breakpoints_by_path_.find(path);
    if (it != breakpoints_by_path_.end()) {
        if (source_panel_->breakpoint_lines() != it->second) {
            source_panel_->set_breakpoint_lines(it->second);
        }
    }
}

void DebugApp::push_breakpoints_to_session(const std::string& path) {
    if (path.empty() || session_io_ == nullptr || !session_io_->is_active() ||
        model_.session_state == "disconnected") {
        return;
    }

    const auto it = breakpoints_by_path_.find(path);
    std::string json = "[";
    bool first = true;
    if (it != breakpoints_by_path_.end()) {
        for (int bp_line : it->second) {
            if (!first) {
                json += ',';
            }
            json += std::to_string(bp_line);
            first = false;
        }
    }
    json += ']';
    session_io_->post_set_breakpoints(path, json);
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

    std::unordered_set<int>& lines = breakpoints_by_path_[path];
    if (lines.contains(line)) {
        lines.erase(line);
        model_.status_message = "Removed breakpoint at line " + std::to_string(line);
    } else {
        if (!is_breakpointable_line(source_panel_, cached_source_text_, line)) {
            model_.status_message = "Cannot set breakpoint on an empty line";
            sync_status_bar();
            return;
        }
        lines.insert(line);
        model_.status_message = "Set breakpoint at line " + std::to_string(line);
    }
    source_panel_->set_breakpoint_lines(lines);

    if (source_panel_->lines().empty() && !cached_source_text_.empty()) {
        const int line_count =
            uses_full_file_source() ? source_panel_->file_line_count() : std::max(1, source_viewport_height());
        const int first_line = uses_full_file_source() ? 1 : std::max(1, line - line_count / 2);
        apply_instant_source_viewport(first_line, line_count);
    }

    push_breakpoints_to_session(path);
    sync_breakpoints_to_panel();
    sync_status_bar();
    mark_all_panels_dirty();
}

void DebugApp::submit_repl(const std::string& expression) {
    if (expression.empty()) {
        return;
    }

    if (!session_io_->is_active() || model_.stack_frames.empty()) {
        model_.status_message = "REPL unavailable";
        sync_ui_from_model();
        return;
    }

    const std::int64_t frame_id = model_.stack_frames.front().id;
    model_.status_message = "Evaluating…";
    session_io_->post_evaluate(expression, frame_id, "repl");
    if (repl_input_ != nullptr) {
        repl_input_->set_value("");
    }
    sync_ui_from_model();
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
    return std::string("Sent: ") + op;
}

}  // namespace tui_debug_ui
