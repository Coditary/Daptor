#include "tui_debug_ui/file_picker.hpp"

#include "tui_debug_ui/divider_paint.hpp"
#include "tui_debug_ui/workspace_files.hpp"

#include <tuinator/render/canvas.hpp>
#include <tuinator/render/paint_context.hpp>
#include <tuinator/render/text.hpp>

#include <algorithm>
#include <string_view>
#include <utility>

namespace tui_debug_ui {

namespace {

constexpr const char* kTitle = " Open File ";
constexpr const char* kSearchLabel = "Search ";
constexpr const char* kSearchPlaceholder = "fuzzy find by path…";
constexpr int kMinWidth = 40;
constexpr int kMinHeight = 12;
constexpr int kSearchRow = 2;
constexpr int kSeparatorRow = 3;
constexpr int kListStartRow = 4;

} // namespace

FilePicker::FilePicker(tuinator::Style background, tuinator::Style border_style, tuinator::Style title_style,
                       tuinator::Style item_style, tuinator::Style selected_style, tuinator::Style search_label_style,
                       tuinator::Style search_field_style, tuinator::Style search_text_style,
                       tuinator::Style divider_style)
    : background_(std::move(background)),
      border_style_(std::move(border_style)),
      title_style_(std::move(title_style)),
      item_style_(std::move(item_style)),
      selected_style_(std::move(selected_style)),
      search_label_style_(std::move(search_label_style)),
      search_field_style_(std::move(search_field_style)),
      search_text_style_(std::move(search_text_style)),
      divider_style_(std::move(divider_style)) {}

void FilePicker::set_on_select(SelectCallback callback) { on_select_ = std::move(callback); }

void FilePicker::open(tuinator::Rect clip_bounds, std::vector<std::filesystem::path> files,
                      const std::filesystem::path& workspace_root) {
    files_ = std::move(files);
    workspace_root_ = workspace_root;
    filter_.clear();
    selected_ = 0;
    clip_bounds_ = clip_bounds;
    open_ = true;
    rebuild_filtered();
    mark_dirty();
}

void FilePicker::close() {
    if (!open_) {
        return;
    }
    open_ = false;
    files_.clear();
    filtered_.clear();
    filtered_labels_.clear();
    filter_.clear();
    selected_ = 0;
    mark_dirty();
}

void FilePicker::rebuild_filtered() {
    filtered_ = fuzzy_filter_files(files_, workspace_root_, filter_);
    filtered_labels_ = picker_display_labels(workspace_root_, filtered_);
    clamp_selection();
}

void FilePicker::clamp_selection() {
    if (filtered_.empty()) {
        selected_ = 0;
        return;
    }
    selected_ = std::clamp(selected_, 0, static_cast<int>(filtered_.size()) - 1);
}

int FilePicker::list_height() const {
    const tuinator::Rect panel = panel_bounds();
    return std::max(1, panel.height - kListStartRow - 1);
}

int FilePicker::list_scroll_offset() const {
    const int visible = list_height();
    if (selected_ < visible) {
        return 0;
    }
    return selected_ - visible + 1;
}

tuinator::Rect FilePicker::panel_bounds() const {
    if (!open_ || clip_bounds_.width <= 0 || clip_bounds_.height <= 0) {
        return {};
    }

    const int width = std::clamp(clip_bounds_.width - 4, kMinWidth, clip_bounds_.width);
    const int height = std::clamp(clip_bounds_.height - 2, kMinHeight, clip_bounds_.height);
    const int x = clip_bounds_.x + (clip_bounds_.width - width) / 2;
    const int y = clip_bounds_.y + (clip_bounds_.height - height) / 2;
    return {x, y, width, height};
}

int FilePicker::row_at_position(tuinator::Point position) const {
    const tuinator::Rect panel = panel_bounds();
    if (!panel.contains(position)) {
        return -1;
    }

    const int local_y = position.y - panel.y - kListStartRow;
    if (local_y < 0 || local_y >= list_height()) {
        return -1;
    }

    const int index = list_scroll_offset() + local_y;
    if (index < 0 || index >= static_cast<int>(filtered_.size())) {
        return -1;
    }
    return index;
}

void FilePicker::activate_selected() {
    if (selected_ < 0 || selected_ >= static_cast<int>(filtered_.size())) {
        return;
    }

    const std::filesystem::path selected = filtered_[static_cast<std::size_t>(selected_)];
    close();
    if (on_select_) {
        on_select_(selected);
    }
}

tuinator::Size FilePicker::preferred_size() const {
    const tuinator::Rect panel = panel_bounds();
    return {panel.width, panel.height};
}

void FilePicker::layout(tuinator::Rect bounds) {
    bounds_ = bounds;
    mark_dirty();
}

void FilePicker::paint(tuinator::PaintContext& ctx) const {
    if (!open_) {
        return;
    }

    const tuinator::Rect panel = panel_bounds();
    if (panel.width <= 0 || panel.height <= 0) {
        return;
    }

    tuinator::Canvas& canvas = ctx.canvas;
    const tuinator::Point origin{panel.x - bounds_.x, panel.y - bounds_.y};
    const tuinator::Size size{panel.width, panel.height};
    const int last_inner_row = origin.y + panel.height - 2;

    canvas.fill_rect({origin, size}, ' ', background_);
    canvas.draw_box({origin, size}, border_style_, ctx.glyphs());

    const int title_x = origin.x + std::max(1, (panel.width - static_cast<int>(std::string_view(kTitle).size())) / 2);
    canvas.draw_text({title_x, origin.y}, kTitle, title_style_);

    const int search_x = origin.x + 1;
    const int search_label_width = tuinator::text_display_width(kSearchLabel);
    const int input_x = search_x + search_label_width;
    const int input_width = std::max(1, panel.width - search_label_width - 2);
    canvas.draw_text({search_x, origin.y + kSearchRow}, kSearchLabel, search_label_style_);
    canvas.fill_rect({input_x, origin.y + kSearchRow, input_width, 1}, ' ', search_field_style_);

    if (filter_.empty()) {
        tuinator::Style placeholder_style = search_text_style_;
        placeholder_style.dim = true;
        const std::size_t placeholder_bytes =
            tuinator::text_byte_length_for_width(kSearchPlaceholder, std::max(0, input_width));
        canvas.draw_text({input_x, origin.y + kSearchRow}, std::string(kSearchPlaceholder).substr(0, placeholder_bytes),
                         placeholder_style);
    } else {
        const std::size_t text_bytes = tuinator::text_byte_length_for_width(filter_, std::max(0, input_width - 1));
        const std::string visible_text = filter_.substr(0, text_bytes);
        canvas.draw_text({input_x, origin.y + kSearchRow}, visible_text, search_text_style_);
        const int cursor_x = input_x + tuinator::text_display_width(visible_text);
        if (cursor_x < input_x + input_width) {
            tuinator::Style cursor_style = search_text_style_;
            cursor_style.reverse = true;
            canvas.draw_text({cursor_x, origin.y + kSearchRow}, " ", cursor_style);
        }
    }

    draw_thin_hline(canvas, origin.x + 1, origin.y + kSeparatorRow, std::max(0, panel.width - 2), divider_style_);

    const int visible = list_height();
    const int scroll = list_scroll_offset();
    for (int row = 0; row < visible; ++row) {
        const int index = scroll + row;
        if (index < 0 || index >= static_cast<int>(filtered_.size())) {
            break;
        }

        const int row_y = origin.y + kListStartRow + row;
        if (row_y > last_inner_row) {
            break;
        }

        const std::string& label = filtered_labels_[static_cast<std::size_t>(index)];
        const std::size_t separator = label.rfind('/');
        const std::string directory = separator == std::string::npos ? std::string{} : label.substr(0, separator + 1);
        const std::string filename = separator == std::string::npos ? label : label.substr(separator + 1);
        const bool selected = index == selected_;
        const tuinator::Style& row_style = selected ? selected_style_ : item_style_;
        int x = origin.x + 1;
        int remaining = std::max(0, panel.width - 2);

        if (!directory.empty() && remaining > 0) {
            tuinator::Style directory_style = row_style;
            if (!selected) {
                directory_style.dim = true;
            }
            const std::size_t directory_bytes = tuinator::text_byte_length_for_width(directory, remaining);
            const std::string visible_directory = directory.substr(0, directory_bytes);
            canvas.draw_text({x, row_y}, visible_directory, directory_style);
            const int directory_width = tuinator::text_display_width(visible_directory);
            x += directory_width;
            remaining = std::max(0, remaining - directory_width);
        }

        if (!filename.empty() && remaining > 0) {
            const std::size_t filename_bytes = tuinator::text_byte_length_for_width(filename, remaining);
            canvas.draw_text({x, row_y}, filename.substr(0, filename_bytes), row_style);
        }
    }

    if (filtered_.empty() && kListStartRow <= last_inner_row) {
        tuinator::Style empty_style = item_style_;
        empty_style.dim = true;
        canvas.draw_text({origin.x + 1, origin.y + kListStartRow}, "(no matching files)", empty_style);
    }
}

bool FilePicker::handle_event(const tuinator::Event& event) {
    if (!open_) {
        return false;
    }

    if (const auto* key = std::get_if<tuinator::KeyPress>(&event)) {
        if (key->key == tuinator::Key::Escape) {
            close();
            return true;
        }
        if (key->key == tuinator::Key::Up || key->character == 'k') {
            if (!filtered_.empty()) {
                selected_ = std::max(0, selected_ - 1);
                clamp_selection();
            }
            mark_dirty();
            return true;
        }
        if (key->key == tuinator::Key::Down || key->character == 'j') {
            if (!filtered_.empty()) {
                selected_ = std::min(static_cast<int>(filtered_.size()) - 1, selected_ + 1);
                clamp_selection();
            }
            mark_dirty();
            return true;
        }
        if (key->key == tuinator::Key::Enter) {
            activate_selected();
            return true;
        }
        if (key->key == tuinator::Key::Backspace) {
            if (!filter_.empty()) {
                filter_.pop_back();
                selected_ = 0;
                rebuild_filtered();
            }
            mark_dirty();
            return true;
        }
        if (!key->ctrl && !key->alt && key->character >= 32 && key->character != 127) {
            filter_.push_back(static_cast<char>(key->character));
            selected_ = 0;
            rebuild_filtered();
            mark_dirty();
            return true;
        }
        return true;
    }

    if (const auto* mouse = std::get_if<tuinator::MouseEvent>(&event)) {
        const bool left = mouse->button == tuinator::MouseButton::Left;
        const bool pointer_pick = mouse->action == tuinator::MouseAction::Click ||
                                  mouse->action == tuinator::MouseAction::Release;
        if (pointer_pick && left) {
            const int row = row_at_position(mouse->position);
            if (row >= 0) {
                selected_ = row;
                activate_selected();
            } else {
                close();
            }
            return true;
        }
        if (mouse->action == tuinator::MouseAction::Press && left) {
            const int row = row_at_position(mouse->position);
            if (row >= 0) {
                selected_ = row;
                mark_dirty();
            }
            return true;
        }
        return true;
    }

    return open_;
}

} // namespace tui_debug_ui
