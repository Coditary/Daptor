#include "tui_debug_ui/keybindings.hpp"

#include <algorithm>
#include <cctype>

namespace tui_debug_ui {
namespace {

std::string lowercase_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

tuinator::Key function_key_from_name(const std::string& name) {
    if (name == "f1") {
        return tuinator::Key::F1;
    }
    if (name == "f2") {
        return tuinator::Key::F2;
    }
    if (name == "f3") {
        return tuinator::Key::F3;
    }
    if (name == "f4") {
        return tuinator::Key::F4;
    }
    if (name == "f5") {
        return tuinator::Key::F5;
    }
    if (name == "f6") {
        return tuinator::Key::F6;
    }
    if (name == "f7") {
        return tuinator::Key::F7;
    }
    if (name == "f8") {
        return tuinator::Key::F8;
    }
    if (name == "f9") {
        return tuinator::Key::F9;
    }
    if (name == "f10") {
        return tuinator::Key::F10;
    }
    if (name == "f11") {
        return tuinator::Key::F11;
    }
    if (name == "f12") {
        return tuinator::Key::F12;
    }
    return tuinator::Key::Unknown;
}

}  // namespace

bool KeyBindings::matches(const tuinator::KeyPress& press, const KeyBindingSpec& spec) const {
    if (press.ctrl != spec.ctrl || press.alt != spec.alt || press.shift != spec.shift) {
        return false;
    }
    if (spec.key != tuinator::Key::Unknown) {
        return press.key == spec.key;
    }
    if (spec.character != '\0') {
        return press.character == spec.character;
    }
    return false;
}

bool KeyBindings::matches_quit(const tuinator::KeyPress& press) const {
    return matches(press, quit_key) || matches(press, quit_escape);
}

KeyBindings default_keybindings() { return KeyBindings{}; }

std::optional<KeyBindingSpec> parse_key_binding(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }

    KeyBindingSpec spec;
    std::string normalized = lowercase_copy(text);

    if (normalized == "space") {
        spec.character = ' ';
        return spec;
    }
    if (normalized == "escape" || normalized == "esc") {
        spec.key = tuinator::Key::Escape;
        return spec;
    }
    if (normalized == "tab") {
        spec.key = tuinator::Key::Tab;
        return spec;
    }
    if (normalized == "enter") {
        spec.key = tuinator::Key::Enter;
        return spec;
    }

    if (normalized.rfind("shift+", 0) == 0) {
        spec.shift = true;
        normalized = normalized.substr(6);
    }
    if (normalized.rfind("ctrl+", 0) == 0) {
        spec.ctrl = true;
        normalized = normalized.substr(5);
    }
    if (normalized.rfind("alt+", 0) == 0) {
        spec.alt = true;
        normalized = normalized.substr(4);
    }

    if (normalized.size() == 1) {
        spec.character = normalized[0];
        return spec;
    }

    const tuinator::Key function_key = function_key_from_name(normalized);
    if (function_key != tuinator::Key::Unknown) {
        spec.key = function_key;
        return spec;
    }

    return std::nullopt;
}

KeyBindings parse_keybindings(const std::unordered_map<std::string, std::string>& values) {
    KeyBindings bindings = default_keybindings();
    for (const auto& [name, raw] : values) {
        const std::optional<KeyBindingSpec> parsed = parse_key_binding(raw);
        if (!parsed.has_value()) {
            continue;
        }
        if (name == "continue") {
            bindings.continue_key = *parsed;
        } else if (name == "step_over") {
            bindings.step_over = *parsed;
        } else if (name == "step_into") {
            bindings.step_into = *parsed;
        } else if (name == "step_out") {
            bindings.step_out = *parsed;
        } else if (name == "breakpoint") {
            bindings.breakpoint = *parsed;
        } else if (name == "focus_repl") {
            bindings.focus_repl = *parsed;
        } else if (name == "follow_execution") {
            bindings.follow_execution = *parsed;
        } else if (name == "file_picker") {
            bindings.file_picker = *parsed;
        } else if (name == "panel_prev") {
            bindings.panel_prev = *parsed;
        } else if (name == "panel_next") {
            bindings.panel_next = *parsed;
        } else if (name == "quit") {
            bindings.quit_key = *parsed;
        } else if (name == "continue_fn") {
            bindings.continue_fn = *parsed;
        } else if (name == "step_over_fn") {
            bindings.step_over_fn = *parsed;
        } else if (name == "step_into_fn") {
            bindings.step_into_fn = *parsed;
        } else if (name == "step_out_fn") {
            bindings.step_out_fn = *parsed;
        } else if (name == "breakpoint_fn") {
            bindings.breakpoint_fn = *parsed;
        }
    }
    return bindings;
}

}  // namespace tui_debug_ui
