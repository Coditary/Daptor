#include "tui_debug_ui/breakpoints_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/layout/box.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace tui_debug_ui {

namespace {

std::string basename_from_path(const std::string& path) {
    const std::size_t slash = path.find_last_of('/');
    if (slash == std::string::npos) {
        return path;
    }
    return path.substr(slash + 1);
}

std::string friendly_data_access_type(const std::string& access_type) {
    if (access_type == "readWrite") {
        return "read/write";
    }
    return access_type;
}

}  // namespace

BreakpointsPanel::BreakpointsPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                                   const std::string& title) {
    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::Breakpoints, &theme);
    list_->set_row_action_layout(ListRowActionLayout::BreakpointRow);
    list_->set_on_row_action([this](int index, RowActionType action, tuinator::Point anchor) {
        const BreakpointRow* row = breakpoint_at_display_index(index);
        if (row == nullptr) {
            return;
        }

        switch (display_kind_at(index)) {
        case DisplayLineKind::WhenCondition:
            if (action == RowActionType::Edit && on_edit_when_condition_ != nullptr) {
                on_edit_when_condition_(*row, anchor);
            } else if (action == RowActionType::Remove && on_clear_when_condition_ != nullptr) {
                on_clear_when_condition_(*row);
            }
            return;
        case DisplayLineKind::HitCondition:
            if (action == RowActionType::Edit && on_edit_hit_condition_ != nullptr) {
                on_edit_hit_condition_(*row, anchor);
            } else if (action == RowActionType::Remove && on_clear_hit_condition_ != nullptr) {
                on_clear_hit_condition_(*row);
            }
            return;
        case DisplayLineKind::DataBreakpoint:
        case DisplayLineKind::FunctionBreakpoint:
        case DisplayLineKind::ExceptionBreakpoint:
            if (action == RowActionType::Remove && on_remove_ != nullptr) {
                on_remove_(*row);
            } else if (action == RowActionType::Add) {
                if (row->exception_enabled && on_add_condition_ != nullptr) {
                    on_add_condition_(*row, index, anchor);
                } else if (!row->exception_enabled && on_activate_ != nullptr) {
                    on_activate_(*row);
                }
            }
            return;
        case DisplayLineKind::Breakpoint:
            if (action == RowActionType::Remove && on_remove_ != nullptr) {
                on_remove_(*row);
            } else if (action == RowActionType::Add && on_add_condition_ != nullptr) {
                on_add_condition_(*row, index, anchor);
            }
            return;
        case DisplayLineKind::None:
        case DisplayLineKind::WhenConditionEditing:
        case DisplayLineKind::HitConditionEditing:
            break;
        }
    });
    list_->set_on_row_click([this](int index, const std::string& /*item*/) {
        const DisplayLineKind kind = display_kind_at(index);
        if (kind == DisplayLineKind::GroupHeader) {
            return try_toggle_expand(index);
        }
        const BreakpointRow* row = breakpoint_at_display_index(index);
        if (row != nullptr && row_has_collapsible_conditions(*row)) {
            return try_toggle_expand(index);
        }
        return false;
    });
    list_->set_on_activate([this](int index) {
        if (display_kind_at(index) == DisplayLineKind::GroupHeader) {
            (void)try_toggle_expand(index);
            return;
        }
        const BreakpointRow* row = breakpoint_at_display_index(index);
        if (row != nullptr && on_activate_ != nullptr) {
            on_activate_(*row);
        }
    });
    list_->set_on_row_context([this](int index, const std::string& /*item*/, tuinator::Point anchor) {
        const BreakpointRow* row = breakpoint_at_display_index(index);
        if (row != nullptr && on_context_ != nullptr) {
            on_context_(*row, anchor);
        }
    });
    list_->set_on_inline_edit_change([this](const std::string& value) {
        if (inline_edit_.active) {
            inline_edit_.value = value;
        }
        if (on_change_ != nullptr) {
            on_change_(value);
        }
    });
    list_->set_on_inline_edit_submit([this](const std::string& value) {
        if (on_submit_ != nullptr) {
            on_submit_(value);
        }
    });
    list_->set_on_inline_edit_cancel([this]() {
        if (on_inline_edit_cancel_ != nullptr) {
            on_inline_edit_cancel_();
        }
    });

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(list), theme.title_breakpoints, theme.panel_background,
                                               std::move(scroll_options), true);
    if (tuinator::ScrollView* scroll = pane_->scroll_view()) {
        list_->set_scroll_parent(scroll);
    }
}

