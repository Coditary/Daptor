//! Breakpoints list panel — mirrors nvim-dap-ui breakpoints element.

use ratatui::crossterm::event::{KeyCode, KeyEvent};
use ratatui::layout::Rect;
use ratatui::widgets::{List, ListItem, ListState};
use ratatui::Frame;

use crate::session::DebugSession;
use crate::ui::chrome::panel_block;
use crate::ui::layout::is_focused;
use crate::ui::panel::{Panel, PanelAction};
use crate::ui::state::{AppState, Focus};
use crate::ui::theme;

#[derive(Debug, Default)]
pub struct BreakpointsPanel {
    list_state: ListState,
}

impl BreakpointsPanel {
    fn clamp_selection(state: &mut AppState) {
        if state.breakpoints.is_empty() {
            state.breakpoints_selected = 0;
        } else if state.breakpoints_selected >= state.breakpoints.len() {
            state.breakpoints_selected = state.breakpoints.len() - 1;
        }
    }

    fn toggle_selected(state: &mut AppState) {
        if let Some(bp) = state.breakpoints.get_mut(state.breakpoints_selected) {
            bp.enabled = !bp.enabled;
        }
    }

    fn remove_selected(state: &mut AppState) {
        if state.breakpoints_selected < state.breakpoints.len() {
            state.breakpoints.remove(state.breakpoints_selected);
            Self::clamp_selection(state);
        }
    }
}

impl Panel for BreakpointsPanel {
    fn id(&self) -> &'static str {
        "breakpoints"
    }

    fn title(&self) -> &'static str {
        "Breakpoints"
    }

    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        let focused = is_focused(state, Focus::Breakpoints);
        let block = panel_block(self.title(), focused);

        let items: Vec<ListItem> = state
            .breakpoints
            .iter()
            .map(|bp| {
                let marker = if bp.enabled { "●" } else { "○" };
                let text = format!("[{}] {}:{}", marker, bp.path.display(), bp.line);
                ListItem::from(text)
            })
            .collect();

        let list = List::new(items)
            .block(block)
            .highlight_style(theme::selection())
            .highlight_symbol("> ");

        self.list_state.select(if state.breakpoints.is_empty() {
            None
        } else {
            Some(state.breakpoints_selected)
        });

        frame.render_stateful_widget(list, area, &mut self.list_state);
    }

    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        _session: &mut DebugSession,
    ) -> PanelAction {
        match key.code {
            KeyCode::Char('j') => {
                if state.breakpoints_selected + 1 < state.breakpoints.len() {
                    state.breakpoints_selected += 1;
                }
            }
            KeyCode::Char('k') => {
                if state.breakpoints_selected > 0 {
                    state.breakpoints_selected -= 1;
                }
            }
            KeyCode::Char('t') | KeyCode::Enter => {
                Self::toggle_selected(state);
            }
            KeyCode::Char('d') => {
                Self::remove_selected(state);
            }
            _ => {}
        }
        PanelAction::None
    }
}
