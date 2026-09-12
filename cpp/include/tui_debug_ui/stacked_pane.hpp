#pragma once

#include <tuinator/core/event.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/widget.hpp>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tuinator {
class TextInput;
}

namespace tui_debug_ui {

/// One visible panel at a time; chrome shows as many tab labels as fit.
class StackedPane : public tuinator::Widget {
  public:
    struct Entry {
        std::string label;
        std::unique_ptr<tuinator::Widget> widget;
    };

    using ActiveChangedCallback = std::function<void(int index)>;

    StackedPane(std::vector<Entry> entries, tuinator::Style background, tuinator::Style chrome_label,
                tuinator::Style chrome_hint, tuinator::Style chrome_divider, tuinator::Style chrome_add,
                tuinator::Style chrome_edit);

    [[nodiscard]] int active_index() const { return active_index_; }
    [[nodiscard]] int count() const { return static_cast<int>(entries_.size()); }
    [[nodiscard]] const std::string& active_label() const;
    void set_active_index(int index);
    void cycle(int delta);

    void set_on_active_changed(ActiveChangedCallback callback);
    void set_add_action(std::function<void()> callback);
    void set_rename_action(std::function<void(int index, const std::string& label)> callback);
    void set_pane_menu_action(std::function<void(tuinator::Point anchor)> callback);
    void begin_rename_active_tab();
    void append_entry(std::string label, std::unique_ptr<tuinator::Widget> widget);
    void remove_entry(int index);
    void set_entry_label(int index, std::string label);
    void propagate_on_dirty(std::function<void(tuinator::Rect)> callback);
    [[nodiscard]] bool is_renaming() const { return rename_index_ >= 0; }
    void finish_rename_on_click_outside(tuinator::Point global_point);

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    tuinator::Widget* hit_test(tuinator::Point point) override;
    bool has_focused_descendant() const override;
    void collect_focusable(std::vector<tuinator::Widget*>& out) override;
    void for_each_child(const std::function<void(tuinator::Widget*)>& visitor) override;

  private:
    static constexpr int kChromeLabelRows = 1;
    static constexpr int kChromeDividerRows = 1;
    static constexpr int kChromeHeight = kChromeLabelRows + kChromeDividerRows;

    enum class ChromeMode {
        AllTabs,
        ScrollTabs,
        Compact,
    };

    struct TabSegment {
        int index = -1;
        int x = 0;
        int width = 0;
    };

    struct ArrowSegment {
        int x = 0;
        int width = 0;
        bool visible = false;
    };

    struct ChromeLayout {
        ChromeMode mode = ChromeMode::Compact;
        int first_visible_index = 0;
        std::vector<TabSegment> tabs;
        ArrowSegment prev_arrow;
        ArrowSegment next_arrow;
        int counter_x = -1;
        int edit_x = -1;
        int add_x = -1;
        std::string counter;
    };

    tuinator::Rect chrome_bounds() const;
    tuinator::Rect content_bounds() const;
    void notify_active_changed();
    void ensure_active_tab_visible();
    bool handle_chrome_click(tuinator::Point position);

    [[nodiscard]] int label_width(int index) const;
    [[nodiscard]] int width_for_tabs(int start, int num) const;
    [[nodiscard]] int all_tabs_width() const;
    [[nodiscard]] int scroll_left_overhead() const;
    [[nodiscard]] int scroll_chrome_overhead() const;
    [[nodiscard]] int tabs_budget() const;
    [[nodiscard]] int max_tabs_for_budget(int budget) const;
    [[nodiscard]] int max_scroll_visible_tabs(int start_index) const;
    [[nodiscard]] std::string counter_text() const;
    [[nodiscard]] int counter_width() const;
    [[nodiscard]] int add_glyph_width() const;
    [[nodiscard]] int edit_glyph_width() const;
    [[nodiscard]] int add_action_width() const;
    [[nodiscard]] int rename_action_width() const;
    [[nodiscard]] int far_right_add_x() const;
    [[nodiscard]] int far_right_edit_x() const;
    [[nodiscard]] int far_right_counter_x(bool show_counter) const;
    void apply_right_cluster(ChromeLayout& layout, bool show_counter) const;
    void ensure_rename_input();
    [[nodiscard]] bool rename_field_contains(tuinator::Point global_point) const;
    void begin_rename(int index);
    void cancel_rename();
    void commit_rename(const std::string& value);
    [[nodiscard]] bool scroll_needs_counter(int first_visible_index, int visible_count) const;
    [[nodiscard]] int scroll_trailing_chain_width() const;
    [[nodiscard]] int right_cluster_width(bool include_counter) const;
    [[nodiscard]] ChromeLayout chrome_layout() const;
    [[nodiscard]] tuinator::Style chrome_button_style() const;
    [[nodiscard]] tuinator::Style tab_style(int index) const;
    void paint_chrome(tuinator::Canvas& canvas) const;

    struct EntryData {
        std::string label;
        std::unique_ptr<tuinator::Widget> widget;
    };

    std::vector<EntryData> entries_;
    tuinator::Style background_;
    tuinator::Style chrome_label_;
    tuinator::Style chrome_hint_;
    tuinator::Style chrome_divider_;
    tuinator::Style chrome_add_;
    tuinator::Style chrome_edit_;
    int active_index_ = 0;
    int tab_scroll_offset_ = 0;
    int rename_index_ = -1;
    std::unique_ptr<tuinator::TextInput> rename_input_;
    ActiveChangedCallback on_active_changed_;
    std::function<void()> add_action_;
    std::function<void(int index, const std::string& label)> rename_action_;
    std::function<void(tuinator::Point)> pane_menu_action_;
};

}  // namespace tui_debug_ui
