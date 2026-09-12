use std::path::Path;

use anyhow::{Context, Result};
use serde::Deserialize;
use serde_json::json;

use crate::session::{
    DataBreakpoint, DebugSession, ExceptionBreakpointSetting, FunctionBreakpoint, RrDebugSession,
    SessionSnapshot, SessionState, SourceBreakpoint,
};

enum SessionEngine {
    Dap(DebugSession),
    Rr(RrDebugSession),
}

/// C-visible session handle wrapping the Rust debug session.
pub struct CSession {
    inner: SessionEngine,
}

impl CSession {
    pub fn launch(program: &str, args: &[String]) -> Result<Self> {
        let inner = DebugSession::launch_python_with_args(Path::new(program), args)
            .with_context(|| format!("failed to launch debug session for {program}"))?;
        Ok(Self {
            inner: SessionEngine::Dap(inner),
        })
    }

    pub fn launch_lldb(program: &str, args: &[String]) -> Result<Self> {
        let inner = DebugSession::launch_native_with_args(Path::new(program), args)
            .with_context(|| format!("failed to launch lldb debug session for {program}"))?;
        Ok(Self {
            inner: SessionEngine::Dap(inner),
        })
    }

    pub fn launch_rr(program: &str, args: &[String]) -> Result<Self> {
        let inner = RrDebugSession::launch_with_args(Path::new(program), args)
            .with_context(|| format!("failed to launch rr debug session for {program}"))?;
        Ok(Self {
            inner: SessionEngine::Rr(inner),
        })
    }

