use std::io::{BufRead, BufReader, BufWriter, Write};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::time::{Duration, Instant};

use anyhow::{Context, Result, bail};
use tracing::debug;

#[derive(Debug, Clone)]
pub struct MiFrame {
    pub level: i64,
    pub name: String,
    pub path: String,
    pub line: i64,
}

#[derive(Debug, Clone)]
pub struct MiVariable {
    pub name: String,
    pub value: String,
    pub type_name: Option<String>,
}

#[derive(Debug, Clone)]
pub struct MiStopReason {
    pub reason: String,
    pub thread_id: i64,
}

pub struct MiClient {
    child: Child,
    writer: BufWriter<ChildStdin>,
    reader: BufReader<std::process::ChildStdout>,
    token: i64,
}

impl MiClient {
    pub fn spawn() -> Result<Self> {
        let mut child = Command::new("gdb")
            .args(["-i=mi", "-nx", "--quiet"])
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .context("failed to spawn gdb — install gdb (required for rr replay debugging)")?;

        let stdin = child.stdin.take().context("gdb missing stdin")?;
        let stdout = child.stdout.take().context("gdb missing stdout")?;

        Ok(Self {
            child,
            writer: BufWriter::new(stdin),
            reader: BufReader::new(stdout),
            token: 0,
        })
    }

    pub fn connect_remote(&mut self, host: &str, port: u16) -> Result<()> {
        self.command(&format!("-target-select extended-remote {host}:{port}"))?;
        Ok(())
    }

    pub fn configure_for_rr_replay(&mut self) -> Result<()> {
        // rr recommends local sysroot so GDB reads recorded binaries from disk.
        self.command("-gdb-set sysroot /")?;
        Ok(())
    }

    pub fn insert_breakpoint_function(&mut self, function: &str) -> Result<()> {
        let response = self.command(&format!("-break-insert -f {function}"))?;
        if response.contains("^error") {
            bail!("failed to set breakpoint on {function}: {response}");
        }
        Ok(())
    }

    pub fn exec_run_to_initial_stop(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-run")
    }

    pub fn exec_continue(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-continue")
    }

    pub fn exec_next(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-next")
    }

    pub fn exec_step(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-step")
    }

    pub fn exec_finish(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-finish")
    }

    pub fn exec_next_back(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-next --reverse")
    }

    pub fn exec_step_back_into(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-step --reverse")
    }

    pub fn exec_reverse_continue(&mut self) -> Result<MiStopReason> {
        self.exec_until_stopped("-exec-continue --reverse")
    }

    pub fn stack_frames(&mut self) -> Result<Vec<MiFrame>> {
        let response = self.command("-stack-list-frames")?;
        Ok(parse_frames(&response))
    }

    pub fn stack_locals(&mut self, thread_id: i64, frame_level: i64) -> Result<Vec<MiVariable>> {
        let response = self.command(&format!(
            "-stack-list-locals --thread {thread_id} --frame {frame_level} 2"
        ))?;
        if response.contains("^error") {
            let msg = extract_quoted_field(&response, "msg").unwrap_or_else(|| response.clone());
            bail!("failed to list locals: {msg}");
        }
        Ok(parse_variables(&response))
    }

    pub fn insert_breakpoint(&mut self, path: &str, line: u32) -> Result<()> {
        let response = self.command(&format!("-break-insert -f \"{path}:{line}\""))?;
        if response.contains("^error") {
            bail!("failed to set breakpoint at {path}:{line}: {response}");
        }
        Ok(())
    }

    pub fn clear_breakpoints(&mut self) -> Result<()> {
        let listed = self.command("-break-list")?;
        for number in parse_breakpoint_numbers(&listed) {
            let _ = self.command(&format!("-break-delete {number}"));
        }
        Ok(())
    }

    pub fn evaluate(&mut self, expression: &str, thread_id: i64, frame_level: i64) -> Result<String> {
        let escaped = expression.replace('\\', "\\\\").replace('"', "\\\"");
        let response = self.command(&format!(
            "-data-evaluate-expression --thread {thread_id} --frame {frame_level} \"{escaped}\""
        ))?;
        if let Some(value) = extract_quoted_field(&response, "value") {
            return Ok(value);
        }
        if response.contains("^error") {
            if let Some(msg) = extract_quoted_field(&response, "msg") {
                bail!(msg);
            }
        }
        bail!("failed to evaluate expression")
    }

    pub fn shutdown(&mut self) -> Result<()> {
        let _ = self.command("-gdb-exit");
        let _ = self.child.kill();
        let _ = self.child.wait();
        Ok(())
    }

    fn exec_until_stopped(&mut self, exec_cmd: &str) -> Result<MiStopReason> {
        self.token += 1;
        let token = self.token;
        let line = format!("{token}{exec_cmd}\n");
        debug!(target: "mi", "send: {}", line.trim());
        self.writer.write_all(line.as_bytes())?;
        self.writer.flush()?;

        let deadline = Instant::now() + Duration::from_secs(60);
        while Instant::now() < deadline {
            let response = self.read_line()?;
            if response.is_empty() {
                continue;
            }
            debug!(target: "mi", "recv: {}", response);
            if let Some(stop) = parse_stopped(&response) {
                return Ok(stop);
            }
            if response.starts_with('^') && response.contains("error") {
                let msg = extract_quoted_field(&response, "msg").unwrap_or_else(|| response.clone());
                bail!("MI exec failed: {msg}");
            }
            if response.starts_with(&format!("{token}^")) {
                if response.contains("error") {
                    let msg = extract_quoted_field(&response, "msg").unwrap_or_else(|| response.clone());
                    bail!("MI exec failed: {msg}");
                }
            }
        }
        bail!("timed out waiting for MI stop after {exec_cmd}")
    }