std::unique_ptr<tuinator::Widget> BreakpointsPanel::release_widget() { return pane_->release_widget(); }

bool BreakpointsPanel::paths_match(const std::string& left, const std::string& right) {
    return left == right;
}

void BreakpointsPanel::set_breakpoints(std::vector<BreakpointRow> rows) {
    rows_ = std::move(rows);
    const auto kind_order = [](BreakpointRowKind kind) {
        switch (kind) {
        case BreakpointRowKind::Data:
            return 0;
        case BreakpointRowKind::Function:
            return 1;
        case BreakpointRowKind::Source:
            return 2;
        case BreakpointRowKind::Exception:
            return 3;
        }
        return 3;
    };

    std::sort(rows_.begin(), rows_.end(), [&](const BreakpointRow& left, const BreakpointRow& right) {
        if (left.kind != right.kind) {
            return kind_order(left.kind) < kind_order(right.kind);
        }
        if (left.kind == BreakpointRowKind::Data || left.kind == BreakpointRowKind::Function ||
            left.kind == BreakpointRowKind::Exception) {
            return left.source_text < right.source_text;
        }
        if (left.path != right.path) {
            return left.path < right.path;
        }
        return left.line < right.line;
    });

    rebuild_display();
}

std::string BreakpointsPanel::breakpoint_condition_key(const BreakpointRow& row) {
    if (row.kind == BreakpointRowKind::Exception) {
        return "exception:" + row.data_id;
    }
    return "source:" + row.path + '\x1F' + std::to_string(row.line);
}

bool BreakpointsPanel::row_has_collapsible_conditions(const BreakpointRow& row) const {
    if (row.kind == BreakpointRowKind::Exception) {
        if (!row.condition.empty()) {
            return true;
        }
        return inline_edit_.active && inline_edit_.exception && row.data_id == inline_edit_.exception_filter;
    }
    if (row.kind != BreakpointRowKind::Source) {
        return false;
    }
    if (!row.condition.empty() || !row.hit_condition.empty()) {
        return true;
    }
    return inline_edit_.active && !inline_edit_.exception && paths_match(row.path, inline_edit_.path) &&
           row.line == inline_edit_.line;
}

bool BreakpointsPanel::conditions_expanded_for_row(const BreakpointRow& row) const {
    if (!row_has_collapsible_conditions(row)) {
        return true;
    }
    if (inline_edit_.active) {
        if (row.kind == BreakpointRowKind::Exception && inline_edit_.exception &&
            row.data_id == inline_edit_.exception_filter) {
            return true;
        }
        if (row.kind == BreakpointRowKind::Source && !inline_edit_.exception &&
            paths_match(row.path, inline_edit_.path) && row.line == inline_edit_.line) {
            return true;
        }
    }
    return collapsed_condition_keys_.count(breakpoint_condition_key(row)) == 0;
}

