#include "tui_debug_ui/network_panel.hpp"

#include "tui_debug_ui/clipboard.hpp"
#include "tui_debug_ui/compose_templates.hpp"
#include "tui_debug_ui/dap_ui_theme.hpp"

#include <tuinator/core/event.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/scrollbar.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include <utility>

namespace tui_debug_ui {
namespace {

constexpr int kStatusRows = 1;
constexpr int kTabRows = 1;

std::string trim_whitespace(std::string value) {
    const auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
    value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
    return value;
}

bool is_pointer_pick(const tuinator::MouseEvent& mouse) {
    return mouse.action == tuinator::MouseAction::Click || mouse.action == tuinator::MouseAction::Release ||
           mouse.action == tuinator::MouseAction::Press;
}

bool is_wheel_action(tuinator::MouseAction action) {
    return action == tuinator::MouseAction::WheelUp || action == tuinator::MouseAction::WheelDown ||
           action == tuinator::MouseAction::WheelLeft || action == tuinator::MouseAction::WheelRight;
}

bool point_in_content(int x, int y, int width, int height, tuinator::Point local) {
    return local.x >= x && local.x < x + width && local.y >= y && local.y < y + height;
}

enum class DetailLineKind {
    Headline,
    Muted,
    Section,
    Body,
    Json,
};

struct DetailDisplayLine {
    std::string text;
    DetailLineKind kind = DetailLineKind::Body;
};

std::vector<std::string> wrap_text(std::string_view text, int width) {
    std::vector<std::string> lines;
    if (width <= 0) {
        return lines;
    }
    if (text.empty()) {
        lines.emplace_back();
        return lines;
    }

    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::string_view remainder = text.substr(offset);
        const std::size_t bytes = tuinator::text_byte_length_for_width(remainder, width);
        if (bytes == 0) {
            break;
        }
        lines.emplace_back(remainder.substr(0, bytes));
        offset += bytes;
    }
    return lines;
}

void append_wrapped_block(std::vector<DetailDisplayLine>& out, std::string_view title, std::string_view block,
                         int width, DetailLineKind body_kind) {
    if (!title.empty()) {
        out.push_back({std::string(title), DetailLineKind::Section});
    }
    if (block.empty()) {
        out.push_back({"(empty)", DetailLineKind::Muted});
        return;
    }

    std::string line;
    for (char ch : block) {
        if (ch == '\n') {
            for (const std::string& wrapped : wrap_text(line, width)) {
                const DetailLineKind kind =
                    wrapped.rfind("HTTP/", 0) == 0 || wrapped.rfind("{", 0) == 0 ? DetailLineKind::Json : body_kind;
                out.push_back({wrapped, kind});
            }
            line.clear();
            continue;
        }
        line.push_back(ch);
    }
    if (!line.empty()) {
        for (const std::string& wrapped : wrap_text(line, width)) {
            const DetailLineKind kind =
                wrapped.rfind("HTTP/", 0) == 0 || wrapped.rfind("{", 0) == 0 ? DetailLineKind::Json : body_kind;
            out.push_back({wrapped, kind});
        }
    }
}

const char* origin_detail_banner(NetworkExchangeOrigin origin) {
    switch (origin) {
    case NetworkExchangeOrigin::Demo:
        return "Preview data only — not real traffic";
    case NetworkExchangeOrigin::Compose:
        return "You sent this from Compose (real HTTP)";
    case NetworkExchangeOrigin::Captured:
        return nullptr;
    }
    return nullptr;
}

std::vector<DetailDisplayLine> build_exchange_detail_lines(const NetworkExchange& exchange, int width) {
    std::vector<DetailDisplayLine> lines;
    if (width <= 0) {
        return lines;
    }

    if (const char* banner = origin_detail_banner(exchange.origin); banner != nullptr) {
        for (const std::string& wrapped : wrap_text(banner, width)) {
            lines.push_back({wrapped, DetailLineKind::Section});
        }
        lines.push_back({"", DetailLineKind::Body});
    }

    std::ostringstream headline;
    headline << exchange.method << ' ' << exchange.path;
    if (exchange.state == NetworkExchangeState::Dropped) {
        headline << "  [FAILED]";
    } else if (exchange.state == NetworkExchangeState::Pending) {
        headline << "  [INTERCEPTED]";
    } else if (exchange.status_code > 0) {
        headline << "  -> " << exchange.status_code;
        if (exchange.duration_ms > 0) {
            headline << "  (" << exchange.duration_ms << "ms)";
        }
    }
    for (const std::string& wrapped : wrap_text(headline.str(), width)) {
        lines.push_back({wrapped, DetailLineKind::Headline});
    }
    if (!exchange.summary.empty() && exchange.origin == NetworkExchangeOrigin::Captured) {
        for (const std::string& wrapped : wrap_text(exchange.summary, width)) {
            lines.push_back({wrapped, DetailLineKind::Muted});
        }
    }
    lines.push_back({"", DetailLineKind::Body});
    append_wrapped_block(lines, "Request headers", exchange.request_headers, width, DetailLineKind::Body);
    if (!exchange.request_body.empty()) {
        lines.push_back({"", DetailLineKind::Body});
        append_wrapped_block(lines, "Request body", exchange.request_body, width, DetailLineKind::Json);
    }
    lines.push_back({"", DetailLineKind::Body});
    append_wrapped_block(lines, "Response", exchange.response_headers, width, DetailLineKind::Body);
    if (!exchange.response_body.empty()) {
        lines.push_back({"", DetailLineKind::Body});
        append_wrapped_block(lines, "Response body", exchange.response_body, width, DetailLineKind::Json);
    }
    return lines;
}

std::string compose_field_label(const char* label, ComposeEditField field,
                                std::optional<ComposeEditField> active_field) {
    if (active_field == field) {
        return std::string("> ") + label;
    }
    return label;
}

std::vector<DetailDisplayLine> build_compose_detail_lines(const NetworkComposeTemplate& item, int width,
                                                          std::optional<ComposeEditField> active_field,
                                                          const std::string* timeout_edit = nullptr) {
    std::vector<DetailDisplayLine> lines;
    if (width <= 0) {
        return lines;
    }

    if (active_field.has_value()) {
        append_wrapped_block(lines, compose_field_label("Name", ComposeEditField::Name, active_field), item.name, width,
                             DetailLineKind::Body);
        lines.push_back({"", DetailLineKind::Body});
    }
    append_wrapped_block(lines, compose_field_label("Method", ComposeEditField::Method, active_field), item.method,
                         width, DetailLineKind::Body);
    lines.push_back({"", DetailLineKind::Body});
    append_wrapped_block(lines, compose_field_label("URL", ComposeEditField::Url, active_field), item.url, width,
                         DetailLineKind::Body);
    lines.push_back({"", DetailLineKind::Body});
    append_wrapped_block(lines, compose_field_label("Headers", ComposeEditField::Headers, active_field),
                         item.headers.empty() ? "(none)" : item.headers, width, DetailLineKind::Body);
    lines.push_back({"", DetailLineKind::Body});
    append_wrapped_block(lines, compose_field_label("Body", ComposeEditField::Body, active_field),
                         item.body.empty() ? "(none)" : item.body, width, DetailLineKind::Json);
    lines.push_back({"", DetailLineKind::Body});
    const std::string timeout_value =
        active_field == ComposeEditField::Timeout && timeout_edit != nullptr ? *timeout_edit
                                                                             : std::to_string(item.timeout_ms);
    append_wrapped_block(lines, compose_field_label("Timeout (ms)", ComposeEditField::Timeout, active_field),
                         timeout_value, width, DetailLineKind::Body);
    lines.push_back({"", DetailLineKind::Body});
    if (active_field.has_value()) {
        lines.push_back({"Tab next field  Enter save  Esc cancel", DetailLineKind::Muted});
    } else {
        lines.push_back({"e edit  n new  r reload  s send", DetailLineKind::Muted});
    }
    return lines;
}

tuinator::Style detail_line_style(const DapUiTheme& theme, DetailLineKind kind) {
    switch (kind) {
    case DetailLineKind::Headline:
        return theme.title_watches;
    case DetailLineKind::Muted:
        return theme.row_action_muted;
    case DetailLineKind::Section:
        return theme.scope_header;
    case DetailLineKind::Json:
        return theme.console_stdout;
    case DetailLineKind::Body:
        return theme.label;
    }
    return theme.label;
}

}  // namespace

