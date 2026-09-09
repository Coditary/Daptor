//! Debug controls winbar — centered, full-width (nvim-dap-ui style).

use ratatui::crossterm::event::{KeyCode, KeyEvent, MouseButton, MouseEvent, MouseEventKind};
use ratatui::layout::Rect;
use ratatui::style::{Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;
use ratatui::Frame;

use crate::session::SessionState;
use crate::ui::icons::controls as ic;
use crate::ui::panel::PanelAction;
use crate::ui::state::AppState;
use crate::ui::theme::{self, bg};

const CONTROL_COUNT: usize = 8;

struct ControlButton {
    icon: &'static str,
    action: PanelAction,
    needs_stopped: bool,
    always_available: bool,
}

const BUTTONS: [ControlButton; CONTROL_COUNT] = [
    ControlButton { icon: ic::PLAY, action: PanelAction::PlayPause, needs_stopped: false, always_available: false },
    ControlButton { icon: ic::STEP_INTO, action: PanelAction::StepInto, needs_stopped: true, always_available: false },
    ControlButton { icon: ic::STEP_OVER, action: PanelAction::StepOver, needs_stopped: true, always_available: false },
    ControlButton { icon: ic::STEP_OUT, action: PanelAction::StepOut, needs_stopped: true, always_available: false },
    ControlButton { icon: ic::STEP_BACK, action: PanelAction::StepBack, needs_stopped: true, always_available: false },
    ControlButton { icon: ic::RUN_LAST, action: PanelAction::Restart, needs_stopped: false, always_available: false },
    ControlButton { icon: ic::TERMINATE, action: PanelAction::Terminate, needs_stopped: false, always_available: true },
    ControlButton { icon: ic::DISCONNECT, action: PanelAction::Disconnect, needs_stopped: false, always_available: true },
];

fn is_stopped(state: &AppState) -> bool {
    matches!(state.session_state, SessionState::Stopped { .. })
}

fn is_running(state: &AppState) -> bool {
    matches!(state.session_state, SessionState::Running)
}

fn has_session(state: &AppState) -> bool {
    !matches!(state.session_state, SessionState::Disconnected | SessionState::Exited)
}

fn button_available(state: &AppState, btn: &ControlButton) -> bool {
    if !state.is_connected() {
        return false;
    }
    if !has_session(state) {
        return false;
    }
    if btn.always_available {
        return true;
    }
    if btn.action == PanelAction::PlayPause {
        return is_stopped(state) || is_running(state);
    }
    if btn.action == PanelAction::Restart {
        return true;
    }
    if btn.needs_stopped {
        return is_stopped(state);
    }
    true
}

fn button_style(state: &AppState, btn: &ControlButton, hovered: bool) -> Style {
    if !button_available(state, btn) {
        return theme::control_disabled();
    }
    let base = match btn.action {
        PanelAction::PlayPause => theme::control_play(),
        PanelAction::Terminate | PanelAction::Disconnect => theme::control_stop(),
        PanelAction::Restart => theme::control_restart(),
        _ => theme::control_step(),
    };
    if hovered {
        base.add_modifier(Modifier::REVERSED)
    } else {
        base
    }
}

fn play_pause_icon(state: &AppState) -> &'static str {
    if is_running(state) && !is_stopped(state) {
        ic::PAUSE
    } else {
        ic::PLAY
    }
}

pub fn render_controls(frame: &mut Frame, area: Rect, state: &mut AppState) {
    state.control_areas.clear();

    let mut icon_spans = Vec::new();
    for (i, btn) in BUTTONS.iter().enumerate() {
        let icon = if btn.action == PanelAction::PlayPause {
            play_pause_icon(state)
        } else {
            btn.icon
        };
        let hovered = state.controls_hover == Some(i);
        icon_spans.push(Span::styled(
            format!("  {icon}  "),
            button_style(state, btn, hovered),
        ));
    }

    let line = Line::from(icon_spans);
    let content_width: u16 = BUTTONS.len() as u16 * 5;
    let pad = area.width.saturating_sub(content_width) / 2;

    frame.render_widget(
        Paragraph::new(line).style(Style::default().bg(bg::CONTROL_BAR)),
        area,
    );

    let start_x = area.x + pad;
    for (i, btn) in BUTTONS.iter().enumerate() {
        let x = start_x + (i as u16 * 5);
        if x + 5 <= area.x + area.width {
            state.control_areas.push((Rect::new(x, area.y, 5, area.height), btn.action));
        }
    }
}

pub fn handle_mouse(state: &AppState, mouse: MouseEvent) -> Option<PanelAction> {
    if !matches!(mouse.kind, MouseEventKind::Down(MouseButton::Left)) {
        return None;
    }
    for (rect, action) in &state.control_areas {
        if mouse.column >= rect.x
            && mouse.column < rect.x + rect.width
            && mouse.row >= rect.y
            && mouse.row < rect.y + rect.height
        {
            let btn = BUTTONS.iter().find(|b| b.action == *action)?;
            if button_available(state, btn) {
                return Some(*action);
            }
        }
    }
    None
}

pub fn update_hover(state: &mut AppState, mouse: &MouseEvent) {
    if !matches!(mouse.kind, MouseEventKind::Moved) {
        return;
    }
    state.controls_hover = state.control_areas.iter().position(|(rect, _)| {
        mouse.column >= rect.x
            && mouse.column < rect.x + rect.width
            && mouse.row >= rect.y
            && mouse.row < rect.y + rect.height
    });
}

pub fn handle_key(state: &AppState, key: KeyEvent) -> Option<PanelAction> {
    let index = match key.code {
        KeyCode::Char('1') => 0,
        KeyCode::Char('2') => 1,
        KeyCode::Char('3') => 2,
        KeyCode::Char('4') => 3,
        KeyCode::Char('5') => 4,
        KeyCode::Char('6') => 5,
        KeyCode::Char('7') => 6,
        KeyCode::Char('8') => 7,
        _ => return None,
    };
    let btn = BUTTONS.get(index)?;
    if button_available(state, btn) {
        Some(btn.action)
    } else {
        None
    }
}
