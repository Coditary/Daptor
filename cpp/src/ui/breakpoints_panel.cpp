#include "tui_debug_ui/breakpoints_panel.hpp"

#include "tui_debug_ui/dap_ui_theme.hpp"
#include "tui_debug_ui/navigable_list_view.hpp"
#include "tui_debug_ui/titled_scroll_pane.hpp"

#include <tuinator/layout/box.hpp>
#include <tuinator/widgets/controls/text_input.hpp>

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

bool is_file_header_row(const std::string& line) {
    return !line.empty() && line.back() == ':' && line[0] != ' ';
}

}  // namespace

BreakpointsPanel::BreakpointsPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options,
                                   const std::string& title) {
    auto root = std::make_unique<tuinator::VBox>(tuinator::BoxOptions{.gap = 0, .padding = 0});

    auto input = std::make_unique<tuinator::TextInput>(
        tuinator::TextInputOptions{.placeholder = "> condition (when)"}, theme.label, theme.selection);
    input->set_flex(0);
    input_ = input.get();

    auto list = std::make_unique<NavigableListView>(theme.label, theme.selection, theme.panel_background, true);
    list_ = list.get();
    list_->set_paint_mode(ListPaintMode::Breakpoints, &theme);
    list_->set_row_action_layout(ListRowActionLayout::BreakpointRow);
    list_->set_row_action_edit_state([this](int index) {
        const BreakpointRow* row = breakpoint_at_display_index(index);
        return row != nullptr && !row->condition.empty();
    });
    list_->set_on_row_action([this](int index, RowActionType action) {
        const BreakpointRow* row = breakpoint_at_display_index(index);
        if (row == nullptr) {
            return;
        }
        if (action == RowActionType::Remove && on_remove_ != nullptr) {
            on_remove_(*row);
        } else if ((action == RowActionType::Add || action == RowActionType::Edit) && on_add_condition_ != nullptr) {
            on_add_condition_(*row);
        }
    });
    list_->set_flex(1);
    list_->set_on_activate([this](int index) {
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

    root->add_child(std::move(list));
    root->add_child(std::move(input));

    pane_ = std::make_unique<TitledScrollPane>(title, std::move(root), theme.title_breakpoints, theme.panel_background,
                                               std::move(scroll_options), false);
}

std::unique_ptr<tuinator::Widget> BreakpointsPanel::release_widget() { return pane_->release_widget(); }

void BreakpointsPanel::set_breakpoints(std::vector<BreakpointRow> rows) {
    rows_ = std::move(rows);
    std::sort(rows_.begin(), rows_.end(), [](const BreakpointRow& left, const BreakpointRow& right) {
        if (left.path != right.path) {
            return left.path < right.path;
        }
        return left.line < right.line;
    });

    if (list_ == nullptr) {
        return;
    }

    std::vector<std::string> items;
    std::vector<int> display_to_row;
    items.reserve(rows_.size() * 2);
    display_to_row.reserve(rows_.size() * 2);

    std::string current_path;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        const BreakpointRow& row = rows_[index];
        if (row.path != current_path) {
            if (!items.empty()) {
                items.push_back("");
                display_to_row.push_back(-1);
            }
            items.push_back(basename_from_path(row.path) + ":");
            display_to_row.push_back(-1);
            current_path = row.path;
        }

        std::string entry = "  " + std::to_string(row.line);
        if (!row.source_text.empty()) {
            entry += " " + row.source_text;
        }
        items.push_back(std::move(entry));
        display_to_row.push_back(static_cast<int>(index));

        if (!row.condition.empty()) {
            items.push_back("    when " + row.condition);
            display_to_row.push_back(static_cast<int>(index));
        }
    }

    display_to_row_ = std::move(display_to_row);
    if (list_->items() != items) {
        list_->assign_items(std::move(items));
    }
}

void BreakpointsPanel::set_on_activate(ActivateCallback callback) { on_activate_ = std::move(callback); }

void BreakpointsPanel::set_on_context(ContextCallback callback) { on_context_ = std::move(callback); }

void BreakpointsPanel::set_on_remove(RemoveCallback callback) { on_remove_ = std::move(callback); }

void BreakpointsPanel::set_on_add_condition(AddConditionCallback callback) {
    on_add_condition_ = std::move(callback);
}

void BreakpointsPanel::set_on_submit(SubmitCallback callback) {
    on_submit_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_submit([this](const std::string& value) {
            if (on_submit_ != nullptr) {
                on_submit_(value);
            }
        });
    }
}

void BreakpointsPanel::set_on_change(ChangeCallback callback) {
    on_change_ = std::move(callback);
    if (input_ != nullptr) {
        input_->set_on_change([this](const std::string& value) {
            if (on_change_ != nullptr) {
                on_change_(value);
            }
        });
    }
}

void BreakpointsPanel::set_input_value(std::string value) {
    if (input_ != nullptr) {
        input_->set_value(std::move(value));
    }
}

std::string BreakpointsPanel::input_value() const {
    return input_ != nullptr ? input_->value() : std::string{};
}

void BreakpointsPanel::focus_input() {
    if (input_ != nullptr) {
        input_->set_focused(true);
        if (list_ != nullptr) {
            list_->set_focused(false);
        }
    }
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

int BreakpointsPanel::selected_index() const {
    return list_ != nullptr ? list_->selected_index() : -1;
}

const BreakpointRow* BreakpointsPanel::selected_row() const {
    return breakpoint_at_display_index(selected_index());
}

tuinator::Widget* BreakpointsPanel::list_widget() const { return list_; }

tuinator::TextInput* BreakpointsPanel::input_widget() const { return input_; }

tuinator::ScrollView* BreakpointsPanel::scroll_view() const {
    return pane_ != nullptr ? pane_->scroll_view() : nullptr;
}

}  // namespace tui_debug_ui
