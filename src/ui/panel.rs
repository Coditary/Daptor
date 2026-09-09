use std::path::Path;

use ratatui::crossterm::event::KeyEvent;
use ratatui::layout::Rect;
use ratatui::Frame;

use crate::session::DebugSession;
use crate::ui::state::AppState;

/// Result of handling a key in a panel.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PanelAction {
    None,
    Refresh,
    Continue,
    StepOver,
    StepInto,
    StepOut,
    StepBack,
    PlayPause,
    Pause,
    Restart,
    Terminate,
    Disconnect,
    Quit,
    ToggleUi,
    CloseFloat,
    OpenEval,
    EvaluateRepl,
    SyncBreakpoints,
}

/// One UI element matching nvim-dap-ui element IDs.
pub trait Panel {
    fn id(&self) -> &'static str;
    fn title(&self) -> &'static str;
    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState);
    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        session: &mut DebugSession,
    ) -> PanelAction;
}

/// Load source file content into state from disk.
pub fn load_source(state: &mut AppState, path: &Path) {
    if let Ok(content) = std::fs::read_to_string(path) {
        state.source_path = Some(path.to_path_buf());
        state.source_lines = content.lines().map(str::to_string).collect();
        state.source_cursor_line = 1;
        state.source_scroll = 0;
    }
}

/// Jump cursor to a line and keep it visible (call after setting scroll area height).
pub fn set_source_cursor(state: &mut AppState, line: u32, visible_height: usize) {
    let max_line = state.source_lines.len().max(1) as u32;
    state.source_cursor_line = line.clamp(1, max_line);
    ensure_cursor_visible(state, visible_height);
}

pub fn ensure_cursor_visible(state: &mut AppState, visible_height: usize) {
    if state.source_lines.is_empty() {
        return;
    }
    let cursor = state.source_cursor_line.saturating_sub(1) as usize;
    let scroll = state.source_scroll as usize;
    let view = visible_height.max(1);

    if cursor < scroll {
        state.source_scroll = cursor as u16;
    } else if cursor >= scroll + view {
        state.source_scroll = (cursor + 1).saturating_sub(view) as u16;
    }

    let max_scroll = state.source_lines.len().saturating_sub(1);
    state.source_scroll = state.source_scroll.min(max_scroll as u16);
}
