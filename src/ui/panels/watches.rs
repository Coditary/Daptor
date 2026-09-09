//! Watch expressions panel — mirrors nvim-dap-ui watches element.

use ratatui::crossterm::event::{KeyCode, KeyEvent};
use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::{Modifier, Style};
use ratatui::widgets::{List, ListItem, ListState, Paragraph};
use ratatui::Frame;

use crate::session::DebugSession;
use crate::ui::chrome::panel_block;
use crate::ui::layout::is_focused;
use crate::ui::panel::{Panel, PanelAction};
use crate::ui::state::{AppState, Focus, WatchEntry};
use crate::ui::theme;

#[derive(Debug, Default)]
pub struct WatchesPanel {
    list_state: ListState,
    insert_mode: bool,
    input_buffer: String,
}

impl WatchesPanel {
    fn clamp_selection(state: &mut AppState) {
        if state.watches.is_empty() {
            state.watches_selected = 0;
        } else if state.watches_selected >= state.watches.len() {
            state.watches_selected = state.watches.len() - 1;
        }
    }

    fn submit_watch(state: &mut AppState, expression: String) {
        let expression = expression.trim().to_string();
        if expression.is_empty() {
            return;
        }

        let id = state.next_watch_id;
        state.next_watch_id += 1;
        state.watches.push(WatchEntry {
            id,
            expression,
            value: String::new(),
            error: None,
        });
        state.watches_selected = state.watches.len().saturating_sub(1);
        state.status_message = format!("Added watch #{id}");
    }

    fn remove_selected(state: &mut AppState) {
        if state.watches_selected < state.watches.len() {
            state.watches.remove(state.watches_selected);
            Self::clamp_selection(state);
        }
    }

    fn handle_insert_key(&mut self, key: KeyEvent, state: &mut AppState) -> PanelAction {
        match key.code {
            KeyCode::Enter => {
                let expression = std::mem::take(&mut self.input_buffer);
                Self::submit_watch(state, expression);
                self.insert_mode = false;
                PanelAction::Refresh
            }
            KeyCode::Esc => {
                self.input_buffer.clear();
                self.insert_mode = false;
                PanelAction::None
            }
            KeyCode::Backspace => {
                self.input_buffer.pop();
                PanelAction::None
            }
            KeyCode::Char(c) => {
                self.input_buffer.push(c);
                PanelAction::None
            }
            _ => PanelAction::None,
        }
    }
}

impl Panel for WatchesPanel {
    fn id(&self) -> &'static str {
        "watches"
    }

    fn title(&self) -> &'static str {
        "Watches"
    }

    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        let focused = is_focused(state, Focus::Watches);
        let block = panel_block(self.title(), focused);

        let inner = block.inner(area);
        frame.render_widget(block, area);

        if self.insert_mode {
            let prompt = Paragraph::new(format!("> {}", self.input_buffer))
                .style(theme::frame_current());
            frame.render_widget(prompt, inner);
            return;
        }

        let items: Vec<ListItem> = state
            .watches
            .iter()
            .map(|watch| {
                let text = if let Some(err) = &watch.error {
                    format!("{} = <error: {}>", watch.expression, err)
                } else if watch.value.is_empty() {
                    format!("{} = ?", watch.expression)
                } else {
                    format!("{} = {}", watch.expression, watch.value)
                };
                ListItem::from(text)
            })
            .collect();

        let list = List::new(items)
            .highlight_style(Style::default().add_modifier(Modifier::REVERSED))
            .highlight_symbol("> ");

        self.list_state.select(if state.watches.is_empty() {
            None
        } else {
            Some(state.watches_selected)
        });

        if inner.height <= 1 {
            frame.render_stateful_widget(list, inner, &mut self.list_state);
        } else {
            let chunks = Layout::default()
                .direction(Direction::Vertical)
                .constraints([Constraint::Min(0), Constraint::Length(1)])
                .split(inner);

            frame.render_stateful_widget(list, chunks[0], &mut self.list_state);

            let hint = Paragraph::new("w/Enter: add  d: remove")
                .style(theme::control_disabled());
            frame.render_widget(hint, chunks[1]);
        }
    }

    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        _session: &mut DebugSession,
    ) -> PanelAction {
        if self.insert_mode {
            return self.handle_insert_key(key, state);
        }

        match key.code {
            KeyCode::Char('j') => {
                if state.watches_selected + 1 < state.watches.len() {
                    state.watches_selected += 1;
                }
            }
            KeyCode::Char('k') => {
                if state.watches_selected > 0 {
                    state.watches_selected -= 1;
                }
            }
            KeyCode::Char('w') | KeyCode::Enter => {
                self.insert_mode = true;
                self.input_buffer.clear();
            }
            KeyCode::Char('d') => {
                Self::remove_selected(state);
            }
            _ => {}
        }
        PanelAction::None
    }
}