bool BreakpointsPanel::try_toggle_expand(int display_index) {
    if (has_inline_edit()) {
        return false;
    }
    if (display_index < 0 || display_index >= static_cast<int>(display_expand_keys_.size())) {
        return false;
    }

    const std::string& key = display_expand_keys_[static_cast<std::size_t>(display_index)];
    if (key.empty()) {
        return false;
    }

    if (key.rfind("file:", 0) == 0 || key.rfind("section:", 0) == 0) {
        if (collapsed_group_keys_.count(key) > 0) {
            collapsed_group_keys_.erase(key);
        } else {
            collapsed_group_keys_.insert(key);
        }
    } else {
        if (collapsed_condition_keys_.count(key) > 0) {
            collapsed_condition_keys_.erase(key);
        } else {
            collapsed_condition_keys_.insert(key);
        }
    }

    const int previous_index = selected_index();
    rebuild_display();
    if (list_ != nullptr) {
        const int item_count = static_cast<int>(display_to_row_.size());
        if (previous_index >= 0 && previous_index < item_count) {
            list_->set_selected_index(previous_index);
        }
    }
    return true;
}

void BreakpointsPanel::rebuild_display() {
    if (list_ == nullptr) {
        return;
    }

    std::vector<std::string> items;
    std::vector<int> display_to_row;
    std::vector<DisplayLineKind> display_kind;
    std::vector<std::string> display_expand_keys;
    items.reserve(rows_.size() * 2);
    display_to_row.reserve(rows_.size() * 2);
    display_kind.reserve(rows_.size() * 2);
    display_expand_keys.reserve(rows_.size() * 2);
    inline_edit_display_index_ = -1;

    auto section_expanded = [this](const char* section_key) {
        return collapsed_group_keys_.count(std::string("section:") + section_key) == 0;
    };
    auto add_group_header = [&](const std::string& title, const std::string& group_key, bool expanded) {
        if (!items.empty()) {
            items.push_back("");
            display_to_row.push_back(-1);
            display_kind.push_back(DisplayLineKind::None);
            display_expand_keys.push_back({});
        }
        items.push_back(std::string(expanded ? kScopeExpandExpanded : kScopeExpandCollapsed) + title);
        display_to_row.push_back(-1);
        display_kind.push_back(DisplayLineKind::GroupHeader);
        display_expand_keys.push_back(group_key);
    };

    bool data_header_added = false;
    bool function_header_added = false;
    bool exception_header_added = false;
    bool data_section_collapsed = false;
    bool function_section_collapsed = false;
    bool exception_section_collapsed = false;
    std::string current_path;
    bool current_file_collapsed = false;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const BreakpointRow& row = rows_[index];
        if (row.kind == BreakpointRowKind::Data) {
            if (!data_header_added) {
                data_section_collapsed = !section_expanded("data");
                add_group_header("Data:", "section:data", !data_section_collapsed);
                data_header_added = true;
            }
            if (data_section_collapsed) {
                continue;
            }

            std::string entry = "  \u2295 " + row.source_text;
            if (!row.access_type.empty()) {
                entry += " \u00b7 " + friendly_data_access_type(row.access_type);
            }
            items.push_back(std::move(entry));
            display_to_row.push_back(static_cast<int>(index));
            display_kind.push_back(DisplayLineKind::DataBreakpoint);
            display_expand_keys.push_back({});
            continue;
        }

        if (row.kind == BreakpointRowKind::Function) {
            if (!function_header_added) {
                function_section_collapsed = !section_expanded("function");
                add_group_header("Functions:", "section:function", !function_section_collapsed);
                function_header_added = true;
            }
            if (function_section_collapsed) {
                continue;
            }

            std::string entry = "  \u0192 " + row.source_text;
            items.push_back(std::move(entry));
            display_to_row.push_back(static_cast<int>(index));
            display_kind.push_back(DisplayLineKind::FunctionBreakpoint);
            display_expand_keys.push_back({});
            continue;
        }

        if (row.kind == BreakpointRowKind::Exception) {
            if (!exception_header_added) {
                exception_section_collapsed = !section_expanded("exception");
                if (inline_edit_.active && inline_edit_.exception) {
                    exception_section_collapsed = false;
                }
                add_group_header("Exceptions:", "section:exception", !exception_section_collapsed);
                exception_header_added = true;
            }
            if (exception_section_collapsed) {
                continue;
            }

            const bool editing_when = inline_edit_.active && inline_edit_.exception &&
                                      row.data_id == inline_edit_.exception_filter;
            const bool has_collapsible = row_has_collapsible_conditions(row);
            const bool conditions_expanded = conditions_expanded_for_row(row);

            std::string entry = "  ";
            if (has_collapsible) {
                entry += conditions_expanded ? kScopeExpandExpanded : kScopeExpandCollapsed;
            }
            entry += row.exception_enabled ? "\u2713 " : "\u00b7 ";
            entry += row.source_text;
            items.push_back(std::move(entry));
            display_to_row.push_back(static_cast<int>(index));
            display_kind.push_back(DisplayLineKind::ExceptionBreakpoint);
            display_expand_keys.push_back(has_collapsible ? breakpoint_condition_key(row) : std::string{});

            if (conditions_expanded) {
                if (editing_when) {
                    items.push_back(kInlineWhenEditRow);
                    display_to_row.push_back(static_cast<int>(index));
                    display_kind.push_back(DisplayLineKind::WhenConditionEditing);
                    display_expand_keys.push_back({});
                    inline_edit_display_index_ = static_cast<int>(items.size()) - 1;
                } else if (!row.condition.empty()) {
                    items.push_back("    when " + row.condition);
                    display_to_row.push_back(static_cast<int>(index));
                    display_kind.push_back(DisplayLineKind::WhenCondition);
                    display_expand_keys.push_back({});
                }
            }
            continue;
        }

        if (row.kind != BreakpointRowKind::Source) {
            continue;
        }

        if (row.path != current_path) {
            current_path = row.path;
            const std::string file_group_key = "file:" + row.path;
            current_file_collapsed = collapsed_group_keys_.count(file_group_key) > 0;
            add_group_header(basename_from_path(row.path) + ":", file_group_key, !current_file_collapsed);
        }

        if (current_file_collapsed) {
            continue;
        }

        const bool editing_when = inline_edit_.active && !inline_edit_.hit &&
                                  paths_match(row.path, inline_edit_.path) && row.line == inline_edit_.line;
        const bool editing_hit = inline_edit_.active && inline_edit_.hit &&
                                 paths_match(row.path, inline_edit_.path) && row.line == inline_edit_.line;
        const bool has_collapsible = row_has_collapsible_conditions(row);
        const bool conditions_expanded = conditions_expanded_for_row(row);

        std::string entry = "  ";
        if (has_collapsible) {
            entry += conditions_expanded ? kScopeExpandExpanded : kScopeExpandCollapsed;
        }
        entry += std::to_string(row.line);
        if (!row.source_text.empty()) {
            entry += " " + row.source_text;
        }
        if (row.hit_condition.empty() && row.hit_count > 0) {
            entry += "  (" + std::to_string(row.hit_count) + "×)";
        }
        items.push_back(std::move(entry));
        display_to_row.push_back(static_cast<int>(index));
        display_kind.push_back(DisplayLineKind::Breakpoint);
        display_expand_keys.push_back(has_collapsible ? breakpoint_condition_key(row) : std::string{});

        if (conditions_expanded) {
            if (editing_when) {
                items.push_back(kInlineWhenEditRow);
                display_to_row.push_back(static_cast<int>(index));
                display_kind.push_back(DisplayLineKind::WhenConditionEditing);
                display_expand_keys.push_back({});
                inline_edit_display_index_ = static_cast<int>(items.size()) - 1;
            } else if (!row.condition.empty()) {
                items.push_back("    when " + row.condition);
                display_to_row.push_back(static_cast<int>(index));
                display_kind.push_back(DisplayLineKind::WhenCondition);
                display_expand_keys.push_back({});
            }

            if (editing_hit) {
                items.push_back(kInlineHitEditRow);
                display_to_row.push_back(static_cast<int>(index));
                display_kind.push_back(DisplayLineKind::HitConditionEditing);
                display_expand_keys.push_back({});
                inline_edit_display_index_ = static_cast<int>(items.size()) - 1;
            } else if (!row.hit_condition.empty()) {
                std::string hit_line = "    hit " + row.hit_condition + " (" + std::to_string(row.hit_count) + ")";
                items.push_back(std::move(hit_line));
                display_to_row.push_back(static_cast<int>(index));
                display_kind.push_back(DisplayLineKind::HitCondition);
                display_expand_keys.push_back({});
            }
        }
    }

    display_to_row_ = std::move(display_to_row);
    display_kind_ = std::move(display_kind);
    display_expand_keys_ = std::move(display_expand_keys);
    list_->assign_items(std::move(items));
    sync_inline_edit_to_list();
    list_->mark_dirty();
    if (pane_ != nullptr) {
        pane_->refresh_scroll_content();
    }
}

