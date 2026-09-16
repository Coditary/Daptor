use std::path::{Path, PathBuf};
use std::process::Child;

use anyhow::{bail, Context, Result};
use tracing::info;

use crate::dap::protocol::{Scope, Source, StackFrame, Thread, Variable};
use crate::mi::{MiClient, MiStopReason};
use crate::rr::{
    ensure_gdb_available, ensure_rr_available, pick_replay_port, record_program,
    spawn_replay_server,
};
use crate::session::{
    AdapterCapabilities, SessionSnapshot, SessionState, SourceBreakpoint, ThreadStackTrace,
};

const LOCALS_SCOPE_REFERENCE: i64 = 1;

fn rr_capabilities() -> AdapterCapabilities {
    AdapterCapabilities { supports_step_back: true, ..AdapterCapabilities::default() }
}

pub struct RrDebugSession {
    mi: MiClient,
    replay_child: Child,
    program: PathBuf,
    state: SessionState,
    active_thread: i64,
    _replay_port: u16,
    initial_snapshot: Option<SessionSnapshot>,
    cached_variables: Vec<Variable>,
}

impl RrDebugSession {
    pub fn launch(program: impl AsRef<Path>) -> Result<Self> {
        Self::launch_with_args(program, &[])
    }

    pub fn launch_with_args(program: impl AsRef<Path>, args: &[String]) -> Result<Self> {
        ensure_rr_available()?;
        ensure_gdb_available()?;

        let program = program.as_ref().canonicalize().with_context(|| {
            format!("failed to resolve program path: {}", program.as_ref().display())
        })?;

        record_program(&program, args)?;

        let replay_port = pick_replay_port(50505)?;
        let replay_child = spawn_replay_server(replay_port)?;

        let mut mi = MiClient::spawn()?;
        mi.connect_remote("127.0.0.1", replay_port)?;
        mi.configure_for_rr_replay()?;
        mi.insert_breakpoint_function("main")?;
        let mut stop = mi.exec_run_to_initial_stop()?;
        for _ in 0..32 {
            if stop.reason == "breakpoint-hit" {
                break;
            }
            stop = mi.exec_continue()?;
        }

        let mut session = Self {
            mi,
            replay_child,
            program,
            state: SessionState::Stopped {
                thread_id: stop.thread_id,
                reason: map_stop_reason(&stop),
            },
            active_thread: stop.thread_id,
            _replay_port: replay_port,
            initial_snapshot: None,
            cached_variables: Vec::new(),
        };

        let snapshot = session.refresh_snapshot()?;
        session.initial_snapshot = Some(snapshot);
        info!("rr replay session ready on port {} for {}", replay_port, session.program.display());
        Ok(session)
    }

    pub fn state(&self) -> &SessionState {
        &self.state
    }

    pub fn take_initial_snapshot(&mut self) -> Option<SessionSnapshot> {
        self.initial_snapshot.take()
    }

    pub fn snapshot_for_sync(&mut self) -> Result<SessionSnapshot> {
        if let Some(snapshot) = self.take_initial_snapshot() {
            return Ok(snapshot);
        }

        match &self.state {
            SessionState::Exited | SessionState::Disconnected => {
                Ok(self.empty_state_snapshot(self.state.clone()))
            }
            _ => self.refresh_snapshot(),
        }
    }

    pub fn poll_events(&mut self) -> Result<Option<SessionSnapshot>> {
        Ok(None)
    }

    pub fn drain_console_output(&self) -> Vec<crate::dap::protocol::OutputEventBody> {
        Vec::new()
    }

    pub fn dispatch_continue(&mut self) -> Result<()> {
        let stop = self.mi.exec_continue()?;
        self.apply_stop(stop);
        Ok(())
    }

    pub fn dispatch_step(&mut self, kind: &str) -> Result<()> {
        let stop = match kind {
            "next" => self.mi.exec_next()?,
            "stepIn" => self.mi.exec_step()?,
            "stepOut" => self.mi.exec_finish()?,
            other => bail!("unsupported rr step command: {other}"),
        };
        self.apply_stop(stop);
        Ok(())
    }

    pub fn dispatch_step_in(&mut self, _target_id: Option<i64>) -> Result<()> {
        let stop = self.mi.exec_step()?;
        self.apply_stop(stop);
        Ok(())
    }

    pub fn dispatch_reverse_continue(&mut self) -> Result<()> {
        let stop = self.mi.exec_reverse_continue()?;
        self.apply_stop(stop);
        Ok(())
    }

    pub fn dispatch_step_back(&mut self) -> Result<()> {
        let stop = self.mi.exec_next_back()?;
        self.apply_stop(stop);
        Ok(())
    }

