#pragma once

#include "tui_debug_ui/network_mock_data.hpp"

#include <filesystem>
#include <tuinator/core/geometry.hpp>
#include <tuinator/render/scrollbar.hpp>
#include <tuinator/render/style.hpp>
#include <tuinator/widgets/containers/scroll_view.hpp>
#include <tuinator/widgets/widget.hpp>

#include <optional>

#include <functional>
#include <string>
#include <vector>

namespace tui_debug_ui {

struct DapUiTheme;

enum class NetworkPanelView {
    Traffic,
    Compose,
};

enum class NetworkFocusPane {
    List,
    Detail,
};

enum class ComposeEditField {
    Name,
    Method,
    Url,
    Headers,
    Body,
    Timeout,
};

/// MITM proxy traffic panel (mock data until live capture is wired).
class NetworkPanel : public tuinator::Widget {
  public:
    NetworkPanel(const DapUiTheme& theme, tuinator::ScrollViewOptions scroll_options);

    void set_session(NetworkMockSession session);
    void set_on_action(std::function<void(const std::string& action, const NetworkExchange& exchange)> callback);
    void set_on_compose_send(std::function<bool(const NetworkComposeTemplate& template_item)> callback);
    void set_on_message(std::function<void(const std::string& message)> callback);
    void set_workspace_root(const std::filesystem::path& workspace_root);
    void set_compose_pending(bool pending);
    void set_compose_result(const std::optional<NetworkExchange>& result);
    void select_last_exchange();
    void refresh_scroll_content();

    [[nodiscard]] NetworkPanelView active_view() const { return active_view_; }
    void set_view(NetworkPanelView view);
    void cycle_view();

    [[nodiscard]] tuinator::Widget* list_widget() const;
    [[nodiscard]] tuinator::Widget* focus_widget() const;
    [[nodiscard]] bool is_compose_input_focused() const;
    [[nodiscard]] const NetworkExchange* selected_exchange() const;
    [[nodiscard]] bool perform_action(char action_key);

    tuinator::Size preferred_size() const override;
    void layout(tuinator::Rect bounds) override;
    void paint(tuinator::PaintContext& ctx) const override;
    bool handle_event(const tuinator::Event& event) override;
    tuinator::Widget* hit_test(tuinator::Point point) override;
    tuinator::Widget* hit_test_focusable(tuinator::Point point) override;
    bool has_focused_descendant() const override;
    bool is_focusable() const override { return true; }
    void collect_focusable(std::vector<tuinator::Widget*>& out) override;

  private:
    struct TextCell {
        int line = 0;
        int column = 0;
    };

    struct TextSelection {
        NetworkFocusPane pane = NetworkFocusPane::Detail;
        TextCell anchor;
        TextCell cursor;
        bool dragging = false;
    };

    struct PaneContentRect {
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
    };

    void sync_detail_scroll();
    void apply_compose_template(int index);
    void load_compose_templates_from_disk();
    bool begin_compose_edit();
    bool commit_compose_edit();
    void cancel_compose_edit();
    bool handle_compose_edit_key(const tuinator::KeyPress& key);
    bool add_compose_template();
    bool delete_compose_template();
    bool reload_compose_templates();
    [[nodiscard]] std::string& compose_edit_field_value();
    void cycle_compose_edit_field(int delta);
    void flush_compose_edit_field();
    void prepare_compose_edit_field();
    [[nodiscard]] const std::string* compose_timeout_edit_for_paint() const;
    bool send_compose();
    bool copy_selection();
    void clear_selection();
    [[nodiscard]] std::string format_list_row(const NetworkExchange& exchange, int max_width) const;
    [[nodiscard]] std::string path_from_url(const std::string& url) const;
    [[nodiscard]] int list_area_top() const;
    [[nodiscard]] int list_area_height() const;
    [[nodiscard]] int split_column() const;
    [[nodiscard]] int row_at_list_y(int local_y) const;
    [[nodiscard]] bool point_in_list_pane(tuinator::Point local) const;
    [[nodiscard]] bool point_in_detail_pane(tuinator::Point local) const;
    [[nodiscard]] PaneContentRect list_content_rect() const;
    [[nodiscard]] PaneContentRect detail_content_rect() const;
    [[nodiscard]] int detail_pane_width() const;
    [[nodiscard]] int detail_pane_height() const;
    [[nodiscard]] int traffic_detail_line_count(int width) const;
    [[nodiscard]] int compose_detail_line_count(int width) const;
    [[nodiscard]] std::vector<std::string> list_display_lines() const;
    [[nodiscard]] std::vector<std::string> detail_display_lines() const;
    [[nodiscard]] std::optional<TextCell> text_cell_at(NetworkFocusPane pane, tuinator::Point local) const;
    [[nodiscard]] std::string selected_text() const;
    void reset_detail_scroll();
    void scroll_list(int delta);
    void scroll_detail(int delta);
    void paint_scrollbar(tuinator::Canvas& canvas, const PaneContentRect& content, int line_count, int scroll,
                         const tuinator::ScrollbarInteractionState& state) const;
    bool handle_scrollbar_mouse(const tuinator::MouseEvent& mouse, const PaneContentRect& content, int line_count,
                                int& scroll, tuinator::ScrollbarInteractionState& state);
    void paint_line_with_selection(tuinator::Canvas& canvas, int x, int y, const std::string& line,
                                   tuinator::Style style, NetworkFocusPane pane, int line_index, int max_width) const;
    void begin_selection(NetworkFocusPane pane, tuinator::Point local);
    void update_selection(tuinator::Point local);
    void end_selection();
    void paint_status_row(tuinator::Canvas& canvas) const;
    void paint_tab_row(tuinator::Canvas& canvas) const;
    void paint_traffic_view(tuinator::Canvas& canvas, int top, int height) const;
    void paint_compose_view(tuinator::Canvas& canvas, int top, int height) const;
    void paint_exchange_detail(tuinator::Canvas& canvas, int x, int y, int width, int height,
                               const NetworkExchange& exchange) const;
    bool handle_traffic_key(const tuinator::KeyPress& key);
    bool handle_compose_key(const tuinator::KeyPress& key);
    bool handle_mouse(const tuinator::MouseEvent& mouse);

    const DapUiTheme& theme_;
    NetworkMockSession session_;
    std::vector<NetworkComposeTemplate> compose_templates_;
    std::filesystem::path workspace_root_;
    std::filesystem::path compose_templates_path_;
    bool compose_editing_ = false;
    bool compose_pending_ = false;
    ComposeEditField compose_edit_field_ = ComposeEditField::Url;
    std::optional<NetworkComposeTemplate> compose_edit_backup_;
    std::string compose_timeout_edit_;
    std::optional<NetworkExchange> last_compose_result_;
    NetworkPanelView active_view_ = NetworkPanelView::Traffic;
    NetworkFocusPane focus_pane_ = NetworkFocusPane::List;
    int selected_index_ = 0;
    int selected_template_index_ = 0;
    int list_scroll_ = 0;
    int detail_scroll_ = 0;
    tuinator::ScrollbarOptions scrollbar_options_;
    tuinator::ScrollbarInteractionState list_scrollbar_state_;
    tuinator::ScrollbarInteractionState detail_scrollbar_state_;
    std::optional<TextSelection> text_selection_;
    std::function<void(const std::string&, const NetworkExchange&)> on_action_;
    std::function<bool(const NetworkComposeTemplate&)> on_compose_send_;
    std::function<void(const std::string&)> on_message_;
};

}  // namespace tui_debug_ui
