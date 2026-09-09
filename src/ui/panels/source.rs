//! Center-panel source viewer with syntax highlighting and nvim-dap-ui gutter.

use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::Rect;
use ratatui::style::Style;
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, List, ListItem, Paragraph};
use ratatui::Frame;

use crate::session::DebugSession;
use crate::ui::chrome::panel_block;
use crate::ui::icons::{BREAKPOINT, CURRENT_FRAME};
use crate::ui::layout::is_focused;
use crate::ui::panel::{load_source, set_source_cursor, Panel, PanelAction};
use crate::ui::state::{AppState, BreakpointEntry, Focus, SourceMode};
use crate::ui::theme::{self, bg};
use crate::ui::workspace::{filter_files, relative_display};

#[derive(Debug, Default)]
pub struct SourcePanel {
    last_visible_height: usize,
}

impl SourcePanel {
    fn block_title(state: &AppState) -> String {
        match &state.source_path {
            Some(path) => path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| path.display().to_string()),
            None => "no file".into(),
        }
    }

    fn has_breakpoint(state: &AppState, path: &std::path::Path, line: u32) -> bool {
        state
            .breakpoints
            .iter()
            .any(|bp| bp.path == path && bp.line == line)
    }

    fn toggle_breakpoint_at_cursor(state: &mut AppState) -> PanelAction {
        let Some(path) = state.source_path.clone() else {
            state.status_message = "No file open".into();
            return PanelAction::None;
        };
        let line = state.source_cursor_line;
        if let Some(idx) = state
            .breakpoints
            .iter()
            .position(|bp| bp.path == path && bp.line == line)
        {
            state.breakpoints.remove(idx);
            state.status_message = format!("Removed breakpoint at {}:{line}", path.display());
        } else {
            let line_idx = line.saturating_sub(1) as usize;
            let is_empty_line = state
                .source_lines
                .get(line_idx)
                .map(|text| text.trim().is_empty())
                .unwrap_or(true);
            if is_empty_line {
                state.status_message = "Cannot set breakpoint on an empty line".into();
                return PanelAction::None;
            }

            let id = state.breakpoints.len() as i64;
            state.breakpoints.push(BreakpointEntry {
                id,
                path: path.clone(),
                line,
                enabled: true,
                verified: false,
            });
            state.status_message = format!("Breakpoint at {}:{line}", path.display());
        }
        PanelAction::SyncBreakpoints
    }

    pub fn toggle_breakpoint_at_line(state: &mut AppState, line: u32) -> PanelAction {
        state.source_cursor_line = line;
        Self::toggle_breakpoint_at_cursor(state)
    }

    fn move_cursor(&self, state: &mut AppState, delta: i32) {
        if state.source_lines.is_empty() {
            return;
        }
        let max = state.source_lines.len() as u32;
        let next = (state.source_cursor_line as i32 + delta).clamp(1, max as i32) as u32;
        set_source_cursor(state, next, self.last_visible_height);
    }

    fn page_scroll(&self, state: &mut AppState, direction: i32) {
        let page = self.last_visible_height.max(1) as i32;
        self.move_cursor(state, direction * page);
    }

    fn render_file_picker(&self, frame: &mut Frame, area: Rect, state: &AppState) {
        let block = Block::default()
            .title(" Open File ")
            .borders(Borders::ALL)
            .border_style(theme::border_focused())
            .style(Style::default().bg(bg::PANEL));
        let inner = block.inner(area);
        frame.render_widget(block, area);

        let filtered =
            filter_files(&state.workspace_files, &state.workspace_root, &state.file_picker_filter);

        let filter_line = Line::from(vec![
            Span::styled("> ", Style::default().fg(theme::variable_type().fg.unwrap_or_default())),
            Span::raw(state.file_picker_filter.as_str()),
        ]);

        let list_height = inner.height.saturating_sub(2) as usize;
        let scroll = state
            .file_picker_selected
            .saturating_sub(list_height.saturating_sub(1));

        let items: Vec<ListItem> = filtered
            .iter()
            .enumerate()
            .skip(scroll)
            .take(list_height)
            .map(|(i, path)| {
                let label = relative_display(&state.workspace_root, path);
                let style = if i == state.file_picker_selected {
                    theme::selection()
                } else {
                    theme::source_text()
                };
                ListItem::new(label).style(style)
            })
            .collect();

        let filter_area = Rect::new(inner.x, inner.y, inner.width, 1);
        let list_area = Rect::new(
            inner.x,
            inner.y + 1,
            inner.width,
            inner.height.saturating_sub(1),
        );

        frame.render_widget(Paragraph::new(filter_line), filter_area);
        if filtered.is_empty() {
            frame.render_widget(
                Paragraph::new("No matching files").style(Style::default().fg(theme::control_disabled().fg.unwrap_or_default())),
                list_area,
            );
        } else {
            frame.render_widget(List::new(items), list_area);
        }
    }

    fn render_source_view(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        let focused = is_focused(state, Focus::Source);
        let block = panel_block(&Self::block_title(state), focused);
        let inner = block.inner(area);
        frame.render_widget(block, area);

        self.last_visible_height = inner.height as usize;

        if state.source_lines.is_empty() {
            frame.render_widget(
                Paragraph::new("No source loaded — press p to open a file")
                    .style(Style::default().fg(theme::control_disabled().fg.unwrap_or_default())),
                inner,
            );
            return;
        }

        let total_lines = state.source_lines.len();
        let visible_height = inner.height as usize;
        let scroll = (state.source_scroll as usize).min(total_lines.saturating_sub(1));
        let gutter_width = total_lines.to_string().len().max(3);

        let exec_path = state.execution_path.as_deref();
        let view_path = state.source_path.as_deref();

        let mut lines = Vec::with_capacity(visible_height);
        for (offset, text) in state
            .source_lines
            .iter()
            .enumerate()
            .skip(scroll)
            .take(visible_height)
        {
            let line_num = (offset + 1) as u32;
            let is_cursor = line_num == state.source_cursor_line;
            let is_execution = exec_path == view_path
                && line_num == state.current_line
                && matches!(state.session_state, crate::session::SessionState::Stopped { .. });
            let has_bp = view_path
                .map(|p| Self::has_breakpoint(state, p, line_num))
                .unwrap_or(false);

            let row_style = if is_execution {
                theme::execution_line()
            } else if is_cursor {
                theme::cursor_line()
            } else {
                theme::source_text()
            };

            let bp_span = Span::styled(
                if has_bp { format!("{BREAKPOINT} ") } else { "  ".into() },
                if has_bp { theme::breakpoint_marker() } else { theme::line_number() },
            );
            let arrow_span = Span::styled(
                if is_execution {
                    format!("{CURRENT_FRAME} ")
                } else if is_cursor {
                    "▸ ".to_string()
                } else {
                    "  ".to_string()
                },
                row_style,
            );
            let num_span = Span::styled(
                format!("{line_num:>gutter_width$} │ "),
                theme::line_number(),
            );

            let mut code_spans = theme::highlight_python(text)
                .into_iter()
                .map(|(part, mut style)| {
                    if row_style.bg.is_some() {
                        style.bg = row_style.bg;
                    }
                    Span::styled(part, style)
                })
                .collect::<Vec<_>>();

            let mut gutter = vec![bp_span, arrow_span, num_span];
            gutter.append(&mut code_spans);
            lines.push(Line::from(gutter));
        }

        frame.render_widget(Paragraph::new(lines).style(Style::default().bg(bg::PANEL)), inner);
    }

    fn handle_file_picker_key(&mut self, key: KeyEvent, state: &mut AppState) -> PanelAction {
        let filtered =
            filter_files(&state.workspace_files, &state.workspace_root, &state.file_picker_filter);

        match key.code {
            KeyCode::Esc => state.close_file_picker(),
            KeyCode::Enter => {
                if let Some(path) = filtered.get(state.file_picker_selected) {
                    load_source(state, path);
                    state.close_file_picker();
                    state.status_message = format!("Opened {}", path.display());
                }
            }
            KeyCode::Char('j') | KeyCode::Down if !filtered.is_empty() => {
                state.file_picker_selected = (state.file_picker_selected + 1).min(filtered.len() - 1);
            }
            KeyCode::Char('k') | KeyCode::Up => {
                state.file_picker_selected = state.file_picker_selected.saturating_sub(1);
            }
            KeyCode::Backspace => {
                state.file_picker_filter.pop();
                state.file_picker_selected = 0;
            }
            KeyCode::Char(c) if !key.modifiers.contains(KeyModifiers::CONTROL) => {
                state.file_picker_filter.push(c);
                state.file_picker_selected = 0;
            }
            _ => {}
        }
        PanelAction::None
    }
}