NetworkPanel::NetworkPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options)
    : theme_(theme), scrollbar_options_(scroll_options.scrollbars.with_horizontal(false)) {
    session_ = make_demo_network_session();
    compose_templates_ = default_compose_templates();

    for (std::size_t i = 0; i < session_.exchanges.size(); ++i) {
        if (session_.exchanges[i].state == NetworkExchangeState::Pending) {
            selected_index_ = static_cast<int>(i);
            break;
        }
    }
    if (!compose_templates_.empty()) {
        apply_compose_template(0);
    }
}

tuinator::Size NetworkPanel::preferred_size() const {
    const int rows = std::max(8, kStatusRows + kTabRows + static_cast<int>(session_.exchanges.size()) + 2);
    return {48, rows};
}

void NetworkPanel::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    sync_detail_scroll();
}

void NetworkPanel::paint(tuinator::PaintContext& ctx) const {
    tuinator::Canvas& canvas = ctx.canvas;
    if (bounds_.width <= 0 || bounds_.height <= 0) {
        return;
    }

    canvas.fill_rect({{0, 0}, bounds_.size()}, ' ', theme_.panel_background);

    if (bounds_.height == 1) {
        paint_status_row(canvas);
        return;
    }

    paint_status_row(canvas);
    if (bounds_.height >= 2) {
        paint_tab_row(canvas);
    }

    const int top = list_area_top();
    const int height = list_area_height();
    if (height <= 0) {
        return;
    }

    if (active_view_ == NetworkPanelView::Traffic) {
        paint_traffic_view(canvas, top, height);
    } else {
        paint_compose_view(canvas, top, height);
    }
}

bool NetworkPanel::handle_event(const tuinator::Event& event) {
    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        return handle_mouse(*mouse);
    }

    if (!is_focused()) {
        return false;
    }

    const auto* key = std::get_if<tuinator::KeyPress>(&event);
    if (key == nullptr) {
        return false;
    }

    if (active_view_ == NetworkPanelView::Compose) {
        return handle_compose_key(*key);
    }
    return handle_traffic_key(*key);
}

tuinator::Widget* NetworkPanel::hit_test(tuinator::Point point) {
    if (bounds_.contains(point)) {
        return this;
    }
    return nullptr;
}

tuinator::Widget* NetworkPanel::hit_test_focusable(tuinator::Point point) {
    return hit_test(point);
}

bool NetworkPanel::has_focused_descendant() const { return is_focused(); }

void NetworkPanel::collect_focusable(std::vector<tuinator::Widget*>& out) { out.push_back(this); }

void NetworkPanel::refresh_scroll_content() { mark_dirty(); }

void NetworkPanel::set_session(NetworkMockSession session) {
    session_ = std::move(session);
    selected_index_ = 0;
    list_scroll_ = 0;
    detail_scroll_ = 0;
    mark_dirty();
}

void NetworkPanel::set_on_action(std::function<void(const std::string&, const NetworkExchange&)> callback) {
    on_action_ = std::move(callback);
}

void NetworkPanel::set_on_compose_send(std::function<bool(const NetworkComposeTemplate&)> callback) {
    on_compose_send_ = std::move(callback);
}

void NetworkPanel::set_on_message(std::function<void(const std::string&)> callback) {
    on_message_ = std::move(callback);
}

void NetworkPanel::set_workspace_root(const std::filesystem::path& workspace_root) {
    workspace_root_ = workspace_root;
    load_compose_templates_from_disk();
}

void NetworkPanel::load_compose_templates_from_disk() {
    const ComposeTemplatesLoadResult loaded = load_compose_templates(workspace_root_);
    compose_templates_path_ = loaded.path;
    compose_templates_ = loaded.templates;
    if (selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        selected_template_index_ = compose_templates_.empty() ? -1 : 0;
    }
    if (selected_template_index_ >= 0) {
        apply_compose_template(selected_template_index_);
    }
    mark_dirty();
}

void NetworkPanel::set_compose_pending(bool pending) {
    compose_pending_ = pending;
    mark_dirty();
}

void NetworkPanel::set_compose_result(const std::optional<NetworkExchange>& result) {
    compose_pending_ = false;
    last_compose_result_ = result;
    reset_detail_scroll();
    mark_dirty();
}

void NetworkPanel::select_last_exchange() {
    if (session_.exchanges.empty()) {
        return;
    }
    selected_index_ = static_cast<int>(session_.exchanges.size()) - 1;
    active_view_ = NetworkPanelView::Traffic;
    list_scroll_ = 0;
    detail_scroll_ = 0;
    mark_dirty();
}

void NetworkPanel::set_view(NetworkPanelView view) {
    if (active_view_ == view) {
        return;
    }
    active_view_ = view;
    list_scroll_ = 0;
    detail_scroll_ = 0;
    mark_dirty();
}

void NetworkPanel::cycle_view() {
    set_view(active_view_ == NetworkPanelView::Traffic ? NetworkPanelView::Compose : NetworkPanelView::Traffic);
}

tuinator::Widget* NetworkPanel::list_widget() const { return const_cast<NetworkPanel*>(this); }

tuinator::Widget* NetworkPanel::focus_widget() const { return const_cast<NetworkPanel*>(this); }

bool NetworkPanel::is_compose_input_focused() const { return compose_editing_; }

const NetworkExchange* NetworkPanel::selected_exchange() const {
    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(session_.exchanges.size())) {
        return nullptr;
    }
    return &session_.exchanges[static_cast<std::size_t>(selected_index_)];
}

bool NetworkPanel::perform_action(char action_key) {
    if (active_view_ == NetworkPanelView::Compose) {
        if (compose_editing_) {
            return false;
        }
        switch (action_key) {
        case 's':
            return send_compose();
        case 'e':
            return begin_compose_edit();
        case 'n':
            return add_compose_template();
        case 'r':
            return reload_compose_templates();
        case 'd':
            return delete_compose_template();
        default:
            return false;
        }
    }

    if (selected_index_ < 0 || selected_index_ >= static_cast<int>(session_.exchanges.size())) {
        return false;
    }
    NetworkExchange& exchange = session_.exchanges[static_cast<std::size_t>(selected_index_)];

    if (action_key == 'f' && exchange.state == NetworkExchangeState::Pending) {
        exchange.state = NetworkExchangeState::Completed;
        exchange.status_code = 201;
        exchange.duration_ms = 128;
        exchange.response_headers =
            "HTTP/1.1 201 Created\n"
            "Content-Type: application/json\n"
            "Location: /api/v1/orders/ord_42";
        exchange.response_body = R"({"order_id":"ord_42","status":"confirmed","total":59.97})";
        if (on_action_) {
            on_action_("forward", exchange);
        }
        mark_dirty();
        return true;
    }
    if (action_key == 'd' && exchange.state == NetworkExchangeState::Pending) {
        exchange.state = NetworkExchangeState::Dropped;
        exchange.status_code = 0;
        exchange.duration_ms = 0;
        exchange.response_headers = "(dropped by debugger)";
        exchange.response_body = "";
        if (on_action_) {
            on_action_("drop", exchange);
        }
        mark_dirty();
        return true;
    }
    if (action_key == 'e') {
        if (on_action_) {
            on_action_("edit", exchange);
        }
        return true;
    }
    if (action_key == 'r' && exchange.state == NetworkExchangeState::Completed) {
        if (on_action_) {
            on_action_("replay", exchange);
        }
        return true;
    }
    return false;
}