    pub fn dispatch_step_back_into(&mut self) -> Result<()> {
        let stop = self.mi.exec_step_back_into()?;
        self.apply_stop(stop);
        Ok(())
    }

    pub fn step_back(&mut self) -> Result<SessionSnapshot> {
        self.dispatch_step_back()?;
        self.refresh_snapshot()
    }

    pub fn step_back_into(&mut self) -> Result<SessionSnapshot> {
        self.dispatch_step_back_into()?;
        self.refresh_snapshot()
    }

    pub fn pause(&mut self) -> Result<SessionSnapshot> {
        bail!("pause is not supported during rr replay")
    }

    pub fn terminate(&mut self) -> Result<()> {
        self.state = SessionState::Exited;
        Ok(())
    }

    pub fn disconnect(&mut self) -> Result<()> {
        self.state = SessionState::Disconnected;
        Ok(())
    }

    pub fn restart(&mut self) -> Result<()> {
        let program = self.program.clone();
        let _ = self.shutdown();
        *self = Self::launch(program)?;
        Ok(())
    }

    pub fn set_source_breakpoints(
        &mut self,
        path: &Path,
        breakpoints: &[SourceBreakpoint],
    ) -> Result<()> {
        self.mi.clear_breakpoints()?;
        for breakpoint in breakpoints {
            self.mi.insert_breakpoint(&path.to_string_lossy(), breakpoint.line)?;
        }
        Ok(())
    }

    pub fn variables(&self, variables_reference: i64) -> Result<Vec<Variable>> {
        if variables_reference == LOCALS_SCOPE_REFERENCE {
            return Ok(self.cached_variables.clone());
        }
        Ok(Vec::new())
    }

    pub fn evaluate(&mut self, expression: &str, frame_id: i64, _context: &str) -> Result<String> {
        let frame_level = frame_id.max(0);
        self.mi
            .evaluate(expression, self.active_thread, frame_level)
            .map(|value| serde_json::to_string(&value).unwrap_or_else(|_| format!("\"{value}\"")))
    }

    pub fn set_variable(
        &self,
        _variables_reference: i64,
        _name: &str,
        _value: &str,
    ) -> Result<Variable> {
        bail!("set variable is not supported in rr replay mode")
    }

    pub fn fetch_source(&self, _source_reference: i64) -> Result<String> {
        bail!("adapter source fetch is not supported in rr replay mode")
    }

    pub fn step_in_targets(
        &self,
        _frame_id: i64,
    ) -> Result<Vec<crate::dap::protocol::StepInTarget>> {
        Ok(Vec::new())
    }

    pub fn goto_targets(
        &self,
        _path: &str,
        _line: i64,
        _column: i64,
        _source_reference: i64,
    ) -> Result<Vec<crate::dap::protocol::GotoTarget>> {
        Ok(Vec::new())
    }

    pub fn shutdown(&mut self) -> Result<()> {
        let _ = self.mi.shutdown();
        let _ = self.replay_child.kill();
        let _ = self.replay_child.wait();
        Ok(())
    }

    fn apply_stop(&mut self, stop: MiStopReason) {
        self.active_thread = stop.thread_id;
        self.state =
            SessionState::Stopped { thread_id: stop.thread_id, reason: map_stop_reason(&stop) };
    }

    fn refresh_snapshot(&mut self) -> Result<SessionSnapshot> {
        let thread_id = self.active_thread;
        let frames = self.mi.stack_frames()?;
        let stack_frames = frames
            .iter()
            .map(|frame| StackFrame {
                id: frame.level,
                name: frame.name.clone(),
                line: frame.line,
                column: 0,
                source: if frame.path.is_empty() {
                    None
                } else {
                    Some(Source {
                        name: Path::new(&frame.path)
                            .file_name()
                            .and_then(|name| name.to_str())
                            .map(str::to_string),
                        path: Some(frame.path.clone()),
                        source_reference: None,
                    })
                },
                instruction_pointer_reference: None,
            })
            .collect::<Vec<_>>();

        self.cached_variables = if let Some(top) = frames.first() {
            self.mi
                .stack_locals(thread_id, top.level)?
                .into_iter()
                .map(|var| Variable {
                    name: var.name,
                    value: var.value,
                    type_name: var.type_name,
                    variables_reference: 0,
                })
                .collect()
        } else {
            Vec::new()
        };

        let scopes = vec![Scope {
            name: "Locals".into(),
            variables_reference: LOCALS_SCOPE_REFERENCE,
            expensive: false,
        }];

        let threads = vec![Thread {
            id: thread_id,
            name: self
                .program
                .file_name()
                .and_then(|name| name.to_str())
                .unwrap_or("main")
                .to_string(),
        }];

        let thread_stacks =
            vec![ThreadStackTrace { thread_id, stack_frames: stack_frames.clone() }];

        Ok(SessionSnapshot {
            state: self.state.clone(),
            threads,
            stack_frames,
            thread_stacks,
            scopes,
            variables: self.cached_variables.clone(),
            capabilities: rr_capabilities(),
            breakpoint_hits: vec![],
            exception_info: None,
            debug_process_ids: vec![],
        })
    }