    pub fn poll_json(&mut self) -> Result<Option<String>> {
        let snapshot = match &mut self.inner {
            SessionEngine::Dap(session) => session.poll_events()?,
            SessionEngine::Rr(session) => session.poll_events()?,
        };
        match snapshot {
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
        let snapshot = match &mut self.inner {
            SessionEngine::Dap(session) => session.snapshot_for_sync()?,
            SessionEngine::Rr(session) => session.snapshot_for_sync()?,
        };
        snapshot_to_json(&snapshot)
    }

    pub fn drain_console_json(&mut self) -> Result<String> {
        let entries: Vec<_> = match &mut self.inner {
            SessionEngine::Dap(session) => session
                .drain_console_output()
                .into_iter()
                .map(|output| {
                    json!({
                        "category": output.category.unwrap_or_else(|| "stdout".into()),
                        "text": output.output,
                    })
                })
                .collect(),
            SessionEngine::Rr(_) => Vec::new(),
        };
        serde_json::to_string(&entries).context("failed to serialize console output")
    }

    pub fn terminal_write(&mut self, bytes: &[u8]) -> Result<()> {
        match &mut self.inner {
            SessionEngine::Dap(session) => session.terminal_write_input(bytes),
            SessionEngine::Rr(_) => Ok(()),
        }
    }

    pub fn command(&mut self, cmd_json: &str) -> Result<()> {
        let cmd: CommandRequest =
            serde_json::from_str(cmd_json).context("invalid command JSON")?;

        match &mut self.inner {
            SessionEngine::Dap(session) => match cmd.op.as_str() {
                "continue" => session.dispatch_continue(),
                "step_over" | "next" => session.dispatch_step("next"),
                "step_into" | "step_in" => session.dispatch_step_in(cmd.target_id),
                "step_out" => session.dispatch_step("stepOut"),
                "step_back" => session.step_back().map(|_| ()),
                "step_back_into" => session.step_back().map(|_| ()),
                "reverse_continue" => session.dispatch_reverse_continue(),
                "pause" => session.pause().map(|_| ()),
                "play_pause" => match session.state() {
                    SessionState::Running => session.pause().map(|_| ()),
                    SessionState::Stopped { .. } => session.dispatch_continue(),
                    _ => Ok(()),
                },
                "terminate" => session.terminate(),
                "disconnect" => session.disconnect(),
                "restart" => session.restart(),
                "goto" => {
                    let target_id = cmd
                        .target_id
                        .context("goto requires target_id")?;
                    session.dispatch_goto(target_id)
                }
                "goto_line" => {
                    let line = cmd.line.context("goto_line requires line")?;
                    let path = cmd.path.unwrap_or_default();
                    session.dispatch_goto_line(&path, line)
                }
                other => anyhow::bail!("unknown command op: {other}"),
            },
            SessionEngine::Rr(session) => match cmd.op.as_str() {
                "continue" => session.dispatch_continue(),
                "step_over" | "next" => session.dispatch_step("next"),
                "step_into" | "step_in" => session.dispatch_step_in(cmd.target_id),
                "step_out" => session.dispatch_step("stepOut"),
                "step_back" => session.step_back().map(|_| ()),
                "step_back_into" => session.step_back_into().map(|_| ()),
                "reverse_continue" => session.dispatch_reverse_continue(),
                "pause" => session.pause().map(|_| ()),
                "play_pause" => match session.state() {
                    SessionState::Running => session.pause().map(|_| ()),
                    SessionState::Stopped { .. } => session.dispatch_continue(),
                    _ => Ok(()),
                },
                "terminate" => session.terminate(),
                "disconnect" => session.disconnect(),
                "restart" => session.restart(),
                other => anyhow::bail!("unknown command op: {other}"),
            },
        }
    }

    pub fn evaluate(&mut self, expression: &str, frame_id: i64, context: &str) -> Result<String> {
        match &mut self.inner {
            SessionEngine::Dap(session) => session.evaluate(expression, frame_id, context),
            SessionEngine::Rr(session) => session.evaluate(expression, frame_id, context),
        }
    }

    pub fn completions(&mut self, text: &str, column: i64, frame_id: i64) -> Result<String> {
        match &mut self.inner {
            SessionEngine::Dap(session) => {
                let items = session.completions(text, column, frame_id)?;
                serde_json::to_string(&items).context("failed to serialize completions")
            }
            SessionEngine::Rr(_) => Ok("[]".to_string()),
        }
    }

    pub fn set_breakpoints(&mut self, path: &str, breakpoints: &[SourceBreakpoint]) -> Result<String> {
        match &mut self.inner {
            SessionEngine::Dap(session) => {
                let results = session.set_source_breakpoints(Path::new(path), breakpoints)?;
                let json = results
                    .iter()
                    .map(|breakpoint| {
                        let mut entry = json!({
                            "line": breakpoint.line,
                            "verified": breakpoint.verified,
                        });
                        entry["hitCount"] = json!(breakpoint.hit_count.unwrap_or(0));
                        entry
                    })
                    .collect::<Vec<_>>();
                serde_json::to_string(&json).context("failed to serialize breakpoint results")
            }
            SessionEngine::Rr(session) => {
                session.set_source_breakpoints(Path::new(path), breakpoints)?;
                Ok("[]".to_string())
            }
        }
    }

    pub fn data_breakpoint_info(
        &mut self,
        variables_reference: i64,
        frame_id: i64,
        name: Option<&str>,
    ) -> Result<String> {
        match &mut self.inner {
            SessionEngine::Dap(session) => {
                let info = session.data_breakpoint_info(variables_reference, frame_id, name)?;
                serde_json::to_string(&json!({
                    "dataId": info.data_id,
                    "description": info.description,
                    "accessTypes": info.access_types,
                }))
                .context("failed to serialize dataBreakpointInfo response")
            }
            SessionEngine::Rr(_) => anyhow::bail!("data breakpoints are not supported in rr sessions"),
        }
    }

    pub fn set_data_breakpoints(&mut self, breakpoints: &[DataBreakpoint]) -> Result<String> {
        match &mut self.inner {
            SessionEngine::Dap(session) => {
                let results = session.set_data_breakpoints(breakpoints)?;
                let json = results
                    .iter()
                    .map(|breakpoint| {
                        json!({
                            "dataId": breakpoint.data_id,
                            "verified": breakpoint.verified,
                            "message": breakpoint.message,
                        })
                    })
                    .collect::<Vec<_>>();
                serde_json::to_string(&json).context("failed to serialize data breakpoint results")
            }
            SessionEngine::Rr(_) => anyhow::bail!("data breakpoints are not supported in rr sessions"),
        }
    }

    pub fn set_exception_breakpoints(
        &mut self,
        settings: &[ExceptionBreakpointSetting],
    ) -> Result<()> {
        match &mut self.inner {
            SessionEngine::Dap(session) => session.set_exception_breakpoints(settings),
            SessionEngine::Rr(_) => anyhow::bail!("exception breakpoints are not supported in rr sessions"),
        }
    }

    pub fn set_function_breakpoints(&mut self, breakpoints: &[FunctionBreakpoint]) -> Result<String> {
        match &mut self.inner {
            SessionEngine::Dap(session) => {
                let results = session.set_function_breakpoints(breakpoints)?;
                let json = results
                    .iter()
                    .map(|breakpoint| {
                        json!({
                            "name": breakpoint.name,
                            "verified": breakpoint.verified,
                            "message": breakpoint.message,
                        })
                    })
                    .collect::<Vec<_>>();
                serde_json::to_string(&json).context("failed to serialize function breakpoint results")
            }
            SessionEngine::Rr(_) => anyhow::bail!("function breakpoints are not supported in rr sessions"),
        }
    }

    pub fn fetch_variables_json(
        &self,
        variables_reference: i64,
        _scope_name: Option<&str>,
    ) -> Result<String> {
        let variables = match &self.inner {
            SessionEngine::Dap(session) => session.variables(variables_reference)?,
            SessionEngine::Rr(session) => session.variables(variables_reference)?,
        };
        serde_json::to_string(&variables).context("failed to serialize variables")
    }

    pub fn set_variable(&self, variables_reference: i64, name: &str, value: &str) -> Result<String> {
        let variable = match &self.inner {
            SessionEngine::Dap(session) => session.set_variable(variables_reference, name, value)?,
            SessionEngine::Rr(session) => session.set_variable(variables_reference, name, value)?,
        };
        serde_json::to_string(&variable).context("failed to serialize setVariable result")
    }

    pub fn fetch_source(&self, source_reference: i64) -> Result<String> {
        match &self.inner {
            SessionEngine::Dap(session) => session.fetch_source(source_reference),
            SessionEngine::Rr(session) => session.fetch_source(source_reference),
        }
    }

    pub fn read_memory_json(&self, memory_reference: &str, offset: i64, count: i64) -> Result<String> {
        let result = match &self.inner {
            SessionEngine::Dap(session) => session.read_memory(memory_reference, offset, count)?,
            SessionEngine::Rr(_) => anyhow::bail!("readMemory is not supported in rr sessions"),
        };
        serde_json::to_string(&result).context("failed to serialize readMemory result")
    }

    pub fn write_memory_json(&self, memory_reference: &str, offset: i64, data: &str) -> Result<String> {
        let result = match &self.inner {
            SessionEngine::Dap(session) => session.write_memory(memory_reference, offset, data)?,
            SessionEngine::Rr(_) => anyhow::bail!("writeMemory is not supported in rr sessions"),
        };
        serde_json::to_string(&result).context("failed to serialize writeMemory result")
    }

    pub fn disassemble_json(
        &self,
        memory_reference: &str,
        instruction_offset: i64,
        offset: i64,
        instruction_count: i64,
    ) -> Result<String> {
        let instructions = match &self.inner {
            SessionEngine::Dap(session) => {
                session.disassemble(memory_reference, instruction_offset, offset, instruction_count)?
            }
            SessionEngine::Rr(_) => anyhow::bail!("disassemble is not supported in rr sessions"),
        };
        serde_json::to_string(&instructions).context("failed to serialize disassemble result")
    }

    pub fn fetch_step_in_targets_json(&self, frame_id: i64) -> Result<String> {
        let targets = match &self.inner {
            SessionEngine::Dap(session) => session.step_in_targets(frame_id)?,
            SessionEngine::Rr(session) => session.step_in_targets(frame_id)?,
        };
        serde_json::to_string(&targets).context("failed to serialize step-in targets")
    }

    pub fn fetch_goto_targets_json(
        &self,
        path: &str,
        line: i64,
        column: i64,
        source_reference: i64,
    ) -> Result<String> {
        let targets = match &self.inner {
            SessionEngine::Dap(session) => session.goto_targets(
                path,
                line,
                (column > 0).then_some(column),
                (source_reference > 0).then_some(source_reference),
            )?,
            SessionEngine::Rr(session) => session.goto_targets(path, line, column, source_reference)?,
        };
        serde_json::to_string(&targets).context("failed to serialize goto targets")
    }

    fn shutdown(&mut self) -> Result<()> {
        match &mut self.inner {
            SessionEngine::Dap(session) => session.shutdown(),
            SessionEngine::Rr(session) => session.shutdown(),
        }
    }
}

impl Drop for CSession {
    fn drop(&mut self) {
        let _ = self.shutdown();
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
    #[serde(default)]
    target_id: Option<i64>,
    #[serde(default)]
    path: Option<String>,
    #[serde(default)]
    line: Option<i64>,
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
            capabilities: Default::default(),
            breakpoint_hits: vec![],
            exception_info: None,
        };

        let json = snapshot_to_json(&snapshot).unwrap();
        let parsed: serde_json::Value = serde_json::from_str(&json).unwrap();
        assert_eq!(parsed["type"], "snapshot");
        assert_eq!(parsed["snapshot"]["state"]["Stopped"]["thread_id"], 1);
        assert_eq!(parsed["snapshot"]["state"]["Stopped"]["reason"], "entry");
    }
}