int NetworkPanel::list_area_top() const { return kStatusRows + kTabRows; }

int NetworkPanel::list_area_height() const { return std::max(0, bounds_.height - list_area_top()); }

int NetworkPanel::split_column() const {
    constexpr int kMinListWidth = 22;
    constexpr int kMinDetailWidth = 24;
    if (bounds_.width <= kMinListWidth + kMinDetailWidth + 1) {
        return std::max(1, bounds_.width / 2);
    }
    const int max_split = bounds_.width - kMinDetailWidth - 1;
    return std::clamp(bounds_.width * 38 / 100, kMinListWidth, max_split);
}

int NetworkPanel::detail_pane_width() const {
    const int detail_x = split_column() + 1;
    return std::max(1, bounds_.width - detail_x - 1);
}

int NetworkPanel::detail_pane_height() const { return std::max(0, list_area_height() - 1); }

NetworkPanel::PaneContentRect NetworkPanel::list_content_rect() const {
    const int top = list_area_top();
    const int split = split_column();
    const int height = detail_pane_height();
    const int item_count = active_view_ == NetworkPanelView::Traffic
                               ? static_cast<int>(session_.exchanges.size())
                               : static_cast<int>(compose_templates_.size());
    const bool show_scrollbar = item_count > height;
    const int width = std::max(1, split - 1 - (show_scrollbar ? 1 : 0));
    return {1, top + 1, width, height};
}

NetworkPanel::PaneContentRect NetworkPanel::detail_content_rect() const {
    const int top = list_area_top();
    const int detail_x = split_column() + 1;
    const int height = detail_pane_height();
    const int width_available = std::max(1, bounds_.width - detail_x - 1);
    const int line_count = active_view_ == NetworkPanelView::Traffic ? traffic_detail_line_count(width_available)
                                                                     : compose_detail_line_count(width_available);
    const bool show_scrollbar = line_count > height;
    const int width = std::max(1, width_available - (show_scrollbar ? 1 : 0));
    return {detail_x, top + 1, width, height};
}

std::vector<std::string> NetworkPanel::list_display_lines() const {
    std::vector<std::string> lines;
    if (active_view_ == NetworkPanelView::Traffic) {
        lines.reserve(session_.exchanges.size());
        const PaneContentRect content = list_content_rect();
        for (const NetworkExchange& exchange : session_.exchanges) {
            lines.push_back(format_list_row(exchange, content.width));
        }
    } else {
        lines.reserve(compose_templates_.size());
        const PaneContentRect content = list_content_rect();
        for (const NetworkComposeTemplate& item : compose_templates_) {
            const std::string line = item.method + "  " + item.name;
            if (content.width > 0 && tuinator::text_display_width(line) > content.width) {
                const std::size_t bytes = tuinator::text_byte_length_for_width(line, content.width - 1);
                lines.push_back(line.substr(0, bytes) + "~");
            } else {
                lines.push_back(line);
            }
        }
    }
    return lines;
}

std::vector<std::string> NetworkPanel::detail_display_lines() const {
    const PaneContentRect content = detail_content_rect();
    if (active_view_ == NetworkPanelView::Traffic) {
        if (const NetworkExchange* exchange = selected_exchange(); exchange != nullptr) {
            std::vector<DetailDisplayLine> styled = build_exchange_detail_lines(*exchange, content.width);
            std::vector<std::string> lines;
            lines.reserve(styled.size());
            for (const DetailDisplayLine& line : styled) {
                lines.push_back(line.text);
            }
            return lines;
        }
        return {};
    }
    if (selected_template_index_ < 0 || selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        return {};
    }
    std::vector<DetailDisplayLine> styled =
        build_compose_detail_lines(compose_templates_[static_cast<std::size_t>(selected_template_index_)], content.width,
                                   compose_editing_ ? std::optional(compose_edit_field_) : std::nullopt,
                                   compose_timeout_edit_for_paint());
    std::vector<std::string> lines;
    lines.reserve(styled.size());
    for (const DetailDisplayLine& line : styled) {
        lines.push_back(line.text);
    }
    return lines;
}

void NetworkPanel::clear_selection() { text_selection_.reset(); }

void NetworkPanel::begin_selection(NetworkFocusPane pane, tuinator::Point local) {
    if (const std::optional<TextCell> cell = text_cell_at(pane, local); cell.has_value()) {
        text_selection_ = TextSelection{pane, *cell, *cell, true};
        focus_pane_ = pane;
    }
}

void NetworkPanel::update_selection(tuinator::Point local) {
    if (!text_selection_.has_value() || !text_selection_->dragging) {
        return;
    }
    if (const std::optional<TextCell> cell = text_cell_at(text_selection_->pane, local); cell.has_value()) {
        text_selection_->cursor = *cell;
        mark_dirty();
    }
}

void NetworkPanel::end_selection() {
    if (text_selection_.has_value()) {
        text_selection_->dragging = false;
    }
}

std::optional<NetworkPanel::TextCell> NetworkPanel::text_cell_at(NetworkFocusPane pane,
                                                                  tuinator::Point local) const {
    const PaneContentRect content = pane == NetworkFocusPane::List ? list_content_rect() : detail_content_rect();
    if (local.x < content.x || local.y < content.y || local.x >= content.x + content.width ||
        local.y >= content.y + content.height) {
        return std::nullopt;
    }

    const std::vector<std::string> lines = pane == NetworkFocusPane::List ? list_display_lines() : detail_display_lines();
    const int scroll = pane == NetworkFocusPane::List ? list_scroll_ : detail_scroll_;
    const int line_index = local.y - content.y + scroll;
    if (line_index < 0 || line_index >= static_cast<int>(lines.size())) {
        return std::nullopt;
    }

    const int column = std::clamp(local.x - content.x, 0, content.width - 1);
    return TextCell{line_index, column};
}

std::string NetworkPanel::selected_text() const {
    if (!text_selection_.has_value()) {
        return {};
    }

    const TextSelection& selection = *text_selection_;
    const std::vector<std::string> lines =
        selection.pane == NetworkFocusPane::List ? list_display_lines() : detail_display_lines();
    if (lines.empty()) {
        return {};
    }

    TextCell start = selection.anchor;
    TextCell end = selection.cursor;
    if (start.line == end.line && start.column == end.column) {
        if (start.line >= 0 && start.line < static_cast<int>(lines.size())) {
            return lines[static_cast<std::size_t>(start.line)];
        }
        return {};
    }
    if (start.line > end.line || (start.line == end.line && start.column > end.column)) {
        std::swap(start, end);
    }

    std::ostringstream out;
    for (int line = start.line; line <= end.line; ++line) {
        if (line < 0 || line >= static_cast<int>(lines.size())) {
            continue;
        }
        const std::string& text = lines[static_cast<std::size_t>(line)];
        if (line == start.line && line == end.line) {
            const int begin = std::min(start.column, static_cast<int>(text.size()));
            const int end_col = std::min(end.column + 1, static_cast<int>(text.size()));
            out << text.substr(static_cast<std::size_t>(begin), static_cast<std::size_t>(end_col - begin));
        } else if (line == start.line) {
            const int begin = std::min(start.column, static_cast<int>(text.size()));
            out << text.substr(static_cast<std::size_t>(begin));
            out << '\n';
        } else if (line == end.line) {
            const int end_col = std::min(end.column + 1, static_cast<int>(text.size()));
            out << text.substr(0, static_cast<std::size_t>(end_col));
        } else {
            out << text << '\n';
        }
    }
    return out.str();
}