void BreakpointsPanel::sync_inline_edit_to_list() {
    if (list_ == nullptr || !inline_edit_.active || inline_edit_display_index_ < 0) {
        if (list_ != nullptr) {
            list_->clear_inline_row_edit();
        }
        return;
    }

    const std::string prefix = inline_edit_.hit ? "    hit " : "    when ";
    list_->set_inline_row_edit(inline_edit_display_index_, prefix, inline_edit_.value);
    list_->set_selected_index(inline_edit_display_index_);
    list_->set_focused(true);

    if (tuinator::ScrollView* scroll = scroll_view()) {
        const int scroll_y = std::max(0, inline_edit_display_index_ - 1);
        scroll->scroll_to(0, scroll_y);
    }
}

void BreakpointsPanel::set_inline_edit(const std::string& path, int line, bool hit, std::string value) {
    inline_edit_.path = path;
    inline_edit_.line = line;
    inline_edit_.hit = hit;
    inline_edit_.exception = false;
    inline_edit_.exception_filter.clear();
    inline_edit_.value = std::move(value);
    inline_edit_.active = true;
}

void BreakpointsPanel::set_exception_inline_edit(const std::string& filter, std::string value) {
    inline_edit_.path.clear();
    inline_edit_.line = 0;
    inline_edit_.hit = false;
    inline_edit_.exception = true;
    inline_edit_.exception_filter = filter;
    inline_edit_.value = std::move(value);
    inline_edit_.active = true;
}

