#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/core/geometry.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/widget.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace tui_debug_ui {

/// Modal fuzzy file browser (press `p` in the source panel).
class FilePicker : public tuinator::Widget {
  public:
    using SelectCallback = std::function<void(const std::filesystem::path& path)>;

    FilePicker(tuinator::Style background, tuinator::Style border_style, tuinator::Style title_style,
               tuinator::Style item_style, tuinator::Style selected_style, tuinator::Style search_label_style,
               tuinator::Style search_field_style, tuinator::Style search_text_style, tuinator::Style divider_style);

    [[nodiscard]] bool is_open() const { return open_; }

    void open(tuinator::Rect clip_bounds, std::vector<std::filesystem::path> files,
              const std::filesystem::path& workspace_root);
    void close();

    void set_on_select(SelectCallback callback);

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    bool is_focusable() const override { return open_; }

  private:
    void rebuild_filtered();
    void clamp_selection();
    [[nodiscard]] tuinator::Rect panel_bounds() const;
    [[nodiscard]] int list_height() const;
    [[nodiscard]] int list_scroll_offset() const;
    [[nodiscard]] int row_at_position(tuinator::Point position) const;
    void activate_selected();

    tuinator::Style background_;
    tuinator::Style border_style_;
    tuinator::Style title_style_;
    tuinator::Style item_style_;
    tuinator::Style selected_style_;
    tuinator::Style search_label_style_;
    tuinator::Style search_field_style_;
    tuinator::Style search_text_style_;
    tuinator::Style divider_style_;
    tuinator::Rect clip_bounds_{};
    std::vector<std::filesystem::path> files_;
    std::vector<std::filesystem::path> filtered_;
    std::vector<std::string> filtered_labels_;
    std::filesystem::path workspace_root_;
    std::string filter_;
    SelectCallback on_select_;
    int selected_ = 0;
    bool open_ = false;
};

} // namespace tui_debug_ui