bool NetworkPanel::copy_selection() {
    const std::string text = selected_text();
    if (text.empty()) {
        return false;
    }
    if (!copy_to_clipboard(text)) {
        if (on_message_ != nullptr) {
            on_message_("Copy failed (clipboard unavailable)");
        }
        return false;
    }
    if (on_message_ != nullptr) {
        on_message_("Copied " + std::to_string(text.size()) + " chars to clipboard");
    }
    return true;
}

void NetworkPanel::paint_scrollbar(tuinator::Canvas& canvas, const PaneContentRect& content, int line_count,
                                     int scroll, const tuinator::ScrollbarInteractionState& /*state*/) const {
    if (content.height <= 0 || line_count <= content.height) {
        return;
    }
    const tuinator::Rect bar_rect{content.x + content.width, content.y, 1, content.height};
    if (bar_rect.x < 0 || bar_rect.x >= bounds_.width) {
        return;
    }
    canvas.with_clip(bar_rect, [&](tuinator::Canvas& bar_canvas) {
        tuinator::paint_scrollbars(bar_canvas, scrollbar_options_, 0, scroll, 1, line_count);
    });
}

bool NetworkPanel::handle_scrollbar_mouse(const tuinator::MouseEvent& mouse, const PaneContentRect& content,
                                           int line_count, int& scroll, tuinator::ScrollbarInteractionState& state) {
    if (content.height <= 0 || line_count <= content.height) {
        return false;
    }

    tuinator::ScrollbarConfig config = scrollbar_options_.config;
    config.horizontal = false;
    config.vertical = true;
    const int pane_width = content.width + 1;
    const tuinator::ScrollbarLayout layout = tuinator::compute_scrollbar_layout(
        pane_width, content.height, pane_width, line_count, 0, scroll, config, scrollbar_options_.behavior.show_arrows);

    tuinator::Point local{mouse.position.x - bounds_.x - content.x, mouse.position.y - bounds_.y - content.y};
    tuinator::ScrollbarScrollActions actions;
    actions.scroll_by = [this, &scroll, line_count, content](int /*dx*/, int dy) {
        scroll = std::clamp(scroll + dy, 0, std::max(0, line_count - content.height));
        sync_detail_scroll();
        mark_dirty();
    };
    actions.scroll_to = [this, &scroll, line_count, content](int /*x*/, int y) {
        scroll = std::clamp(y, 0, std::max(0, line_count - content.height));
        sync_detail_scroll();
        mark_dirty();
    };

    return tuinator::handle_scrollbar_mouse(mouse, local, layout, scrollbar_options_.behavior, state, actions, 0,
                                            scroll, pane_width, line_count);
}

void NetworkPanel::paint_line_with_selection(tuinator::Canvas& canvas, int x, int y, const std::string& line,
                                               tuinator::Style style, NetworkFocusPane pane, int line_index,
                                               int max_width) const {
    if (!text_selection_.has_value() || text_selection_->pane != pane) {
        const std::size_t bytes = tuinator::text_byte_length_for_width(line, max_width);
        canvas.draw_text({x, y}, line.substr(0, bytes), style);
        return;
    }

    TextCell start = text_selection_->anchor;
    TextCell end = text_selection_->cursor;
    if (start.line > end.line || (start.line == end.line && start.column > end.column)) {
        std::swap(start, end);
    }
    if (line_index < start.line || line_index > end.line) {
        const std::size_t bytes = tuinator::text_byte_length_for_width(line, max_width);
        canvas.draw_text({x, y}, line.substr(0, bytes), style);
        return;
    }

    const int sel_start = line_index == start.line ? std::clamp(start.column, 0, static_cast<int>(line.size())) : 0;
    const int sel_end = line_index == end.line ? std::clamp(end.column + 1, 0, static_cast<int>(line.size()))
                                               : static_cast<int>(line.size());

    const std::string prefix = line.substr(0, static_cast<std::size_t>(sel_start));
    const std::string middle = line.substr(static_cast<std::size_t>(sel_start),
                                           static_cast<std::size_t>(std::max(0, sel_end - sel_start)));
    const std::string suffix = line.substr(static_cast<std::size_t>(sel_end));

    int column = x;
    if (!prefix.empty()) {
        const std::size_t bytes = tuinator::text_byte_length_for_width(prefix, max_width);
        canvas.draw_text({column, y}, prefix.substr(0, bytes), style);
        column += tuinator::text_display_width(prefix.substr(0, bytes));
    }
    if (!middle.empty()) {
        const int remaining = std::max(0, max_width - (column - x));
        const std::size_t bytes = tuinator::text_byte_length_for_width(middle, remaining);
        canvas.draw_text({column, y}, middle.substr(0, bytes), theme_.selection);
        column += tuinator::text_display_width(middle.substr(0, bytes));
    }
    if (!suffix.empty()) {
        const int remaining = std::max(0, max_width - (column - x));
        const std::size_t bytes = tuinator::text_byte_length_for_width(suffix, remaining);
        canvas.draw_text({column, y}, suffix.substr(0, bytes), style);
    }
}

int NetworkPanel::traffic_detail_line_count(int width) const {
    if (const NetworkExchange* exchange = selected_exchange(); exchange != nullptr) {
        return static_cast<int>(build_exchange_detail_lines(*exchange, width).size());
    }
    return 0;
}

int NetworkPanel::compose_detail_line_count(int width) const {
    int count = 0;
    if (selected_template_index_ >= 0 && selected_template_index_ < static_cast<int>(compose_templates_.size())) {
        count += static_cast<int>(
            build_compose_detail_lines(compose_templates_[static_cast<std::size_t>(selected_template_index_)], width,
                                       compose_editing_ ? std::optional(compose_edit_field_) : std::nullopt,
                                       compose_timeout_edit_for_paint())
                .size());
    }
    if (compose_pending_) {
        count += 2;
    } else if (last_compose_result_.has_value()) {
        count += 2 + static_cast<int>(build_exchange_detail_lines(*last_compose_result_, width).size());
    }
    return count;
}

void NetworkPanel::reset_detail_scroll() { detail_scroll_ = 0; }

void NetworkPanel::scroll_list(int delta) {
    if (delta == 0) {
        return;
    }
    const int list_rows = std::max(0, list_area_height() - 1);
    const int item_count = active_view_ == NetworkPanelView::Traffic
                               ? static_cast<int>(session_.exchanges.size())
                               : static_cast<int>(compose_templates_.size());
    const int max_list_scroll = std::max(0, item_count - list_rows);
    list_scroll_ = std::clamp(list_scroll_ + delta, 0, max_list_scroll);
}