void BreakpointsPanel::clear_inline_edit() {
    inline_edit_ = {};
    inline_edit_display_index_ = -1;
    if (list_ != nullptr) {
        list_->clear_inline_row_edit();
        list_->mark_dirty();
    }
}

bool BreakpointsPanel::has_inline_edit() const { return inline_edit_.active; }

std::string BreakpointsPanel::inline_edit_value() const {
    if (list_ != nullptr && list_->has_inline_row_edit()) {
        return list_->inline_row_edit_value();
    }
    return inline_edit_.active ? inline_edit_.value : std::string{};
}

void BreakpointsPanel::focus_inline_edit() {
    if (list_ == nullptr) {
        return;
    }
    sync_inline_edit_to_list();
    list_->set_focused(true);
    list_->mark_dirty();
}

bool BreakpointsPanel::handle_inline_edit_key(const tuinator::Event& event) {
    if (list_ == nullptr || !list_->has_inline_row_edit()) {
        return false;
    }
    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (!list_->is_focused()) {
            list_->set_focused(true);
        }
        return list_->handle_inline_row_edit_key(*key);
    }
    return false;
}

void BreakpointsPanel::set_on_activate(ActivateCallback callback) { on_activate_ = std::move(callback); }

void BreakpointsPanel::set_on_context(ContextCallback callback) { on_context_ = std::move(callback); }

void BreakpointsPanel::set_on_remove(RemoveCallback callback) { on_remove_ = std::move(callback); }

void BreakpointsPanel::set_on_add_condition(AddConditionCallback callback) {
    on_add_condition_ = std::move(callback);
}

