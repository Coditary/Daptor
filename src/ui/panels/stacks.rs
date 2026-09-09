//! Threads and stack frames panel (nvim-dap-ui stacks element).

use ratatui::crossterm::event::{KeyCode, KeyEvent};
use ratatui::layout::Rect;
use ratatui::text::{Line, Span};
use ratatui::widgets::{List, ListItem, Paragraph};
use ratatui::Frame;

use crate::dap::protocol::StackFrame;
use crate::session::{DebugSession, SessionState};
use crate::ui::chrome::panel_block;
use crate::ui::icons::CURRENT_FRAME;
use crate::ui::layout::is_focused;
use crate::ui::panel::{load_source, set_source_cursor, Panel, PanelAction};
use crate::ui::state::{AppState, Focus};
use crate::ui::theme;

#[derive(Debug, Default)]
pub struct StacksPanel {
    show_subtle: bool,
}

impl StacksPanel {
    fn stopped_thread_id(state: &AppState) -> Option<i64> {
        match &state.session_state {
            SessionState::Stopped { thread_id, .. } => Some(*thread_id),
            _ => None,
        }
    }

    fn is_subtle_frame(name: &str) -> bool {
        (name.starts_with('<') && name.ends_with('>')) || name == "<lambda>"
    }

    fn filtered_frames<'a>(
        frames: &'a [StackFrame],
        show_subtle: bool,
    ) -> Vec<&'a StackFrame> {
        if show_subtle {
            frames.iter().collect()
        } else {
            frames
                .iter()
                .filter(|frame| !Self::is_subtle_frame(&frame.name))
                .collect()
        }
    }

    fn frame_source_path(frame: &StackFrame) -> &str {
        frame
            .source
            .as_ref()
            .and_then(|source| source.path.as_deref())
            .or(frame
                .source
                .as_ref()
                .and_then(|source| source.name.as_deref()))
            .unwrap_or("<unknown>")
    }

    fn format_frame_line(
        frame: &StackFrame,
        is_current: bool,
        selected: bool,
        focused: bool,
    ) -> Line<'static> {
        let prefix = if is_current {
            format!("{CURRENT_FRAME} ")
        } else {
            "  ".into()
        };
        let path = Self::frame_source_path(frame);
        let text = format!(
            "{prefix}#{} {} @ {}:{}",
            frame.id, frame.name, path, frame.line
        );

        let style = if selected && focused {
            theme::selection()
        } else if is_current {
            theme::frame_current()
        } else {
            theme::frame_normal()
        };

        Line::from(Span::styled(text, style))
    }

    fn build_lines(state: &AppState, show_subtle: bool) -> (Vec<Line<'static>>, usize, usize) {
        let focused = is_focused(state, Focus::Stacks);
        let stopped_id = Self::stopped_thread_id(state);
        let visible_frames = Self::filtered_frames(&state.stack_frames, show_subtle);
        let frame_count = visible_frames.len();

        let mut selected = state.stacks_selected;
        if frame_count == 0 {
            selected = 0;
        } else if selected >= frame_count {
            selected = frame_count - 1;
        }

        let mut lines = Vec::new();
        let mut selected_line_idx = 0;

        if state.threads.is_empty() {
            lines.push(Line::from(Span::styled(
                "No threads",
                theme::control_disabled(),
            )));
            return (lines, 0, 0);
        }

        let mut stopped_threads = Vec::new();
        let mut other_threads = Vec::new();
        for thread in &state.threads {
            if stopped_id == Some(thread.id) {
                stopped_threads.push(thread);
            } else {
                other_threads.push(thread);
            }
        }

        for thread in stopped_threads.iter().chain(other_threads.iter()) {
            let is_stopped = stopped_id == Some(thread.id);
            let header_style = if is_stopped {
                theme::thread_header_stopped()
            } else {
                theme::thread_header()
            };
            lines.push(Line::from(Span::styled(
                format!("[{}] {}", thread.id, thread.name),
                header_style,
            )));

            if is_stopped {
                for (index, frame) in visible_frames.iter().enumerate() {
                    let is_current = index == 0;
                    let selected_row = index == selected;
                    lines.push(Self::format_frame_line(
                        frame,
                        is_current,
                        selected_row,
                        focused,
                    ));
                    if selected_row {
                        selected_line_idx = lines.len() - 1;
                    }
                }
            }
        }

        (lines, frame_count, selected_line_idx)
    }

    fn open_frame(state: &mut AppState, frame: &StackFrame) {
        if let Some(path) = frame
            .source
            .as_ref()
            .and_then(|source| source.path.as_deref())
            .map(std::path::Path::new)
        {
            load_source(state, path);
            set_source_cursor(state, frame.line.max(1) as u32, 20);
        } else {
            state.current_line = frame.line.max(1) as u32;
        }
    }
}

impl Panel for StacksPanel {
    fn id(&self) -> &'static str {
        "stacks"
    }

    fn title(&self) -> &'static str {
        "Stacks"
    }

    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        let (lines, _frame_count, selected_line_idx) =
            Self::build_lines(state, self.show_subtle);

        let focused = is_focused(state, Focus::Stacks);
        let block = panel_block("Stacks", focused);

        if lines.is_empty() {
            frame.render_widget(
                Paragraph::new("No stack frames").block(block),
                area,
            );
            return;
        }

        let inner_height = area.height.saturating_sub(2) as usize;
        let scroll_offset = if inner_height > 0 && selected_line_idx >= inner_height {
            selected_line_idx - inner_height + 1
        } else {
            0
        };

        let visible_lines: Vec<Line<'_>> = lines
            .iter()
            .skip(scroll_offset)
            .take(inner_height)
            .cloned()
            .collect();

        let items: Vec<ListItem<'_>> = visible_lines
            .into_iter()
            .map(ListItem::new)
            .collect();

        frame.render_widget(List::new(items).block(block), area);
    }

    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        _session: &mut DebugSession,
    ) -> PanelAction {
        let visible_frames: Vec<&StackFrame> =
            Self::filtered_frames(&state.stack_frames, self.show_subtle);
        let frame_count = visible_frames.len();

        if frame_count == 0 {
            if matches!(key.code, KeyCode::Char('t')) {
                self.show_subtle = !self.show_subtle;
            }
            return PanelAction::None;
        }

        if state.stacks_selected >= frame_count {
            state.stacks_selected = frame_count - 1;
        }

        match key.code {
            KeyCode::Char('j') | KeyCode::Down => {
                if state.stacks_selected + 1 < frame_count {
                    state.stacks_selected += 1;
                }
            }
            KeyCode::Char('k') | KeyCode::Up => {
                if state.stacks_selected > 0 {
                    state.stacks_selected -= 1;
                }
            }
            KeyCode::Char('t') => {
                self.show_subtle = !self.show_subtle;
                let new_count =
                    Self::filtered_frames(&state.stack_frames, self.show_subtle).len();
                if new_count == 0 {
                    state.stacks_selected = 0;
                } else if state.stacks_selected >= new_count {
                    state.stacks_selected = new_count - 1;
                }
            }
            KeyCode::Enter | KeyCode::Char('o') => {
                if let Some(frame) = visible_frames
                    .get(state.stacks_selected)
                    .map(|f| (*f).clone())
                {
                    Self::open_frame(state, &frame);
                }
            }
            _ => {}
        }

        PanelAction::None
    }
}