void NetworkPanel::scroll_detail(int delta) {
    if (delta == 0) {
        return;
    }
    const int line_count = active_view_ == NetworkPanelView::Traffic ? traffic_detail_line_count(detail_pane_width())
                                                                     : compose_detail_line_count(detail_pane_width());
    const int max_detail_scroll = std::max(0, line_count - detail_pane_height());
    detail_scroll_ = std::clamp(detail_scroll_ + delta, 0, max_detail_scroll);
}

int NetworkPanel::row_at_list_y(int local_y) const {
    const int top = list_area_top();
    if (local_y <= top) {
        return -1;
    }
    return local_y - top - 1 + list_scroll_;
}

bool NetworkPanel::point_in_list_pane(tuinator::Point local) const {
    const int top = list_area_top();
    const int split = split_column();
    return local.x >= 0 && local.x < split && local.y > top && local.y < bounds_.height;
}

bool NetworkPanel::point_in_detail_pane(tuinator::Point local) const {
    const int top = list_area_top();
    const int split = split_column();
    return local.x > split && local.y > top && local.y < bounds_.height;
}

void NetworkPanel::paint_status_row(tuinator::Canvas& canvas) const {
    std::ostringstream line;
    if (active_view_ == NetworkPanelView::Compose) {
        if (compose_editing_) {
            line << "Compose edit  |  Tab field  |  Enter save  |  Esc cancel";
        } else {
            line << "Compose  |  e edit  n new  r reload  |  s send  |  v traffic";
        }
    } else {
        int pending = 0;
        int captured = 0;
        int compose = 0;
        int demo = 0;
        for (const NetworkExchange& exchange : session_.exchanges) {
            switch (exchange.origin) {
            case NetworkExchangeOrigin::Captured:
                ++captured;
                break;
            case NetworkExchangeOrigin::Compose:
                ++compose;
                break;
            case NetworkExchangeOrigin::Demo:
                ++demo;
                break;
            }
            if (exchange.state == NetworkExchangeState::Pending) {
                ++pending;
            }
        }
        line << (session_.intercept_enabled ? "* Intercept ON" : "o Intercept OFF");
        line << "  |  Proxy " << session_.proxy_address;
        line << "  |  ";
        bool first_count = true;
        const auto append_count = [&](const char* label, int count) {
            if (count <= 0) {
                return;
            }
            if (!first_count) {
                line << ", ";
            }
            line << count << ' ' << label;
            first_count = false;
        };
        append_count("debuggee", captured);
        append_count("compose", compose);
        append_count("demo", demo);
        if (first_count) {
            line << "0 requests";
        }
        if (pending > 0) {
            line << "  |  " << pending << " pending";
        }
        line << "  |  ←/→ panes  |  drag+y copy  |  j/k scroll  |  f/d/e/r  |  v compose";
    }

    const int width = std::max(1, bounds_.width - 2);
    const std::string text = line.str();
    const std::size_t bytes = tuinator::text_byte_length_for_width(text, width);
    canvas.draw_text({1, 0}, text.substr(0, bytes), theme_.row_action_muted);
}

void NetworkPanel::paint_tab_row(tuinator::Canvas& canvas) const {
    const auto draw_tab = [&](int x, std::string_view label, bool active) {
        canvas.draw_text({x, kStatusRows}, label, active ? theme_.title_watches : theme_.row_action_muted);
    };

    int x = 1;
    draw_tab(x, "Traffic", active_view_ == NetworkPanelView::Traffic);
    x += tuinator::text_display_width("Traffic") + 2;
    canvas.draw_text({x, kStatusRows}, "|", theme_.row_action_muted);
    x += 2;
    draw_tab(x, "Compose", active_view_ == NetworkPanelView::Compose);
}

void NetworkPanel::paint_traffic_view(tuinator::Canvas& canvas, int top, int height) const {
    const int split = split_column();
    const PaneContentRect list_content = list_content_rect();
    const PaneContentRect detail_content = detail_content_rect();
    const int detail_x = split + 1;

    const std::string list_title =
        focus_pane_ == NetworkFocusPane::List && is_focused() ? "Requests *" : "Requests";
    canvas.draw_text({1, top}, list_title, theme_.scope_header);
    if (detail_x < bounds_.width) {
        const std::string detail_title =
            focus_pane_ == NetworkFocusPane::Detail && is_focused() ? "Details *" : "Details";
        canvas.draw_text({detail_x, top}, detail_title, theme_.scope_header);
        if (split < bounds_.width) {
            for (int row = top; row < top + height; ++row) {
                canvas.draw_text({split, row}, "|", theme_.divider);
            }
        }
    }

    const int list_rows = list_content.height;
    const int exchange_count = static_cast<int>(session_.exchanges.size());
    if (exchange_count == 0) {
        canvas.draw_text({list_content.x, list_content.y}, "(no requests yet)", theme_.label);
    } else {
        for (int row = 0; row < list_rows; ++row) {
            const int index = row + list_scroll_;
            if (index < 0 || index >= exchange_count) {
                break;
            }
            const bool selected = is_focused() && focus_pane_ == NetworkFocusPane::List && index == selected_index_;
            const std::string line =
                format_list_row(session_.exchanges[static_cast<std::size_t>(index)], list_content.width);
            const std::string prefix = selected ? "> " : "  ";
            const tuinator::Style style = selected ? theme_.selection : theme_.label;
            const int content_width = std::max(0, list_content.width - static_cast<int>(prefix.size()));
            const std::string display = prefix + line;
            paint_line_with_selection(canvas, list_content.x, list_content.y + row, display, style,
                                      NetworkFocusPane::List, index, content_width);
        }
    }
    paint_scrollbar(canvas, list_content, exchange_count, list_scroll_, list_scrollbar_state_);

    if (const NetworkExchange* exchange = selected_exchange(); exchange != nullptr && detail_content.width > 0) {
        paint_exchange_detail(canvas, detail_content.x, detail_content.y, detail_content.width, detail_content.height,
                              *exchange);
        paint_scrollbar(canvas, detail_content, traffic_detail_line_count(detail_content.width), detail_scroll_,
                        detail_scrollbar_state_);
    } else if (detail_content.width > 0) {
        canvas.draw_text({detail_content.x, detail_content.y}, "(select a request)", theme_.label);
    }
}

