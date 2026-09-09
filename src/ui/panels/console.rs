//! Console output panel — mirrors nvim-dap-ui console element.

use ratatui::crossterm::event::{KeyCode, KeyEvent};
use ratatui::layout::Rect;
use ratatui::style::Style;
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
pub struct ConsolePanel {
    visible_height: u16,
}

impl ConsolePanel {
    fn max_scroll(state: &AppState, visible_height: u16) -> u16 {
        let line_count = state.console_lines.len() as u16;
        line_count.saturating_sub(visible_height)
    }

    fn clamp_scroll(state: &mut AppState, visible_height: u16) {
        let max = Self::max_scroll(state, visible_height);
        if state.console_scroll > max {
            state.console_scroll = max;
        }
    }
}

impl Panel for ConsolePanel {
    fn id(&self) -> &'static str {
        "console"
    }

    fn title(&self) -> &'static str {
        "Console"
    }

    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        let focused = is_focused(state, Focus::Console);
        let block = panel_block(self.title(), focused);

        let inner = block.inner(area);
        self.visible_height = inner.height.max(1);

        let lines: Vec<Line> = state
            .console_lines
            .iter()
            .map(|line| {
                Line::from(vec![
                    Span::styled(
                        format!("[{}] ", line.category),
                        theme::control_disabled(),
                    ),
                    Span::raw(line.text.trim_end()),
                ])
            })
            .collect();

        let paragraph = Paragraph::new(lines)
            .block(block)
            .scroll((state.console_scroll, 0));

        frame.render_widget(paragraph, area);
    }

    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        _session: &mut DebugSession,
    ) -> PanelAction {
        match key.code {
            KeyCode::Char('j') => {
                let max = Self::max_scroll(state, self.visible_height);
                if state.console_scroll < max {
                    state.console_scroll += 1;
                }
            }
            KeyCode::Char('k') => {
                if state.console_scroll > 0 {
                    state.console_scroll -= 1;
                }
            }
            _ => {}
        }

        Self::clamp_scroll(state, self.visible_height);
        PanelAction::None
    }
}
