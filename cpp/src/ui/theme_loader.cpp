#include "tui_debug_ui/theme_loader.hpp"

#include "tui_debug_ui/xdg_paths.hpp"

#include <cstdlib>
#include <fstream>
#include <optional>
#include <string>
#include <unordered_map>

#if __has_include(<nlohmann/json.hpp>)
#include <nlohmann/json.hpp>
#define TUI_DEBUG_UI_HAS_NLOHMANN_JSON 1
#endif

namespace tui_debug_ui {
namespace {

#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
using Json = nlohmann::json;

std::optional<tuinator::Rgb> parse_rgb_array(const Json& value) {
    if (!value.is_array() || value.size() != 3) {
        return std::nullopt;
    }
    return tuinator::Rgb{
        static_cast<std::uint8_t>(value[0].get<int>()),
        static_cast<std::uint8_t>(value[1].get<int>()),
        static_cast<std::uint8_t>(value[2].get<int>()),
    };
}

struct ParsedStyle {
    std::optional<tuinator::Rgb> fg;
    std::optional<tuinator::Rgb> bg;
    std::optional<bool> bold;
    std::optional<bool> dim;
};

std::optional<ParsedStyle> parse_style_value(const Json& value) {
    if (value.is_array()) {
        ParsedStyle parsed;
        parsed.fg = parse_rgb_array(value);
        return parsed;
    }
    if (!value.is_object()) {
        return std::nullopt;
    }

    ParsedStyle parsed;
    if (value.contains("fg")) {
        parsed.fg = parse_rgb_array(value.at("fg"));
    }
    if (value.contains("bg")) {
        parsed.bg = parse_rgb_array(value.at("bg"));
    }
    if (value.contains("bold")) {
        parsed.bold = value.at("bold").get<bool>();
    }
    if (value.contains("dim")) {
        parsed.dim = value.at("dim").get<bool>();
    }
    return parsed;
}

tuinator::Style apply_parsed_style(const tuinator::Style& base, const ParsedStyle& parsed,
                                     const tuinator::Rgb& default_bg) {
    tuinator::Style style = base;
    const tuinator::Rgb fg = parsed.fg.value_or(default_bg);
    const tuinator::Rgb bg = parsed.bg.value_or(default_bg);
    if (parsed.fg.has_value() || parsed.bg.has_value()) {
        style = tuinator::style_fg_bg(fg, bg);
    }
    if (parsed.bold.has_value()) {
        style.bold = *parsed.bold;
    }
    if (parsed.dim.has_value()) {
        style.dim = *parsed.dim;
    }
    return style;
}

void apply_color_map(DapUiTheme& theme, const Json& colors) {
    using Setter = void (*)(DapUiTheme&, tuinator::Style);
    static const std::unordered_map<std::string, Setter> kSetters = {
        {"border_normal", [](DapUiTheme& t, tuinator::Style s) { t.border_normal = s; }},
        {"border_focused", [](DapUiTheme& t, tuinator::Style s) { t.border_focused = s; }},
        {"title_normal", [](DapUiTheme& t, tuinator::Style s) { t.title_normal = s; }},
        {"title_focused", [](DapUiTheme& t, tuinator::Style s) { t.title_focused = s; }},
        {"panel_background", [](DapUiTheme& t, tuinator::Style s) { t.panel_background = s; }},
        {"root_background", [](DapUiTheme& t, tuinator::Style s) { t.root_background = s; }},
        {"control_bar_background", [](DapUiTheme& t, tuinator::Style s) { t.control_bar_background = s; }},
        {"label", [](DapUiTheme& t, tuinator::Style s) { t.label = s; }},
        {"selection", [](DapUiTheme& t, tuinator::Style s) { t.selection = s; }},
        {"scope_header", [](DapUiTheme& t, tuinator::Style s) { t.scope_header = s; }},
        {"scope_locals_header", [](DapUiTheme& t, tuinator::Style s) { t.scope_locals_header = s; }},
        {"scope_globals_header", [](DapUiTheme& t, tuinator::Style s) { t.scope_globals_header = s; }},
        {"variable_name", [](DapUiTheme& t, tuinator::Style s) { t.variable_name = s; }},
        {"variable_value", [](DapUiTheme& t, tuinator::Style s) { t.variable_value = s; }},
        {"breakpoint_marker", [](DapUiTheme& t, tuinator::Style s) { t.breakpoint_marker = s; }},
        {"breakpoint_file", [](DapUiTheme& t, tuinator::Style s) { t.breakpoint_file = s; }},
        {"breakpoint_line_number", [](DapUiTheme& t, tuinator::Style s) { t.breakpoint_line_number = s; }},
        {"breakpoint_condition", [](DapUiTheme& t, tuinator::Style s) { t.breakpoint_condition = s; }},
        {"breakpoint_hit_condition", [](DapUiTheme& t, tuinator::Style s) { t.breakpoint_hit_condition = s; }},
        {"breakpoint_hit_count", [](DapUiTheme& t, tuinator::Style s) { t.breakpoint_hit_count = s; }},
        {"console_stderr", [](DapUiTheme& t, tuinator::Style s) { t.console_stderr = s; }},
        {"console_stdout", [](DapUiTheme& t, tuinator::Style s) { t.console_stdout = s; }},
        {"console_event", [](DapUiTheme& t, tuinator::Style s) { t.console_event = s; }},
        {"title_scopes", [](DapUiTheme& t, tuinator::Style s) { t.title_scopes = s; }},
        {"title_stacks", [](DapUiTheme& t, tuinator::Style s) { t.title_stacks = s; }},
        {"title_breakpoints", [](DapUiTheme& t, tuinator::Style s) { t.title_breakpoints = s; }},
        {"title_source", [](DapUiTheme& t, tuinator::Style s) { t.title_source = s; }},
        {"title_console", [](DapUiTheme& t, tuinator::Style s) { t.title_console = s; }},
        {"title_repl", [](DapUiTheme& t, tuinator::Style s) { t.title_repl = s; }},
        {"repl_ghost", [](DapUiTheme& t, tuinator::Style s) { t.repl_ghost = s; }},
        {"title_watches", [](DapUiTheme& t, tuinator::Style s) { t.title_watches = s; }},
        {"thread_header", [](DapUiTheme& t, tuinator::Style s) { t.thread_header = s; }},
        {"thread_stopped", [](DapUiTheme& t, tuinator::Style s) { t.thread_stopped = s; }},
        {"frame_current", [](DapUiTheme& t, tuinator::Style s) { t.frame_current = s; }},
        {"frame_normal", [](DapUiTheme& t, tuinator::Style s) { t.frame_normal = s; }},
        {"frame_location", [](DapUiTheme& t, tuinator::Style s) { t.frame_location = s; }},
        {"status_bar", [](DapUiTheme& t, tuinator::Style s) { t.status_bar = s; }},
        {"divider", [](DapUiTheme& t, tuinator::Style s) { t.divider = s; }},
        {"control_play", [](DapUiTheme& t, tuinator::Style s) { t.control_play = s; }},
        {"control_step", [](DapUiTheme& t, tuinator::Style s) { t.control_step = s; }},
        {"control_stop", [](DapUiTheme& t, tuinator::Style s) { t.control_stop = s; }},
        {"control_restart", [](DapUiTheme& t, tuinator::Style s) { t.control_restart = s; }},
        {"control_disabled", [](DapUiTheme& t, tuinator::Style s) { t.control_disabled = s; }},
        {"row_action_muted", [](DapUiTheme& t, tuinator::Style s) { t.row_action_muted = s; }},
        {"layout_drop_swap", [](DapUiTheme& t, tuinator::Style s) { t.layout_drop_swap = s; }},
        {"layout_drop_insert", [](DapUiTheme& t, tuinator::Style s) { t.layout_drop_insert = s; }},
        {"layout_drop_merge", [](DapUiTheme& t, tuinator::Style s) { t.layout_drop_merge = s; }},
        {"layout_drop_span", [](DapUiTheme& t, tuinator::Style s) { t.layout_drop_span = s; }},
        {"layout_menu_pane", [](DapUiTheme& t, tuinator::Style s) { t.layout_menu_pane = s; }},
        {"file_tree_folder", [](DapUiTheme& t, tuinator::Style s) { t.file_tree_folder = s; }},
        {"file_tree_file", [](DapUiTheme& t, tuinator::Style s) { t.file_tree_file = s; }},
        {"file_tree_row_selected", [](DapUiTheme& t, tuinator::Style s) { t.file_tree_row_selected = s; }},
        {"scrollbar_thumb", [](DapUiTheme& t, tuinator::Style s) { t.scrollbar_thumb = s; }},
        {"scrollbar_track", [](DapUiTheme& t, tuinator::Style s) { t.scrollbar_track = s; }},
    };

    for (const auto& [key, value] : colors.items()) {
        const auto setter = kSetters.find(key);
        if (setter == kSetters.end()) {
            continue;
        }
        const std::optional<ParsedStyle> parsed = parse_style_value(value);
        if (!parsed.has_value()) {
            continue;
        }
        tuinator::Style base;
        setter->second(theme, apply_parsed_style(base, *parsed, theme.background));
    }
}

void apply_syntax_color_map(SyntaxTheme& theme, const Json& colors) {
    using Setter = void (*)(SyntaxTheme&, tuinator::Style);
    static const std::unordered_map<std::string, Setter> kSetters = {
        {"keyword", [](SyntaxTheme& t, tuinator::Style s) { t.keyword = s; }},
        {"string", [](SyntaxTheme& t, tuinator::Style s) { t.string = s; }},
        {"comment", [](SyntaxTheme& t, tuinator::Style s) { t.comment = s; }},
        {"function", [](SyntaxTheme& t, tuinator::Style s) { t.function = s; }},
        {"type", [](SyntaxTheme& t, tuinator::Style s) { t.type = s; }},
        {"number", [](SyntaxTheme& t, tuinator::Style s) { t.number = s; }},
        {"operator", [](SyntaxTheme& t, tuinator::Style s) { t.operator_ = s; }},
        {"variable", [](SyntaxTheme& t, tuinator::Style s) { t.variable = s; }},
        {"default_text", [](SyntaxTheme& t, tuinator::Style s) { t.default_text = s; }},
        {"line_number", [](SyntaxTheme& t, tuinator::Style s) { t.line_number = s; }},
        {"breakpoint_marker", [](SyntaxTheme& t, tuinator::Style s) { t.breakpoint_marker = s; }},
        {"breakpoint_conditional_marker",
         [](SyntaxTheme& t, tuinator::Style s) { t.breakpoint_conditional_marker = s; }},
        {"execution_row", [](SyntaxTheme& t, tuinator::Style s) { t.execution_row = s; }},
        {"execution_marker", [](SyntaxTheme& t, tuinator::Style s) { t.execution_marker = s; }},
        {"cursor_row", [](SyntaxTheme& t, tuinator::Style s) { t.cursor_row = s; }},
        {"step_in_candidate", [](SyntaxTheme& t, tuinator::Style s) { t.step_in_candidate = s; }},
        {"step_in_active", [](SyntaxTheme& t, tuinator::Style s) { t.step_in_active = s; }},
        {"panel_background", [](SyntaxTheme& t, tuinator::Style s) { t.panel_background = s; }},
    };

    for (const auto& [key, value] : colors.items()) {
        const auto setter = kSetters.find(key);
        if (setter == kSetters.end()) {
            continue;
        }
        const std::optional<ParsedStyle> parsed = parse_style_value(value);
        if (!parsed.has_value()) {
            continue;
        }
        tuinator::Style base;
        setter->second(theme, apply_parsed_style(base, *parsed, theme.background));
    }
}

void recolor_ui_background(DapUiTheme& theme, const tuinator::Rgb& old_bg, const tuinator::Rgb& new_bg) {
    const auto recolor = [&](tuinator::Style& style) {
        if (style.background_rgb == old_bg) {
            style.background_rgb = new_bg;
        }
    };

    recolor(theme.border_normal);
    recolor(theme.border_focused);
    recolor(theme.title_normal);
    recolor(theme.title_focused);
    recolor(theme.label);
    recolor(theme.scope_header);
    recolor(theme.scope_locals_header);
    recolor(theme.scope_globals_header);
    recolor(theme.variable_name);
    recolor(theme.variable_value);
    recolor(theme.breakpoint_marker);
    recolor(theme.breakpoint_file);
    recolor(theme.breakpoint_line_number);
    recolor(theme.breakpoint_condition);
    recolor(theme.breakpoint_hit_condition);
    recolor(theme.breakpoint_hit_count);
    recolor(theme.console_stderr);
    recolor(theme.console_stdout);
    recolor(theme.console_event);
    recolor(theme.title_scopes);
    recolor(theme.title_stacks);
    recolor(theme.title_breakpoints);
    recolor(theme.title_source);
    recolor(theme.title_console);
    recolor(theme.title_repl);
    recolor(theme.repl_ghost);
    recolor(theme.title_watches);
    recolor(theme.thread_header);
    recolor(theme.thread_stopped);
    recolor(theme.frame_current);
    recolor(theme.frame_normal);
    recolor(theme.frame_location);
    recolor(theme.divider);
    recolor(theme.control_play);
    recolor(theme.control_step);
    recolor(theme.control_stop);
    recolor(theme.control_restart);
    recolor(theme.control_disabled);
    recolor(theme.row_action_muted);
    recolor(theme.file_tree_folder);
    recolor(theme.file_tree_file);
    recolor(theme.scrollbar_thumb);

    theme.background = new_bg;
    theme.panel_background = tuinator::style_bg(new_bg);
    theme.root_background = tuinator::style_bg(new_bg);
    theme.control_bar_background = tuinator::style_bg(new_bg);
}

void recolor_syntax_background(SyntaxTheme& theme, const tuinator::Rgb& old_bg, const tuinator::Rgb& new_bg) {
    const auto recolor = [&](tuinator::Style& style) {
        if (style.background_rgb == old_bg) {
            style.background_rgb = new_bg;
        }
    };

    recolor(theme.keyword);
    recolor(theme.string);
    recolor(theme.comment);
    recolor(theme.function);
    recolor(theme.type);
    recolor(theme.number);
    recolor(theme.operator_);
    recolor(theme.variable);
    recolor(theme.default_text);
    recolor(theme.line_number);
    recolor(theme.breakpoint_marker);
    recolor(theme.breakpoint_conditional_marker);
    recolor(theme.execution_marker);

    theme.background = new_bg;
    theme.panel_background = tuinator::style_bg(new_bg);
}

void apply_ui_shortcuts(DapUiTheme& theme, const Json& ui) {
    const auto set_fg_on_bg = [&](tuinator::Style& style, const tuinator::Rgb& fg) {
        style = tuinator::style_fg_bg(fg, theme.background);
    };

    if (ui.contains("text")) {
        if (const std::optional<tuinator::Rgb> fg = parse_rgb_array(ui.at("text")); fg.has_value()) {
            set_fg_on_bg(theme.label, *fg);
            set_fg_on_bg(theme.variable_value, *fg);
        }
    }
    if (ui.contains("muted")) {
        if (const std::optional<tuinator::Rgb> fg = parse_rgb_array(ui.at("muted")); fg.has_value()) {
            set_fg_on_bg(theme.title_normal, *fg);
            set_fg_on_bg(theme.breakpoint_hit_count, *fg);
            set_fg_on_bg(theme.repl_ghost, *fg);
            set_fg_on_bg(theme.row_action_muted, *fg);
        }
    }
    if (ui.contains("accent")) {
        if (const std::optional<tuinator::Rgb> fg = parse_rgb_array(ui.at("accent")); fg.has_value()) {
            set_fg_on_bg(theme.border_focused, *fg);
            set_fg_on_bg(theme.title_focused, *fg);
            set_fg_on_bg(theme.scope_header, *fg);
            set_fg_on_bg(theme.thread_header, *fg);
            set_fg_on_bg(theme.frame_location, *fg);
        }
    }
    if (ui.contains("divider")) {
        if (const std::optional<tuinator::Rgb> fg = parse_rgb_array(ui.at("divider")); fg.has_value()) {
            set_fg_on_bg(theme.divider, *fg);
        }
    }
    if (ui.contains("selected")) {
        if (const std::optional<ParsedStyle> parsed = parse_style_value(ui.at("selected")); parsed.has_value()) {
            theme.selection = apply_parsed_style(theme.selection, *parsed, theme.background);
        }
    }
}

bool apply_theme_json(LoadedTheme& result, const Json& root) {
    if (root.contains("ui") && root.at("ui").is_object()) {
        const Json& ui = root.at("ui");
        const tuinator::Rgb old_bg = result.ui.background;
        if (ui.contains("background")) {
            if (const std::optional<tuinator::Rgb> bg = parse_rgb_array(ui.at("background")); bg.has_value()) {
                recolor_ui_background(result.ui, old_bg, *bg);
            }
        }
        apply_ui_shortcuts(result.ui, ui);
        if (ui.contains("colors") && ui.at("colors").is_object()) {
            apply_color_map(result.ui, ui.at("colors"));
        }
    }

    if (root.contains("syntax") && root.at("syntax").is_object()) {
        const Json& syntax = root.at("syntax");
        const tuinator::Rgb old_bg = result.syntax.background;
        if (syntax.contains("background")) {
            if (const std::optional<tuinator::Rgb> bg = parse_rgb_array(syntax.at("background")); bg.has_value()) {
                recolor_syntax_background(result.syntax, old_bg, *bg);
            }
        }
        if (syntax.contains("colors") && syntax.at("colors").is_object()) {
            apply_syntax_color_map(result.syntax, syntax.at("colors"));
        }
    }

    return true;
}
#endif

std::filesystem::path theme_path_from_env() {
    const char* env = std::getenv("TUI_DEBUG_THEME");
    if (env == nullptr || env[0] == '\0') {
        return {};
    }
    return expand_user_path(std::filesystem::path(env));
}

}  // namespace

LoadedTheme load_theme_file(const std::filesystem::path& path) {
    LoadedTheme result{
        .ui = DapUiTheme{},
        .syntax = SyntaxTheme{},
        .theme_path = path,
        .loaded_from_file = false,
    };

#ifdef TUI_DEBUG_UI_HAS_NLOHMANN_JSON
    if (path.empty()) {
        return result;
    }

    std::ifstream input(path);
    if (!input.is_open()) {
        return result;
    }

    try {
        const Json root = Json::parse(input);
        if (!root.is_object()) {
            return result;
        }
        apply_theme_json(result, root);
        result.loaded_from_file = true;
        result.ui.finalize_styles();
        result.syntax.finalize_styles();
        return result;
    } catch (const Json::exception&) {
        return result;
    }
#else
    (void)path;
    return result;
#endif
}

LoadedTheme load_application_theme(const AppConfig& config) {
    if (const std::filesystem::path env_path = theme_path_from_env(); !env_path.empty()) {
        return load_theme_file(env_path);
    }

    if (config.theme_file.has_value()) {
        LoadedTheme loaded = load_theme_file(*config.theme_file);
        if (loaded.loaded_from_file) {
            return loaded;
        }
    }

    return LoadedTheme{
        .ui = DapUiTheme{},
        .syntax = SyntaxTheme{},
        .theme_path = config.theme_file.value_or(config.config_directory / "theme.json"),
        .loaded_from_file = false,
    };
}

}  // namespace tui_debug_ui