void NetworkPanel::paint_compose_view(tuinator::Canvas& canvas, int top, int height) const {
    const int split = split_column();
    const PaneContentRect list_content = list_content_rect();
    const PaneContentRect detail_content = detail_content_rect();
    const int detail_x = split + 1;

    const std::string list_title =
        focus_pane_ == NetworkFocusPane::List && is_focused() ? "Templates *" : "Templates";
    canvas.draw_text({1, top}, list_title, theme_.scope_header);
    if (detail_x < bounds_.width) {
        const std::string detail_title =
            focus_pane_ == NetworkFocusPane::Detail && is_focused() ? "Request *" : "Request";
        canvas.draw_text({detail_x, top}, detail_title, theme_.scope_header);
        for (int row = top; row < top + height && split < bounds_.width; ++row) {
            canvas.draw_text({split, row}, "|", theme_.divider);
        }
    }

    const int template_count = static_cast<int>(compose_templates_.size());
    for (int row = 0; row < list_content.height; ++row) {
        const int index = row + list_scroll_;
        if (index < 0 || index >= template_count) {
            break;
        }
        const bool selected =
            is_focused() && focus_pane_ == NetworkFocusPane::List && index == selected_template_index_;
        const NetworkComposeTemplate& item = compose_templates_[static_cast<std::size_t>(index)];
        std::string line = item.method + "  " + item.name;
        if (list_content.width > 0 && tuinator::text_display_width(line) > list_content.width) {
            const std::size_t bytes = tuinator::text_byte_length_for_width(line, list_content.width - 1);
            line = line.substr(0, bytes) + "~";
        }
        const std::string prefix = selected ? "> " : "  ";
        const tuinator::Style style = selected ? theme_.selection : theme_.label;
        const int content_width = std::max(0, list_content.width - static_cast<int>(prefix.size()));
        paint_line_with_selection(canvas, list_content.x, list_content.y + row, prefix + line, style,
                                  NetworkFocusPane::List, index, content_width);
    }
    paint_scrollbar(canvas, list_content, template_count, list_scroll_, list_scrollbar_state_);

    if (detail_content.width <= 0) {
        return;
    }

    std::vector<DetailDisplayLine> lines;
    if (selected_template_index_ >= 0 && selected_template_index_ < template_count) {
        const NetworkComposeTemplate& item = compose_templates_[static_cast<std::size_t>(selected_template_index_)];
        lines = build_compose_detail_lines(item, detail_content.width,
                                           compose_editing_ ? std::optional(compose_edit_field_) : std::nullopt,
                                           compose_timeout_edit_for_paint());
    }
    if (compose_pending_) {
        lines.push_back({"", DetailLineKind::Body});
        if (selected_template_index_ >= 0 && selected_template_index_ < template_count) {
            const int timeout_ms =
                compose_templates_[static_cast<std::size_t>(selected_template_index_)].timeout_ms;
            lines.push_back({"Waiting for response (timeout " + std::to_string(timeout_ms / 1000) + "s)…",
                             DetailLineKind::Section});
        } else {
            lines.push_back({"Waiting for response…", DetailLineKind::Section});
        }
    } else if (last_compose_result_.has_value()) {
        lines.push_back({"", DetailLineKind::Body});
        lines.push_back({"Last send result", DetailLineKind::Section});
        const std::vector<DetailDisplayLine> result_lines =
            build_exchange_detail_lines(*last_compose_result_, detail_content.width);
        lines.insert(lines.end(), result_lines.begin(), result_lines.end());
    }

    int row = 0;
    for (std::size_t i = static_cast<std::size_t>(detail_scroll_); i < lines.size() && row < detail_content.height;
         ++i, ++row) {
        paint_line_with_selection(canvas, detail_content.x, detail_content.y + row, lines[i].text,
                                  detail_line_style(theme_, lines[i].kind), NetworkFocusPane::Detail,
                                  static_cast<int>(i), detail_content.width);
    }
    paint_scrollbar(canvas, detail_content, static_cast<int>(lines.size()), detail_scroll_, detail_scrollbar_state_);
}

void NetworkPanel::paint_exchange_detail(tuinator::Canvas& canvas, int x, int y, int width, int height,
                                         const NetworkExchange& exchange) const {
    const std::vector<DetailDisplayLine> lines = build_exchange_detail_lines(exchange, width);
    int row = 0;
    for (std::size_t i = static_cast<std::size_t>(detail_scroll_); i < lines.size() && row < height; ++i, ++row) {
        paint_line_with_selection(canvas, x, y + row, lines[i].text, detail_line_style(theme_, lines[i].kind),
                                  NetworkFocusPane::Detail, static_cast<int>(i), width);
    }
}

void NetworkPanel::sync_detail_scroll() {
    const int list_rows = std::max(0, list_area_height() - 1);
    const int item_count = active_view_ == NetworkPanelView::Traffic
                               ? static_cast<int>(session_.exchanges.size())
                               : static_cast<int>(compose_templates_.size());
    const int selected = active_view_ == NetworkPanelView::Traffic ? selected_index_ : selected_template_index_;
    const int max_list_scroll = std::max(0, item_count - list_rows);
    list_scroll_ = std::clamp(list_scroll_, 0, max_list_scroll);
    if (focus_pane_ == NetworkFocusPane::List) {
        if (selected < list_scroll_) {
            list_scroll_ = selected;
        } else if (selected >= list_scroll_ + list_rows) {
            list_scroll_ = std::max(0, selected - list_rows + 1);
        }
    }

    const int line_count = active_view_ == NetworkPanelView::Traffic ? traffic_detail_line_count(detail_pane_width())
                                                                     : compose_detail_line_count(detail_pane_width());
    const int max_detail_scroll = std::max(0, line_count - detail_pane_height());
    detail_scroll_ = std::clamp(detail_scroll_, 0, max_detail_scroll);
}

bool NetworkPanel::handle_traffic_key(const tuinator::KeyPress& key) {
    if (key.character == 'y' || key.character == 'Y') {
        return copy_selection();
    }
    if (key.key == tuinator::Key::Left) {
        focus_pane_ = NetworkFocusPane::List;
        sync_detail_scroll();
        mark_dirty();
        return true;
    }
    if (key.key == tuinator::Key::Right) {
        focus_pane_ = NetworkFocusPane::Detail;
        sync_detail_scroll();
        mark_dirty();
        return true;
    }
    if (focus_pane_ == NetworkFocusPane::Detail) {
        if (key.key == tuinator::Key::Up || key.character == 'k') {
            scroll_detail(-1);
            mark_dirty();
            return true;
        }
        if (key.key == tuinator::Key::Down || key.character == 'j') {
            scroll_detail(1);
            mark_dirty();
            return true;
        }
        if (key.key == tuinator::Key::PageUp) {
            scroll_detail(-detail_pane_height());
            mark_dirty();
            return true;
        }
        if (key.key == tuinator::Key::PageDown) {
            scroll_detail(detail_pane_height());
            mark_dirty();
            return true;
        }
        return false;
    }

    if (key.key == tuinator::Key::Up || key.character == 'k') {
        if (selected_index_ > 0) {
            --selected_index_;
            reset_detail_scroll();
            sync_detail_scroll();
            mark_dirty();
        }
        return true;
    }
    if (key.key == tuinator::Key::Down || key.character == 'j') {
        if (selected_index_ + 1 < static_cast<int>(session_.exchanges.size())) {
            ++selected_index_;
            reset_detail_scroll();
            sync_detail_scroll();
            mark_dirty();
        }
        return true;
    }
    if (key.key == tuinator::Key::PageUp) {
        scroll_list(-list_area_height());
        mark_dirty();
        return true;
    }
    if (key.key == tuinator::Key::PageDown) {
        scroll_list(list_area_height());
        mark_dirty();
        return true;
    }
    if (key.character == 'v' || key.character == 'V') {
        cycle_view();
        return true;
    }
    if (key.key == tuinator::Key::Enter) {
        if (const NetworkExchange* exchange = selected_exchange();
            exchange != nullptr && exchange->state == NetworkExchangeState::Pending) {
            return perform_action('f');
        }
        return true;
    }

    const char action = static_cast<char>(std::tolower(static_cast<unsigned char>(key.character)));
    if (action == 'f' || action == 'd' || action == 'e' || action == 'r') {
        return perform_action(action);
    }
    return false;
}