    fn command(&mut self, cmd: &str) -> Result<String> {
        self.token += 1;
        let token = self.token;
        let line = format!("{token}{cmd}\n");
        debug!(target: "mi", "send: {}", line.trim());
        self.writer.write_all(line.as_bytes())?;
        self.writer.flush()?;

        let deadline = Instant::now() + Duration::from_secs(30);
        while Instant::now() < deadline {
            let response = self.read_line()?;
            if response.is_empty() {
                continue;
            }
            debug!(target: "mi", "recv: {}", response);
            if response.starts_with(&format!("{token}^")) {
                return Ok(response);
            }
            if response.starts_with("*stopped") {
                // Ignore async stops triggered outside explicit exec waits.
                continue;
            }
        }
        bail!("timed out waiting for MI response to {cmd}")
    }

    fn read_line(&mut self) -> Result<String> {
        let mut line = String::new();
        let bytes = self.reader.read_line(&mut line)?;
        if bytes == 0 {
            bail!("gdb MI connection closed");
        }
        Ok(line.trim_end().to_string())
    }
}

fn parse_stopped(line: &str) -> Option<MiStopReason> {
    if !line.starts_with("*stopped") {
        return None;
    }
    let reason = extract_quoted_field(line, "reason").unwrap_or_else(|| "stopped".into());
    let thread_id = extract_quoted_field(line, "thread-id")
        .and_then(|value| value.parse().ok())
        .unwrap_or(1);
    Some(MiStopReason { reason, thread_id })
}

fn parse_frames(response: &str) -> Vec<MiFrame> {
    let mut frames = Vec::new();
    for chunk in response.split("frame=") {
        if !chunk.contains("level=") {
            continue;
        }
        let level = extract_quoted_field(chunk, "level")
            .and_then(|value| value.parse().ok())
            .unwrap_or(frames.len() as i64);
        let name = extract_quoted_field(chunk, "func")
            .or_else(|| extract_quoted_field(chunk, "function"))
            .unwrap_or_else(|| "<unknown>".into());
        let path = extract_quoted_field(chunk, "fullname")
            .or_else(|| extract_quoted_field(chunk, "file"))
            .unwrap_or_default();
        let line = extract_quoted_field(chunk, "line")
            .and_then(|value| value.parse().ok())
            .unwrap_or(0);
        frames.push(MiFrame {
            level,
            name,
            path,
            line,
        });
    }
    frames.sort_by_key(|frame| frame.level);
    frames
}

fn parse_variables(response: &str) -> Vec<MiVariable> {
    let mut variables = Vec::new();
    for chunk in response.split('{') {
        if !chunk.contains("name=") {
            continue;
        }
        let name = extract_quoted_field(chunk, "name").unwrap_or_default();
        if name.is_empty() {
            continue;
        }
        let value = extract_quoted_field(chunk, "value").unwrap_or_else(|| "?".into());
        let type_name = extract_quoted_field(chunk, "type");
        variables.push(MiVariable {
            name,
            value,
            type_name,
        });
    }
    variables
}

fn parse_breakpoint_numbers(response: &str) -> Vec<i64> {
    let mut numbers = Vec::new();
    for chunk in response.split("bkpt=") {
        if let Some(number) = extract_quoted_field(chunk, "number").and_then(|v| v.parse().ok()) {
            numbers.push(number);
        }
    }
    numbers
}

fn extract_quoted_field(payload: &str, key: &str) -> Option<String> {
    let needle = format!("{key}=\"");
    let start = payload.find(&needle)? + needle.len();
    let rest = &payload[start..];
    let mut escaped = false;
    let mut end = 0;
    for (index, ch) in rest.char_indices() {
        if escaped {
            escaped = false;
            continue;
        }
        if ch == '\\' {
            escaped = true;
            continue;
        }
        if ch == '"' {
            end = index;
            break;
        }
    }
    Some(rest[..end].replace("\\\"", "\""))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_locals_list() {
        let response = r#"7^done,locals=[{name="x",type="int",value="10"},{name="y",type="int",value="32"}]"#;
        let vars = parse_variables(response);
        assert_eq!(vars.len(), 2);
        assert_eq!(vars[0].name, "x");
        assert_eq!(vars[0].value, "10");
        assert_eq!(vars[1].name, "y");
        assert_eq!(vars[1].value, "32");
    }

    #[test]
    fn parse_frame_list() {
        let response = r#"1^done,stack=[frame={level="0",addr="0x1",func="main",fullname="/tmp/a.c",line="9"}]"#;
        let frames = parse_frames(response);
        assert_eq!(frames.len(), 1);
        assert_eq!(frames[0].name, "main");
        assert_eq!(frames[0].line, 9);
    }

    #[test]
    fn parse_stop_reason() {
        let line = r#"*stopped,reason="breakpoint-hit",thread-id="1",frame={func="main"}"#;
        let stop = parse_stopped(line).expect("stop");
        assert_eq!(stop.reason, "breakpoint-hit");
        assert_eq!(stop.thread_id, 1);
    }
}
