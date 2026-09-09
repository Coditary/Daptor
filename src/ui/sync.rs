//! Sync session events into UI state (auto open/close hooks).

use anyhow::Result;

use crate::dap::protocol::OutputEventBody;
use crate::session::{DebugSession, SessionSnapshot, SessionState};
use crate::ui::panel::load_source;
use crate::ui::state::AppState;

/// Pull latest data from the DAP session into UI state.
pub fn sync_from_session(state: &mut AppState, session: &mut DebugSession) -> Result<()> {
    let snapshot = session.refresh_snapshot()?;
    apply_snapshot(state, &snapshot, session);
    Ok(())
}

/// Re-evaluate all watch expressions against the current stack frame.
pub fn refresh_watches(state: &mut AppState, session: &DebugSession) {
    if !matches!(state.session_state, SessionState::Stopped { .. }) {
        return;
    }

    let frame_id = match state.stack_frames.first() {
        Some(frame) => frame.id,
        None => return,
    };

    for watch in &mut state.watches {
        match session.evaluate(&watch.expression, frame_id, "watch") {
            Ok(result) => {
                watch.value = result;
                watch.error = None;
            }
            Err(err) => {
                watch.value.clear();
                watch.error = Some(err.to_string());
            }
        }
    }
}

pub fn apply_snapshot(state: &mut AppState, snapshot: &SessionSnapshot, session: &mut DebugSession) {
    state.session_state = snapshot.state.clone();
    state.threads = snapshot.threads.clone();
    state.stack_frames = snapshot.stack_frames.clone();
    state.scopes = snapshot.scopes.clone();

    if let Some(frame) = snapshot.stack_frames.first() {
        state.current_line = frame.line.max(1) as u32;
        if let Some(path) = frame
            .source
            .as_ref()
            .and_then(|s| s.path.as_ref())
            .map(std::path::PathBuf::from)
        {
            state.execution_path = Some(path.clone());
            load_source(state, &path);
            state.source_cursor_line = state.current_line;
        }
    }

    for scope in &snapshot.scopes {
        state
            .variables
            .entry(scope.variables_reference)
            .or_insert_with(|| snapshot.variables.clone());
    }

    // Auto-open on stopped (mirrors dap.listeners.before.launch → dapui.open)
    if state.ui_config.auto_open {
        match &snapshot.state {
            SessionState::Stopped { .. } => state.ui_open = true,
            SessionState::Running => state.ui_open = true,
            _ => {}
        }
    }

    // Auto-close on exit (mirrors event_terminated / event_exited → dapui.close)
    if state.ui_config.auto_close && matches!(snapshot.state, SessionState::Exited) {
        state.close_ui();
    }

    state.status_message = match &snapshot.state {
        SessionState::Stopped { reason, .. } => format!("Stopped ({reason})"),
        SessionState::Running => "Running".into(),
        SessionState::Exited => "Session ended".into(),
        SessionState::Disconnected => "Disconnected".into(),
    };

    if matches!(snapshot.state, SessionState::Stopped { .. }) {
        for scope in &snapshot.scopes {
            let ref_id = scope.variables_reference;
            if !state.variables.contains_key(&ref_id) {
                if let Ok(vars) = session.variables(ref_id) {
                    state.variables.insert(ref_id, vars);
                    state.expanded_refs.insert(ref_id);
                }
            }
        }
        refresh_watches(state, session);
    }
}

pub fn push_output(state: &mut AppState, output: &OutputEventBody) {
    state.console_lines.push(crate::ui::state::ConsoleLine {
        category: output.category.clone().unwrap_or_else(|| "stdout".into()),
        text: output.output.clone(),
    });
}

/// Returns true when the UI should exit (auto-close after session ended).
pub fn should_auto_exit(state: &AppState) -> bool {
    state.ui_config.auto_close
        && matches!(state.session_state, SessionState::Exited)
        && !state.ui_open
}