bool NetworkPanel::handle_compose_key(const tuinator::KeyPress& key) {
    if (compose_editing_) {
        return handle_compose_edit_key(key);
    }
    if (key.character == 'y' || key.character == 'Y') {
        return copy_selection();
    }
    if (key.key == tuinator::Key::Left) {
        focus_pane_ = NetworkFocusPane::List;
        sync_detail_scroll();
        mark_dirty();
        return true;
    }
    if (key.key == tuinator::Key::Right) {
        focus_pane_ = NetworkFocusPane::Detail;
        sync_detail_scroll();
        mark_dirty();
        return true;
    }
    if (focus_pane_ == NetworkFocusPane::Detail && !compose_pending_) {
        if (key.key == tuinator::Key::Up || key.character == 'k') {
            scroll_detail(-1);
            mark_dirty();
            return true;
        }
        if (key.key == tuinator::Key::Down || key.character == 'j') {
            scroll_detail(1);
            mark_dirty();
            return true;
        }
        if (key.key == tuinator::Key::PageUp) {
            scroll_detail(-detail_pane_height());
            mark_dirty();
            return true;
        }
        if (key.key == tuinator::Key::PageDown) {
            scroll_detail(detail_pane_height());
            mark_dirty();
            return true;
        }
        return false;
    }

    if (key.key == tuinator::Key::Up || key.character == 'k') {
        if (selected_template_index_ > 0) {
            --selected_template_index_;
            apply_compose_template(selected_template_index_);
            reset_detail_scroll();
            sync_detail_scroll();
            mark_dirty();
        }
        return true;
    }
    if (key.key == tuinator::Key::Down || key.character == 'j') {
        if (selected_template_index_ + 1 < static_cast<int>(compose_templates_.size())) {
            ++selected_template_index_;
            apply_compose_template(selected_template_index_);
            reset_detail_scroll();
            sync_detail_scroll();
            mark_dirty();
        }
        return true;
    }
    if (key.character == 'v' || key.character == 'V') {
        cycle_view();
        return true;
    }
    if (key.character == 's' || key.character == 'S' || key.key == tuinator::Key::Enter) {
        return send_compose();
    }
    return false;
}

bool NetworkPanel::handle_mouse(const tuinator::MouseEvent& mouse) {
    if (!bounds_.contains(mouse.position)) {
        return false;
    }

    const tuinator::Point local{mouse.position.x - bounds_.x, mouse.position.y - bounds_.y};

    if (is_pointer_pick(mouse) && local.y == kStatusRows) {
        const int compose_x =
            1 + tuinator::text_display_width("Traffic") + 2 + tuinator::text_display_width("|") + 2;
        set_view(local.x < compose_x ? NetworkPanelView::Traffic : NetworkPanelView::Compose);
        set_focused(true);
        return true;
    }

    const PaneContentRect list_content = list_content_rect();
    const PaneContentRect detail_content = detail_content_rect();
    const int list_count = active_view_ == NetworkPanelView::Traffic
                               ? static_cast<int>(session_.exchanges.size())
                               : static_cast<int>(compose_templates_.size());
    const int detail_count = active_view_ == NetworkPanelView::Traffic
                                 ? traffic_detail_line_count(detail_content.width)
                                 : compose_detail_line_count(detail_content.width);

    if (handle_scrollbar_mouse(mouse, list_content, list_count, list_scroll_, list_scrollbar_state_)) {
        set_focused(true);
        focus_pane_ = NetworkFocusPane::List;
        sync_detail_scroll();
        return true;
    }
    if (handle_scrollbar_mouse(mouse, detail_content, detail_count, detail_scroll_, detail_scrollbar_state_)) {
        set_focused(true);
        focus_pane_ = NetworkFocusPane::Detail;
        sync_detail_scroll();
        return true;
    }

    if (mouse.action == tuinator::MouseAction::Move && mouse.left_pressed) {
        if (text_selection_.has_value() && text_selection_->dragging) {
            update_selection(local);
            return true;
        }
    }
    if (mouse.action == tuinator::MouseAction::Release) {
        if (text_selection_.has_value() && text_selection_->dragging) {
            end_selection();
            return true;
        }
    }

    if (is_wheel_action(mouse.action)) {
        set_focused(true);
        if (point_in_content(detail_content.x, detail_content.y, detail_content.width, detail_content.height, local)) {
            focus_pane_ = NetworkFocusPane::Detail;
            scroll_detail(mouse.action == tuinator::MouseAction::WheelUp ? -1 : 1);
        } else if (point_in_content(list_content.x, list_content.y, list_content.width, list_content.height, local)) {
            focus_pane_ = NetworkFocusPane::List;
            scroll_list(mouse.action == tuinator::MouseAction::WheelUp ? -1 : 1);
        }
        sync_detail_scroll();
        mark_dirty();
        return true;
    }

    if (!is_pointer_pick(mouse)) {
        return false;
    }

    if (mouse.button == tuinator::MouseButton::Middle) {
        if (point_in_content(detail_content.x, detail_content.y, detail_content.width, detail_content.height, local)) {
            focus_pane_ = NetworkFocusPane::Detail;
            begin_selection(NetworkFocusPane::Detail, local);
            end_selection();
            copy_selection();
            set_focused(true);
            mark_dirty();
            return true;
        }
        if (point_in_content(list_content.x, list_content.y, list_content.width, list_content.height, local)) {
            focus_pane_ = NetworkFocusPane::List;
            begin_selection(NetworkFocusPane::List, local);
            end_selection();
            copy_selection();
            set_focused(true);
            mark_dirty();
            return true;
        }
    }

    if (mouse.button == tuinator::MouseButton::Left &&
        mouse.action == tuinator::MouseAction::Press) {
        if (point_in_content(detail_content.x, detail_content.y, detail_content.width, detail_content.height, local)) {
            focus_pane_ = NetworkFocusPane::Detail;
            begin_selection(NetworkFocusPane::Detail, local);
            set_focused(true);
            mark_dirty();
            return true;
        }
        if (point_in_content(list_content.x, list_content.y, list_content.width, list_content.height, local)) {
            focus_pane_ = NetworkFocusPane::List;
            begin_selection(NetworkFocusPane::List, local);
            const int index = row_at_list_y(local.y);
            if (active_view_ == NetworkPanelView::Traffic && index >= 0 &&
                index < static_cast<int>(session_.exchanges.size())) {
                selected_index_ = index;
                reset_detail_scroll();
            } else if (active_view_ == NetworkPanelView::Compose && index >= 0 &&
                       index < static_cast<int>(compose_templates_.size())) {
                selected_template_index_ = index;
                apply_compose_template(index);
                reset_detail_scroll();
            }
            sync_detail_scroll();
            set_focused(true);
            mark_dirty();
            return true;
        }
    }

    if (point_in_content(detail_content.x, detail_content.y, detail_content.width, detail_content.height, local)) {
        focus_pane_ = NetworkFocusPane::Detail;
        set_focused(true);
        mark_dirty();
        return true;
    }

    if (local.y >= list_area_top()) {
        set_focused(true);
        return true;
    }

    return false;
}

void NetworkPanel::apply_compose_template(int index) {
    if (index < 0 || index >= static_cast<int>(compose_templates_.size())) {
        return;
    }
    selected_template_index_ = index;
    reset_detail_scroll();
}

std::string& NetworkPanel::compose_edit_field_value() {
    NetworkComposeTemplate& item = compose_templates_[static_cast<std::size_t>(selected_template_index_)];
    switch (compose_edit_field_) {
    case ComposeEditField::Name:
        return item.name;
    case ComposeEditField::Method:
        return item.method;
    case ComposeEditField::Url:
        return item.url;
    case ComposeEditField::Headers:
        return item.headers;
    case ComposeEditField::Body:
        return item.body;
    case ComposeEditField::Timeout:
        return compose_timeout_edit_;
    }
    return item.url;
}

