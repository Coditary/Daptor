//! Floating windows — mirrors nvim-dap-ui `float_element` and `eval`.

use ratatui::crossterm::event::{KeyCode, KeyEvent};
use ratatui::layout::{Alignment, Rect};
use ratatui::style::{Color, Modifier, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph, Wrap};
use ratatui::Frame;

use crate::session::DebugSession;
use crate::ui::panel::{Panel, PanelAction};
use crate::ui::panels::AllPanels;
use crate::ui::state::{AppState, FloatElement, FloatKind};

/// Compute a centered rectangle (percent of parent).
pub fn centered_rect(percent_x: u16, percent_y: u16, area: Rect) -> Rect {
    let popup_layout = ratatui::layout::Layout::default()
        .direction(ratatui::layout::Direction::Vertical)
        .constraints([
            ratatui::layout::Constraint::Percentage((100 - percent_y) / 2),
            ratatui::layout::Constraint::Percentage(percent_y),
            ratatui::layout::Constraint::Percentage((100 - percent_y) / 2),
        ])
        .split(area);

    ratatui::layout::Layout::default()
        .direction(ratatui::layout::Direction::Horizontal)
        .constraints([
            ratatui::layout::Constraint::Percentage((100 - percent_x) / 2),
            ratatui::layout::Constraint::Percentage(percent_x),
            ratatui::layout::Constraint::Percentage((100 - percent_x) / 2),
        ])
        .split(popup_layout[1])[1]
}

pub fn open_element(state: &mut AppState, element: FloatElement) {
    state.float = FloatKind::Element {
        element,
        enter: true,
    };
}

pub fn open_eval(state: &mut AppState, expression: String, result: String, error: Option<String>) {
    state.float = FloatKind::Eval {
        expression,
        result,
        error,
    };
}

pub fn close(state: &mut AppState) {
    state.float = FloatKind::None;
}

pub fn is_open(state: &AppState) -> bool {
    !matches!(state.float, FloatKind::None)
}

pub fn float_title(state: &AppState) -> String {
    match &state.float {
        FloatKind::None => String::new(),
        FloatKind::Eval { expression, .. } => format!(" Eval: {expression} "),
        FloatKind::Element { element, .. } => format!(" {element} "),
    }
}

/// Render floating overlay on top of the main UI.
pub fn render_float(
    frame: &mut Frame,
    area: Rect,
    state: &AppState,
    panels: &mut AllPanels,
    _session: &mut DebugSession,
) {
    if matches!(state.float, FloatKind::None) {
        return;
    }

    // Dim full screen behind popup
    let dim = Block::default().style(Style::default().bg(Color::Rgb(20, 20, 20)));
    frame.render_widget(dim, area);

    let popup = centered_rect(60, 50, area);
    frame.render_widget(Clear, popup);

    let title = float_title(state);
    let block = Block::default()
        .borders(Borders::ALL)
        .border_style(Style::default().fg(Color::Cyan))
        .title(title)
        .title_alignment(Alignment::Center);

    match &state.float {
        FloatKind::Eval {
            expression: _,
            result,
            error,
        } => {
            let inner = block.inner(popup);
            frame.render_widget(block, popup);

            let text = if let Some(err) = error {
                vec![Line::from(vec![
                    Span::styled("Error: ", Style::default().fg(Color::Red)),
                    Span::raw(err.clone()),
                ])]
            } else {
                vec![
                    Line::from(Span::styled(
                        result.clone(),
                        Style::default().add_modifier(Modifier::BOLD),
                    )),
                ]
            };
            frame.render_widget(
                Paragraph::new(text).wrap(Wrap { trim: false }),
                inner,
            );
        }
        FloatKind::Element { element, .. } => {
            let inner = block.inner(popup);
            frame.render_widget(block, popup);
            match element {
                FloatElement::Scopes => panels.scopes.render(frame, inner, state),
                FloatElement::Stacks => panels.stacks.render(frame, inner, state),
                FloatElement::Breakpoints => panels.breakpoints.render(frame, inner, state),
                FloatElement::Watches => panels.watches.render(frame, inner, state),
                FloatElement::Repl => panels.repl.render(frame, inner, state),
                FloatElement::Console => panels.console.render(frame, inner, state),
            }
        }
        FloatKind::None => {}
    }

    // Hint bar at bottom of popup
    let hint_y = popup.y + popup.height.saturating_sub(1);
    if hint_y < area.y + area.height {
        let hint = Rect::new(popup.x + 1, hint_y, popup.width.saturating_sub(2), 1);
        frame.render_widget(
            Paragraph::new(" q/Esc: close ")
                .style(Style::default().fg(Color::DarkGray)),
            hint,
        );
    }
}

/// Global key handling when a float is open (nvim-dap-ui floating mappings).
pub fn handle_key(key: KeyEvent, state: &mut AppState) -> bool {
    if !is_open(state) {
        return false;
    }
    match key.code {
        KeyCode::Esc | KeyCode::Char('q') => {
            close(state);
            true
        }
        _ => false,
    }
}

/// Run eval for expression under cursor / word and open float.
pub fn eval_expression(
    state: &mut AppState,
    session: &mut DebugSession,
    expression: Option<String>,
) -> PanelAction {
    let expr = expression.unwrap_or_else(|| state.word_under_cursor());
    if expr.is_empty() {
        state.status_message = "No expression to evaluate".into();
        return PanelAction::None;
    }

    let frame_id = match state.stack_frames.first() {
        Some(f) => f.id,
        None => {
            state.status_message = "No active frame for eval".into();
            return PanelAction::None;
        }
    };

    match session.evaluate(&expr, frame_id, "hover") {
        Ok(result) => {
            open_eval(state, expr, result, None);
            PanelAction::None
        }
        Err(err) => {
            open_eval(state, expr, String::new(), Some(err.to_string()));
            PanelAction::None
        }
    }
}
