//! Live input inspector — shows whether keyboard/mouse events reach the TUI.

use std::collections::VecDeque;

use ratatui::crossterm::event::{
    Event, KeyCode, KeyEvent, KeyModifiers, MouseButton, MouseEvent, MouseEventKind,
};
use ratatui::layout::Rect;
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph};
use ratatui::Frame;

use crate::ui::state::AppState;

const MAX_LOG: usize = 8;

#[derive(Debug, Clone, Default)]
pub struct InputDebug {
    /// F12 toggles the overlay.
    pub visible: bool,
    pub total_events: u64,
    pub total_keys: u64,
    pub total_mouse: u64,
    pub total_resize: u64,
    pub frames: u64,
    pub polls_empty: u64,
    pub polls_hit: u64,
    pub log: VecDeque<String>,
    pub last_action: String,
}

impl InputDebug {
    pub fn new() -> Self {
        Self {
            visible: true,
            last_action: "started — waiting for input".into(),
            ..Default::default()
        }
    }
}

pub fn tick_frame(debug: &mut InputDebug) {
    debug.frames += 1;
}

pub fn tick_poll_empty(debug: &mut InputDebug) {
    debug.polls_empty += 1;
}

pub fn record_event(state: &mut AppState, event: &Event) {
    let debug = &mut state.input_debug;
    debug.total_events += 1;
    debug.polls_hit += 1;

    let line = match event {
        Event::Key(key) => {
            debug.total_keys += 1;
            format!("KEY  {}", describe_key(key))
        }
        Event::Mouse(mouse) => {
            debug.total_mouse += 1;
            format!("MOUSE {}", describe_mouse(mouse))
        }
        Event::Resize(cols, rows) => {
            debug.total_resize += 1;
            format!("RESIZE {cols}x{rows}")
        }
        Event::FocusGained => "FOCUS gained".into(),
        Event::FocusLost => "FOCUS lost".into(),
        Event::Paste(text) => format!("PASTE {} chars", text.len()),
    };

    push_log(debug, line);
}

pub fn set_action(state: &mut AppState, action: impl Into<String>) {
    state.input_debug.last_action = action.into();
}

pub fn toggle_visible(state: &mut AppState) {
    state.input_debug.visible = !state.input_debug.visible;
    let on_off = if state.input_debug.visible { "on" } else { "off" };
    set_action(state, format!("input debug overlay {on_off} (F12)"));
}

pub fn describe_key(key: &KeyEvent) -> String {
    let mut mods = Vec::new();
    if key.modifiers.contains(KeyModifiers::CONTROL) {
        mods.push("Ctrl");
    }
    if key.modifiers.contains(KeyModifiers::ALT) {
        mods.push("Alt");
    }
    if key.modifiers.contains(KeyModifiers::SHIFT) {
        mods.push("Shift");
    }
    if key.modifiers.contains(KeyModifiers::SUPER) {
        mods.push("Super");
    }

    let code = match key.code {
        KeyCode::Char(c) => format!("'{c}'"),
        KeyCode::Enter => "Enter".into(),
        KeyCode::Esc => "Esc".into(),
        KeyCode::Tab => "Tab".into(),
        KeyCode::BackTab => "Shift+Tab".into(),
        KeyCode::Backspace => "Backspace".into(),
        KeyCode::Delete => "Delete".into(),
        KeyCode::Up => "↑".into(),
        KeyCode::Down => "↓".into(),
        KeyCode::Left => "←".into(),
        KeyCode::Right => "→".into(),
        KeyCode::Home => "Home".into(),
        KeyCode::End => "End".into(),
        KeyCode::PageUp => "PgUp".into(),
        KeyCode::PageDown => "PgDn".into(),
        KeyCode::F(n) => format!("F{n}"),
        KeyCode::Null => "Null".into(),
        other => format!("{other:?}"),
    };

    if mods.is_empty() {
        code
    } else {
        format!("{}+{}", mods.join("+"), code)
    }
}

pub fn describe_mouse(mouse: &MouseEvent) -> String {
    let kind = match mouse.kind {
        MouseEventKind::Down(MouseButton::Left) => "DownL",
        MouseEventKind::Down(MouseButton::Right) => "DownR",
        MouseEventKind::Down(MouseButton::Middle) => "DownM",
        MouseEventKind::Up(MouseButton::Left) => "UpL",
        MouseEventKind::Up(MouseButton::Right) => "UpR",
        MouseEventKind::Up(MouseButton::Middle) => "UpM",
        MouseEventKind::Drag(MouseButton::Left) => "DragL",
        MouseEventKind::Drag(MouseButton::Right) => "DragR",
        MouseEventKind::Drag(MouseButton::Middle) => "DragM",
        MouseEventKind::Moved => "Move",
        MouseEventKind::ScrollUp => "ScrollUp",
        MouseEventKind::ScrollDown => "ScrollDn",
        MouseEventKind::ScrollLeft => "ScrollL",
        MouseEventKind::ScrollRight => "ScrollR",
    };
    format!("{kind} @ {},{}", mouse.column, mouse.row)
}

pub fn render(
    frame: &mut Frame,
    root: Rect,
    state: &AppState,
    slot_label: &str,
) {
    let debug = &state.input_debug;
    if !debug.visible {
        return;
    }

    let width = 46.min(root.width);
    let height = 14.min(root.height);
    let x = root.x + root.width.saturating_sub(width);
    let area = Rect::new(x, root.y, width, height);

    frame.render_widget(Clear, area);
    frame.render_widget(
        Block::default()
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Yellow))
            .title(" Input debug (F12) ")
            .style(Style::default().bg(Color::Rgb(18, 18, 24))),
        area,
    );

    let inner = Block::default().inner(area);
    let conn = format!("{:?}", state.connection_state);
    let alive = if debug.frames % 20 < 10 { "●" } else { "○" };

    let mut lines = vec![
        Line::from(vec![
            Span::styled(format!("{alive} loop "), Style::default().fg(Color::Green)),
            Span::raw(format!(
                "frames={} poll_hit={} poll_miss={}",
                debug.frames,
                debug.polls_hit,
                debug.polls_empty
            )),
        ]),
        Line::from(format!(
            "events: {} (keys {} mouse {} resize {})",
            debug.total_events,
            debug.total_keys,
            debug.total_mouse,
            debug.total_resize
        )),
        Line::from(format!("conn={conn}  session={slot_label}")),
        Line::from(format!(
            "ui_open={}  focus={:?}",
            state.ui_open,
            state.focus
        )),
        Line::from(Span::styled(
            format!("last: {}", debug.last_action),
            Style::default()
                .fg(Color::Cyan)
                .add_modifier(Modifier::BOLD),
        )),
        Line::from(""),
        Line::from(Span::styled("recent:", Style::default().fg(Color::DarkGray))),
    ];

    for entry in debug.log.iter() {
        lines.push(Line::from(Span::styled(
            entry.clone(),
            Style::default().fg(Color::Rgb(200, 200, 140)),
        )));
    }

    if debug.log.is_empty() {
        lines.push(Line::from(Span::styled(
            "(no events yet — press keys / click)",
            Style::default().fg(Color::DarkGray),
        )));
    }

    frame.render_widget(Paragraph::new(lines), inner);
}

fn push_log(debug: &mut InputDebug, line: String) {
    debug.log.push_front(line);
    while debug.log.len() > MAX_LOG {
        debug.log.pop_back();
    }
}