const std::string* NetworkPanel::compose_timeout_edit_for_paint() const {
    if (compose_editing_ && compose_edit_field_ == ComposeEditField::Timeout) {
        return &compose_timeout_edit_;
    }
    return nullptr;
}

void NetworkPanel::flush_compose_edit_field() {
    if (!compose_editing_ || selected_template_index_ < 0 ||
        selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        return;
    }
    if (compose_edit_field_ == ComposeEditField::Timeout) {
        NetworkComposeTemplate& item = compose_templates_[static_cast<std::size_t>(selected_template_index_)];
        item.timeout_ms = parse_compose_timeout_ms(compose_timeout_edit_);
    }
}

void NetworkPanel::prepare_compose_edit_field() {
    if (!compose_editing_ || selected_template_index_ < 0 ||
        selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        return;
    }
    if (compose_edit_field_ == ComposeEditField::Timeout) {
        compose_timeout_edit_ =
            std::to_string(compose_templates_[static_cast<std::size_t>(selected_template_index_)].timeout_ms);
    }
}

void NetworkPanel::cycle_compose_edit_field(int delta) {
    flush_compose_edit_field();
    constexpr int field_count = 6;
    int index = static_cast<int>(compose_edit_field_);
    index = (index + delta + field_count * 16) % field_count;
    compose_edit_field_ = static_cast<ComposeEditField>(index);
    prepare_compose_edit_field();
}


bool NetworkPanel::begin_compose_edit() {
    if (selected_template_index_ < 0 || selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        return false;
    }
    compose_edit_backup_ = compose_templates_[static_cast<std::size_t>(selected_template_index_)];
    compose_editing_ = true;
    compose_edit_field_ = ComposeEditField::Url;
    prepare_compose_edit_field();
    focus_pane_ = NetworkFocusPane::Detail;
    if (on_message_) {
        on_message_("Editing template — Enter saves to " + compose_templates_path_.string());
    }
    mark_dirty();
    return true;
}

bool NetworkPanel::commit_compose_edit() {
    if (!compose_editing_) {
        return false;
    }
    flush_compose_edit_field();
    compose_editing_ = false;
    compose_edit_backup_.reset();
    std::string error;
    if (!save_compose_templates(compose_templates_path_, compose_templates_, error)) {
        if (on_message_) {
            on_message_("Template updated (not saved: " + error + ")");
        }
    } else if (on_message_) {
        on_message_("Saved compose templates");
    }
    mark_dirty();
    return true;
}

void NetworkPanel::cancel_compose_edit() {
    if (!compose_editing_) {
        return;
    }
    if (compose_edit_backup_.has_value() && selected_template_index_ >= 0 &&
        selected_template_index_ < static_cast<int>(compose_templates_.size())) {
        compose_templates_[static_cast<std::size_t>(selected_template_index_)] = *compose_edit_backup_;
    }
    compose_editing_ = false;
    compose_edit_backup_.reset();
    compose_timeout_edit_.clear();
    mark_dirty();
}

bool NetworkPanel::handle_compose_edit_key(const tuinator::KeyPress& key) {
    if (key.key == tuinator::Key::Escape) {
        cancel_compose_edit();
        if (on_message_) {
            on_message_("Edit cancelled");
        }
        return true;
    }
    if (key.key == tuinator::Key::Tab) {
        cycle_compose_edit_field(key.shift ? -1 : 1);
        mark_dirty();
        return true;
    }
    if (key.key == tuinator::Key::Enter) {
        return commit_compose_edit();
    }
    if (key.key == tuinator::Key::Backspace) {
        std::string& value = compose_edit_field_value();
        if (!value.empty()) {
            value.pop_back();
        }
        mark_dirty();
        return true;
    }
    if (key.character >= 32 && key.character != 127) {
        if (compose_edit_field_ == ComposeEditField::Timeout &&
            !std::isdigit(static_cast<unsigned char>(key.character))) {
            return true;
        }
        std::string& value = compose_edit_field_value();
        value.push_back(static_cast<char>(key.character));
        mark_dirty();
        return true;
    }
    return true;
}

bool NetworkPanel::add_compose_template() {
    compose_templates_.push_back({
        .name = "New request",
        .method = "GET",
        .url = "http://localhost:8080/",
        .headers = "",
        .body = "",
        .timeout_ms = 30000,
    });
    selected_template_index_ = static_cast<int>(compose_templates_.size()) - 1;
    reset_detail_scroll();
    return begin_compose_edit();
}

bool NetworkPanel::delete_compose_template() {
    if (compose_templates_.size() <= 1) {
        if (on_message_) {
            on_message_("At least one template must remain");
        }
        return false;
    }
    if (selected_template_index_ < 0 || selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        return false;
    }
    compose_templates_.erase(compose_templates_.begin() + selected_template_index_);
    if (selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        selected_template_index_ = static_cast<int>(compose_templates_.size()) - 1;
    }
    compose_editing_ = false;
    compose_edit_backup_.reset();
    std::string error;
    if (!save_compose_templates(compose_templates_path_, compose_templates_, error) && on_message_) {
        on_message_("Deleted template (not saved: " + error + ")");
    } else if (on_message_) {
        on_message_("Deleted template");
    }
    mark_dirty();
    return true;
}

bool NetworkPanel::reload_compose_templates() {
    cancel_compose_edit();
    load_compose_templates_from_disk();
    if (on_message_) {
        on_message_("Reloaded compose templates from " + compose_templates_path_.string());
    }
    return true;
}

bool NetworkPanel::send_compose() {
    if (selected_template_index_ < 0 || selected_template_index_ >= static_cast<int>(compose_templates_.size())) {
        return false;
    }
    const NetworkComposeTemplate& item = compose_templates_[static_cast<std::size_t>(selected_template_index_)];
    if (!on_compose_send_) {
        if (on_message_) {
            on_message_("Compose send is not available");
        }
        return false;
    }
    if (!on_compose_send_(item)) {
        return false;
    }
    focus_pane_ = NetworkFocusPane::List;
    mark_dirty();
    return true;
}

std::string NetworkPanel::format_list_row(const NetworkExchange& exchange, int max_width) const {
    std::ostringstream row;
    if (exchange.origin == NetworkExchangeOrigin::Demo) {
        row << "~demo ";
    } else if (exchange.origin == NetworkExchangeOrigin::Compose) {
        row << "+ ";
    } else if (exchange.state == NetworkExchangeState::Pending) {
        row << "! ";
    } else if (exchange.state == NetworkExchangeState::Dropped) {
        row << "x ";
    } else {
        row << "  ";
    }
    row << exchange.method;
    if (exchange.state == NetworkExchangeState::Pending) {
        row << " ---";
    } else if (exchange.state == NetworkExchangeState::Dropped) {
        row << " FAILED";
    } else if (exchange.status_code > 0) {
        row << ' ' << exchange.status_code;
    }
    if (exchange.duration_ms > 0 && max_width >= 28) {
        row << ' ' << exchange.duration_ms << "ms";
    }
    row << ' ' << exchange.path;

    std::string text = row.str();
    if (max_width > 2 && tuinator::text_display_width(text) > max_width) {
        const std::size_t bytes = tuinator::text_byte_length_for_width(text, max_width - 1);
        text = text.substr(0, bytes) + "~";
    }
    return text;
}

std::string NetworkPanel::path_from_url(const std::string& url) const {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return url;
    }
    const std::size_t path_start = url.find('/', scheme + 3);
    if (path_start == std::string::npos) {
        return "/";
    }
    return url.substr(path_start);
}

}  // namespace tui_debug_ui
