#pragma once

#include <tuinator/core/event.hpp>

#include <optional>
#include <string>
#include <unordered_map>

namespace tui_debug_ui {

struct KeyBindingSpec {
    char character = '\0';
    tuinator::Key key = tuinator::Key::Unknown;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
};

struct KeyBindings {
    KeyBindingSpec continue_key{.character = 'c'};
    KeyBindingSpec step_over{.character = 'n'};
    KeyBindingSpec step_into{.character = 'i'};
    KeyBindingSpec step_out{.character = 'u'};
    KeyBindingSpec breakpoint{.character = 'b'};
    KeyBindingSpec breakpoint_alt{.character = ' '};
    KeyBindingSpec focus_repl{.character = 'r'};
    KeyBindingSpec follow_execution{.character = 'f'};
    KeyBindingSpec file_picker{.character = 'p'};
    KeyBindingSpec panel_prev{.character = '['};
    KeyBindingSpec panel_next{.character = ']'};
    KeyBindingSpec panel_prev_alt{.character = '<'};
    KeyBindingSpec panel_next_alt{.character = '>'};
    KeyBindingSpec quit_key{.character = 'q'};
    KeyBindingSpec quit_escape{.key = tuinator::Key::Escape};

    KeyBindingSpec continue_fn{.key = tuinator::Key::F5};
    KeyBindingSpec step_over_fn{.key = tuinator::Key::F10};
    KeyBindingSpec step_into_fn{.key = tuinator::Key::F11};
    KeyBindingSpec step_out_fn{.key = tuinator::Key::F12};
    KeyBindingSpec step_out_shift_fn{.key = tuinator::Key::F11, .shift = true};
    KeyBindingSpec breakpoint_fn{.key = tuinator::Key::F9};

    [[nodiscard]] bool matches(const tuinator::KeyPress& press, const KeyBindingSpec& spec) const;
    [[nodiscard]] bool matches_quit(const tuinator::KeyPress& press) const;
};

[[nodiscard]] KeyBindings default_keybindings();
[[nodiscard]] KeyBindings parse_keybindings(const std::unordered_map<std::string, std::string>& values);
[[nodiscard]] std::optional<KeyBindingSpec> parse_key_binding(const std::string& text);

}  // namespace tui_debug_ui
