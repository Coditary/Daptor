//! REPL panel — mirrors nvim-dap-ui repl element.

use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::text::{Line, Span};
use ratatui::widgets::Paragraph;
use ratatui::Frame;

use crate::session::DebugSession;
use crate::ui::chrome::panel_block;
use crate::ui::layout::is_focused;
use crate::ui::panel::{Panel, PanelAction};
use crate::ui::state::{AppState, Focus};
use crate::ui::theme;

#[derive(Debug, Default)]
pub struct ReplPanel;

impl ReplPanel {
    fn insert_char(state: &mut AppState, c: char) {
        state.repl_input.push(c);
    }
}

impl Panel for ReplPanel {
    fn id(&self) -> &'static str {
        "repl"
    }

    fn title(&self) -> &'static str {
        "REPL"
    }

    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        let focused = is_focused(state, Focus::Repl);
        let block = panel_block(self.title(), focused);

        let inner = block.inner(area);
        frame.render_widget(block, area);

        let chunks = Layout::default()
            .direction(Direction::Vertical)
            .constraints([Constraint::Min(0), Constraint::Length(1)])
            .split(inner);

        let history_lines: Vec<Line> = state
            .repl_history
            .iter()
            .flat_map(|entry| entry.lines().map(|line| Line::from(Span::raw(line))))
            .collect();

        let history_height = chunks[0].height as usize;
        let skip = history_lines.len().saturating_sub(history_height);
        let visible: Vec<Line> = history_lines.into_iter().skip(skip).collect();

        let history = Paragraph::new(visible);
        frame.render_widget(history, chunks[0]);

        let cursor = if focused { "▏" } else { "" };
        let prompt = Paragraph::new(format!("> {}{cursor}", state.repl_input))
            .style(if focused {
                theme::frame_current()
            } else {
                theme::source_text()
            });
        frame.render_widget(prompt, chunks[1]);
    }

    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        _session: &mut DebugSession,
    ) -> PanelAction {
        if key.modifiers.contains(KeyModifiers::CONTROL) {
            match key.code {
                KeyCode::Char('u') => {
                    state.repl_input.clear();
                    return PanelAction::None;
                }
                KeyCode::Char('a') => {
                    // no cursor position yet — treat as clear-to-start noop
                    return PanelAction::None;
                }
                _ => {}
            }
        }

        match key.code {
            KeyCode::Enter => {
                if state.repl_input.trim().is_empty() {
                    state.repl_input.clear();
                    return PanelAction::None;
                }
                return PanelAction::EvaluateRepl;
            }
            KeyCode::Esc => {
                state.repl_input.clear();
            }
            KeyCode::Backspace => {
                state.repl_input.pop();
            }
            KeyCode::Char(c) if !key.modifiers.contains(KeyModifiers::CONTROL) => {
                Self::insert_char(state, c);
            }
            _ => {}
        }
        PanelAction::None
    }
}
