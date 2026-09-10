use std::path::Path;

use anyhow::{Context, Result};
use serde::Deserialize;
use serde_json::json;

use crate::session::{DebugSession, SessionSnapshot, SessionState, SourceBreakpoint};

/// C-visible session handle wrapping the Rust debug session.
pub struct CSession {
    inner: DebugSession,
}

impl CSession {
    pub fn launch(program: &str) -> Result<Self> {
        let inner = DebugSession::launch_python(Path::new(program))
            .with_context(|| format!("failed to launch debug session for {program}"))?;
        Ok(Self { inner })
    }

    pub fn poll_json(&mut self) -> Result<Option<String>> {
        match self.inner.poll_events()? {
            Some(snapshot) => Ok(Some(
                serde_json::to_string(&json!({
                    "type": "stopped",
                    "snapshot": snapshot,
                }))
                .context("failed to serialize poll event")?,
            )),
            None => Ok(None),
        }
    }

    pub fn sync_snapshot_json(&mut self) -> Result<String> {
        let snapshot = self.inner.snapshot_for_sync()?;
        snapshot_to_json(&snapshot)
    }

    pub fn drain_console_json(&self) -> Result<String> {
        let entries: Vec<_> = self
            .inner
            .drain_console_output()
            .into_iter()
            .map(|output| {
                json!({
                    "category": output.category.unwrap_or_else(|| "stdout".into()),
                    "text": output.output,
                })
            })
            .collect();
        serde_json::to_string(&entries).context("failed to serialize console output")
    }

    pub fn command(&mut self, cmd_json: &str) -> Result<()> {
        let cmd: CommandRequest =
            serde_json::from_str(cmd_json).context("invalid command JSON")?;

        match cmd.op.as_str() {
            "continue" => self.inner.dispatch_continue(),
            "step_over" | "next" => self.inner.dispatch_step("next"),
            "step_into" | "step_in" => self.inner.dispatch_step("stepIn"),
            "step_out" => self.inner.dispatch_step("stepOut"),
            "step_back" => self.inner.step_back().map(|_| ()),
            "pause" => self.inner.pause().map(|_| ()),
            "play_pause" => match self.inner.state() {
                SessionState::Running => self.inner.pause().map(|_| ()),
                SessionState::Stopped { .. } => self.inner.dispatch_continue(),
                _ => Ok(()),
            },
            "terminate" => self.inner.terminate(),
            "disconnect" => self.inner.disconnect(),
            "restart" => self.inner.restart(),
            other => anyhow::bail!("unknown command op: {other}"),
        }
    }

    pub fn evaluate(&self, expression: &str, frame_id: i64, context: &str) -> Result<String> {
        self.inner.evaluate(expression, frame_id, context)
    }

    pub fn set_breakpoints(&self, path: &str, breakpoints: &[SourceBreakpoint]) -> Result<()> {
        self.inner
            .set_source_breakpoints(Path::new(path), breakpoints)
            .map(|_| ())
    }

    pub fn fetch_variables_json(&self, variables_reference: i64) -> Result<String> {
        let variables = self.inner.variables(variables_reference)?;
        serde_json::to_string(&variables).context("failed to serialize variables")
    }

    pub fn set_variable(&self, variables_reference: i64, name: &str, value: &str) -> Result<String> {
        let variable = self.inner.set_variable(variables_reference, name, value)?;
        serde_json::to_string(&variable).context("failed to serialize setVariable result")
    }

    pub fn fetch_source(&self, source_reference: i64) -> Result<String> {
        self.inner.fetch_source(source_reference)
    }
}

impl Drop for CSession {
    fn drop(&mut self) {
        let _ = self.inner.shutdown();
    }
}

fn snapshot_to_json(snapshot: &SessionSnapshot) -> Result<String> {
    serde_json::to_string(&json!({
        "type": "snapshot",
        "snapshot": snapshot,
    }))
    .context("failed to serialize snapshot")
}

#[derive(Debug, Deserialize)]
struct CommandRequest {
    op: String,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::session::SessionState;

    #[test]
    fn snapshot_json_envelope() {
        let snapshot = SessionSnapshot {
            state: SessionState::Stopped {
                thread_id: 1,
                reason: "entry".into(),
            },
            threads: vec![],
            stack_frames: vec![],
            thread_stacks: vec![],
            scopes: vec![],
            variables: vec![],
        };

        let json = snapshot_to_json(&snapshot).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed["type"], "snapshot");
        assert_eq!(parsed["snapshot"]["state"]["Stopped"]["thread_id"], 1);
        assert_eq!(parsed["snapshot"]["state"]["Stopped"]["reason"], "entry");
    }
}