impl Panel for SourcePanel {
    fn id(&self) -> &'static str {
        "source"
    }

    fn title(&self) -> &'static str {
        "Source"
    }

    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        match state.source_mode {
            SourceMode::FilePicker => self.render_file_picker(frame, area, state),
            SourceMode::View => self.render_source_view(frame, area, state),
        }
    }

    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        _session: &mut DebugSession,
    ) -> PanelAction {
        if state.source_mode == SourceMode::FilePicker {
            return self.handle_file_picker_key(key, state);
        }

        match key.code {
            KeyCode::Char('p') => {
                state.open_file_picker();
                PanelAction::None
            }
            KeyCode::Char('j') | KeyCode::Down => {
                self.move_cursor(state, 1);
                PanelAction::None
            }
            KeyCode::Char('k') | KeyCode::Up => {
                self.move_cursor(state, -1);
                PanelAction::None
            }
            KeyCode::Char('g') => {
                set_source_cursor(state, 1, self.last_visible_height);
                PanelAction::None
            }
            KeyCode::Char('G') => {
                let last = state.source_lines.len().max(1) as u32;
                set_source_cursor(state, last, self.last_visible_height);
                PanelAction::None
            }
            KeyCode::PageDown => {
                self.page_scroll(state, 1);
                PanelAction::None
            }
            KeyCode::PageUp => {
                self.page_scroll(state, -1);
                PanelAction::None
            }
            KeyCode::Char('b') | KeyCode::Char(' ') => Self::toggle_breakpoint_at_cursor(state),
            KeyCode::F(5) | KeyCode::Char('c') => PanelAction::Continue,
            KeyCode::F(10) | KeyCode::Char('n') => PanelAction::StepOver,
            KeyCode::F(11) | KeyCode::Char('i') => PanelAction::StepInto,
            KeyCode::F(12) | KeyCode::Char('u') => PanelAction::StepOut,
            _ => PanelAction::None,
        }
    }
}
