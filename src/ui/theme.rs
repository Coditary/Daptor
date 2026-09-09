//! Color palette inspired by nvim-dap-ui highlight groups.

use ratatui::style::{Color, Modifier, Style};

pub mod bg {
    use ratatui::style::Color;
    pub const ROOT: Color = Color::Rgb(24, 24, 27);
    pub const PANEL: Color = Color::Rgb(30, 30, 36);
    pub const CONTROL_BAR: Color = Color::Rgb(40, 40, 48);
    pub const CURRENT_LINE: Color = Color::Rgb(28, 80, 48);
    pub const CURSOR_LINE: Color = Color::Rgb(45, 45, 55);
}

pub fn border_focused() -> Style {
    Style::default().fg(Color::Cyan)
}

pub fn border_normal() -> Style {
    Style::default().fg(Color::Rgb(68, 68, 76))
}

pub fn title_focused(_title: &str) -> Style {
    Style::default()
        .fg(Color::Cyan)
        .add_modifier(Modifier::BOLD)
}

pub fn title_normal(_title: &str) -> Style {
    Style::default().fg(Color::Rgb(120, 120, 130))
}

pub fn line_number() -> Style {
    Style::default().fg(Color::Rgb(100, 100, 110))
}

pub fn execution_line() -> Style {
    Style::default()
        .fg(Color::White)
        .bg(bg::CURRENT_LINE)
        .add_modifier(Modifier::BOLD)
}

pub fn cursor_line() -> Style {
    Style::default().fg(Color::White).bg(bg::CURSOR_LINE)
}

pub fn source_text() -> Style {
    Style::default().fg(Color::Rgb(220, 220, 225))
}

pub fn breakpoint_marker() -> Style {
    Style::default().fg(Color::Rgb(220, 60, 60)).add_modifier(Modifier::BOLD)
}

pub fn scope_header() -> Style {
    Style::default()
        .fg(Color::Cyan)
        .add_modifier(Modifier::BOLD)
}

pub fn variable_name() -> Style {
    Style::default().fg(Color::Rgb(180, 200, 255))
}

pub fn variable_value() -> Style {
    Style::default().fg(Color::Rgb(220, 220, 220))
}

pub fn variable_type() -> Style {
    Style::default().fg(Color::Rgb(140, 180, 220))
}

pub fn thread_header_stopped() -> Style {
    Style::default()
        .fg(Color::Green)
        .add_modifier(Modifier::BOLD)
}

pub fn thread_header() -> Style {
    Style::default().fg(Color::Cyan)
}

pub fn frame_current() -> Style {
    Style::default()
        .fg(Color::Yellow)
        .add_modifier(Modifier::BOLD)
}

pub fn frame_normal() -> Style {
    Style::default().fg(Color::Rgb(200, 200, 210))
}

pub fn selection() -> Style {
    Style::default()
        .fg(Color::Black)
        .bg(Color::Cyan)
        .add_modifier(Modifier::BOLD)
}

pub fn control_play() -> Style {
    Style::default().fg(Color::Green).add_modifier(Modifier::BOLD)
}

pub fn control_step() -> Style {
    Style::default().fg(Color::Rgb(120, 180, 255)).add_modifier(Modifier::BOLD)
}

pub fn control_stop() -> Style {
    Style::default().fg(Color::Rgb(220, 80, 80)).add_modifier(Modifier::BOLD)
}

pub fn control_disabled() -> Style {
    Style::default().fg(Color::Rgb(80, 80, 90))
}

pub fn control_restart() -> Style {
    Style::default().fg(Color::Yellow).add_modifier(Modifier::BOLD)
}

pub fn status_bar() -> Style {
    Style::default().fg(Color::Black).bg(Color::Rgb(80, 160, 200))
}

pub fn divider_idle() -> Style {
    Style::default().fg(Color::Rgb(72, 72, 82))
}

pub fn divider_hover() -> Style {
    Style::default().fg(Color::Rgb(120, 180, 220))
}

pub fn divider_active() -> Style {
    Style::default()
        .fg(Color::Cyan)
        .add_modifier(Modifier::BOLD)
}

/// Very small Python highlighter for the source panel.
pub fn highlight_python(line: &str) -> Vec<(String, Style)> {
    let trimmed = line.trim_start();
    if trimmed.starts_with('#') || trimmed.starts_with("\"\"\"") {
        return vec![(line.to_string(), Style::default().fg(Color::Rgb(100, 140, 100)))];
    }

    let keywords = [
        "def", "class", "import", "from", "return", "for", "in", "if", "elif", "else", "while",
        "True", "False", "None", "and", "or", "not", "as", "with", "pass", "break", "continue",
    ];

    let mut spans = Vec::new();
    let mut rest = line;

    while !rest.is_empty() {
        if rest.starts_with(' ') {
            let len = rest.chars().take_while(|c| *c == ' ').count();
            spans.push((rest[..len].to_string(), source_text()));
            rest = &rest[len..];
            continue;
        }
        if rest.starts_with('"') || rest.starts_with('\'') {
            if let Some(end) = find_string_end(rest) {
                spans.push((
                    rest[..end].to_string(),
                    Style::default().fg(Color::Rgb(180, 220, 140)),
                ));
                rest = &rest[end..];
                continue;
            }
        }
        if rest.starts_with(|c: char| c.is_ascii_digit()) {
            let len = rest.chars().take_while(|c| c.is_ascii_digit() || *c == '.').count();
            spans.push((
                rest[..len].to_string(),
                Style::default().fg(Color::Rgb(140, 200, 220)),
            ));
            rest = &rest[len..];
            continue;
        }
        if rest.starts_with(|c: char| c.is_ascii_alphabetic() || c == '_') {
            let len = rest
                .chars()
                .take_while(|c| c.is_ascii_alphanumeric() || *c == '_')
                .count();
            let word = &rest[..len];
            let style = if keywords.contains(&word) {
                Style::default().fg(Color::Rgb(200, 140, 220)).add_modifier(Modifier::BOLD)
            } else {
                source_text()
            };
            spans.push((word.to_string(), style));
            rest = &rest[len..];
            continue;
        }
        spans.push((rest[..1].to_string(), source_text()));
        rest = &rest[1..];
    }

    spans
}

fn find_string_end(s: &str) -> Option<usize> {
    let quote = s.chars().next()?;
    let mut escaped = false;
    for (i, ch) in s.char_indices().skip(1) {
        if escaped {
            escaped = false;
            continue;
        }
        if ch == '\\' {
            escaped = true;
            continue;
        }
        if ch == quote {
            return Some(i + ch.len_utf8());
        }
    }
    None
}