    fn empty_state_snapshot(&self, state: SessionState) -> SessionSnapshot {
        SessionSnapshot {
            state,
            threads: vec![],
            stack_frames: vec![],
            thread_stacks: vec![],
            scopes: vec![],
            variables: vec![],
            breakpoint_hits: vec![],
            exception_info: None,
            capabilities: rr_capabilities(),
            debug_process_ids: vec![],
        }
    }
}

impl Drop for RrDebugSession {
    fn drop(&mut self) {
        let _ = self.shutdown();
    }
}

fn map_stop_reason(stop: &MiStopReason) -> String {
    match stop.reason.as_str() {
        "breakpoint-hit" => "breakpoint".into(),
        "end-stepping-range" => "step".into(),
        "function-finished" => "step".into(),
        "signal-received" => "entry".into(),
        other => other.replace('-', "_"),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::rr::rr_record_available;
    use crate::test_support::{ensure_native_binary, integration_test_lock};

    #[test]
    #[ignore = "integration: requires rr and gdb"]
    fn launch_rr_reverse_demo() {
        let _guard = integration_test_lock();
        if !rr_record_available() {
            eprintln!("SKIP: rr record unavailable (missing tools or perf_event_paranoid > 1)");
            return;
        }

        let program = ensure_native_binary("reverse_demo");

        let mut session = RrDebugSession::launch(&program).expect("rr launch");
        let snap = session.snapshot_for_sync().expect("snapshot");
        assert!(matches!(snap.state, SessionState::Stopped { .. }));
        assert!(snap.capabilities.supports_step_back);
        assert!(!snap.stack_frames.is_empty());
        assert!(
            !snap.variables.is_empty(),
            "expected locals in snapshot, got scopes={:?} vars={:?}",
            snap.scopes,
            snap.variables
        );
        session.shutdown().expect("shutdown");
    }

    #[test]
    #[ignore = "integration: requires rr and gdb"]
    fn rr_locals_populated_after_step() {
        let _guard = integration_test_lock();
        if !rr_record_available() {
            eprintln!("SKIP: rr record unavailable (missing tools or perf_event_paranoid > 1)");
            return;
        }

        let program = ensure_native_binary("reverse_demo");

        let mut session = RrDebugSession::launch(&program).expect("rr launch");
        session.dispatch_step("next").expect("step");
        let snap = session.snapshot_for_sync().expect("snapshot");
        assert!(
            !snap.variables.is_empty(),
            "expected locals after step, frames={:?} vars={:?}",
            snap.stack_frames.first().map(|f| (&f.name, f.line)),
            snap.variables
        );
        session.shutdown().expect("shutdown");
    }

    #[test]
    #[ignore = "integration: requires rr and gdb"]
    fn rr_step_back_reverses_line() {
        let _guard = integration_test_lock();
        if !rr_record_available() {
            eprintln!("SKIP: rr record unavailable (missing tools or perf_event_paranoid > 1)");
            return;
        }

        let program = ensure_native_binary("reverse_demo");

        let mut session = RrDebugSession::launch(&program).expect("rr launch");

        let before = session.snapshot_for_sync().expect("snapshot before");
        let line_before = before.stack_frames.first().map(|f| f.line).unwrap_or(0);
        assert!(line_before > 0, "expected to stop in user code, got {line_before:?}");

        session.dispatch_step("next").expect("step over");
        let after_step = session.snapshot_for_sync().expect("snapshot after step");
        let line_after = after_step.stack_frames.first().map(|f| f.line).unwrap_or(0);
        assert_ne!(line_before, line_after, "step over should change line");

        session.step_back().expect("step back");
        let after_back = session.snapshot_for_sync().expect("snapshot after back");
        let line_back = after_back.stack_frames.first().map(|f| f.line).unwrap_or(0);
        assert_eq!(
            line_before, line_back,
            "step back should restore previous line (before={line_before}, after_step={line_after}, back={line_back})"
        );

        session.shutdown().expect("shutdown");
    }
}