void BreakpointsPanel::set_on_edit_when_condition(EditConditionCallback callback) {
    on_edit_when_condition_ = std::move(callback);
}

void BreakpointsPanel::set_on_edit_hit_condition(EditConditionCallback callback) {
    on_edit_hit_condition_ = std::move(callback);
}

void BreakpointsPanel::set_on_clear_when_condition(ClearConditionCallback callback) {
    on_clear_when_condition_ = std::move(callback);
}

void BreakpointsPanel::set_on_clear_hit_condition(ClearConditionCallback callback) {
    on_clear_hit_condition_ = std::move(callback);
}

void BreakpointsPanel::set_on_submit(SubmitCallback callback) { on_submit_ = std::move(callback); }

void BreakpointsPanel::set_on_change(ChangeCallback callback) { on_change_ = std::move(callback); }

void BreakpointsPanel::set_on_inline_edit_cancel(std::function<void()> callback) {
    on_inline_edit_cancel_ = std::move(callback);
}

std::string BreakpointsPanel::input_value() const { return inline_edit_value(); }

void BreakpointsPanel::focus_input() { focus_inline_edit(); }

tuinator::Point BreakpointsPanel::row_anchor(int display_index) const {
    if (list_ == nullptr) {
        return {};
    }

    if (tuinator::ScrollView* scroll = scroll_view()) {
        const tuinator::Rect scroll_bounds = scroll->bounds();
        if (scroll_bounds.width <= 0 || scroll_bounds.height <= 0) {
            return {};
        }

        const int scroll_y = scroll->scroll_y();
        const int viewport_row = display_index - scroll_y;
        const int visible_row = std::clamp(viewport_row, 0, std::max(0, scroll_bounds.height - 1));
        return {scroll_bounds.x + 2, scroll_bounds.y + visible_row};
    }

    const tuinator::Rect list_bounds = list_->bounds();
    if (list_bounds.width <= 0 || list_bounds.height <= 0) {
        return {};
    }

    const int visible_row = std::clamp(display_index, 0, std::max(0, list_bounds.height - 1));
    return {list_bounds.x + 2, list_bounds.y + visible_row};
}

tuinator::Point BreakpointsPanel::row_action_anchor(int display_index, RowActionType action) const {
    if (list_ == nullptr) {
        return {};
    }
    return list_->row_action_anchor(display_index, action);
}

tuinator::Rect BreakpointsPanel::panel_bounds() const {
    if (pane_ == nullptr || pane_->root_widget() == nullptr) {
        return {};
    }
    return pane_->root_widget()->bounds();
}

int BreakpointsPanel::selected_index() const {
    return list_ != nullptr ? list_->selected_index() : -1;
}

const BreakpointRow* BreakpointsPanel::selected_row() const {
    return breakpoint_at_display_index(selected_index());
}

tuinator::Widget* BreakpointsPanel::panel_widget() const {
    return pane_ != nullptr ? pane_->root_widget() : nullptr;
}

tuinator::Widget* BreakpointsPanel::list_widget() const { return list_; }

tuinator::ScrollView* BreakpointsPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

const BreakpointRow* BreakpointsPanel::breakpoint_at_display_index(int index) const {
    if (index < 0 || index >= static_cast<int>(display_to_row_.size())) {
        return nullptr;
    }
    const int row_index = display_to_row_[static_cast<std::size_t>(index)];
    if (row_index < 0 || row_index >= static_cast<int>(rows_.size())) {
        return nullptr;
    }
    return &rows_[static_cast<std::size_t>(row_index)];
}

BreakpointsPanel::DisplayLineKind BreakpointsPanel::display_kind_at(int index) const {
    if (index < 0 || index >= static_cast<int>(display_kind_.size())) {
        return DisplayLineKind::None;
    }
    return display_kind_[static_cast<std::size_t>(index)];
}

}  // namespace tui_debug_ui
