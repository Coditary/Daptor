use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::json;
use tracing::{info, warn};

use crate::dap::protocol::{
    InboundMessage, OutputEventBody, Scope, StackFrame, StoppedEventBody, Thread, Variable,
};
pub use crate::dap::protocol::StepInTarget;
use crate::dap::{DapTransport, spawn_debugpy_adapter, spawn_lldb_dap_adapter};

mod rr_session;
pub use rr_session::RrDebugSession;

/// Which DAP adapter backs this session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DebugAdapterKind {
    Debugpy,
    Lldb,
}

/// Adapter features advertised to the UI (from DAP initialize).
#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct AdapterCapabilities {
    #[serde(default)]
    pub supports_step_back: bool,
    #[serde(default)]
    pub supports_step_in_targets: bool,
}

const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const WATCH_EVAL_TIMEOUT: Duration = Duration::from_secs(15);

/// When true, the debug adapter may stop/step into library / runtime source the adapter
/// exposes (e.g. debugpy `justMyCode: false` for Python stdlib). Adapter-specific launch
/// keys belong in [`Self::adapter_launch_extras`].
pub(crate) fn debuggee_includes_library_sources() -> bool {
    true
}

fn adapter_launch_extras() -> serde_json::Value {
    json!({
        "justMyCode": !debuggee_includes_library_sources(),
    })
}

fn sibling_source_path(program: &Path) -> Option<PathBuf> {
    for extension in ["c", "cpp", "cc", "cxx", "rs"] {
        let candidate = program.with_extension(extension);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

fn lldb_init_breakpoint_commands(program: &Path) -> Vec<String> {
    if let Some(source) = sibling_source_path(program) {
        return vec![format!(
            "breakpoint set --file '{}' --name main",
            source.display()
        )];
    }
    vec!["breakpoint set --name main".into()]
}

/// High-level debug session state exposed to the TUI layer.
#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
pub enum SessionState {
    #[default]
    Disconnected,
    Running,
    Stopped { thread_id: i64, reason: String },
    Exited,
}

/// Stack frames for a single thread (nvim-dap-ui fetches one trace per thread).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ThreadStackTrace {
    pub thread_id: i64,
    pub stack_frames: Vec<StackFrame>,
}

/// One source breakpoint sent to the debug adapter.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceBreakpoint {
    pub line: u32,
    pub condition: Option<String>,
}

/// Snapshot of debugger state at a stop point.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SessionSnapshot {
    pub state: SessionState,
    pub threads: Vec<Thread>,
    pub stack_frames: Vec<StackFrame>,
    #[serde(default)]
    pub thread_stacks: Vec<ThreadStackTrace>,
    pub scopes: Vec<Scope>,
    pub variables: Vec<Variable>,
    #[serde(default)]
    pub capabilities: AdapterCapabilities,
}

/// Owns the Dap transport and implements the debug session lifecycle.
pub struct DebugSession {
    transport: DapTransport,
    program: PathBuf,
    state: SessionState,
    active_thread: Option<i64>,
    active_frame: Option<i64>,
    /// Snapshot from launch (stop-on-entry) — avoids re-fetching on UI connect.
    initial_snapshot: Option<SessionSnapshot>,
    supports_step_in_targets: bool,
    supports_step_back: bool,
    adapter: DebugAdapterKind,
}

impl DebugSession {
    /// Connect to debugpy and launch `program`.
    pub fn launch_python(program: impl AsRef<Path>) -> Result<Self> {
        let program = program.as_ref().canonicalize().with_context(|| {
            format!("failed to resolve program path: {}", program.as_ref().display())
        })?;

        info!("spawning debugpy adapter");
        let child = spawn_debugpy_adapter()?;
        let transport = DapTransport::spawn(child)?;
        info!("debugpy adapter started, initializing DAP session");

        let mut session = Self {
            transport,
            program,
            state: SessionState::Disconnected,
            active_thread: None,
            active_frame: None,
            initial_snapshot: None,
            supports_step_in_targets: false,
            supports_step_back: false,
            adapter: DebugAdapterKind::Debugpy,
        };

        session.initialize_and_launch()?;
        Ok(session)
    }

    /// Connect to lldb-dap and launch a native binary.
    pub fn launch_native(program: impl AsRef<Path>) -> Result<Self> {
        let program = program.as_ref().canonicalize().with_context(|| {
            format!("failed to resolve program path: {}", program.as_ref().display())
        })?;

        info!("spawning lldb-dap adapter");
        let child = spawn_lldb_dap_adapter()?;
        let transport = DapTransport::spawn(child)?;
        info!("lldb-dap adapter started, initializing DAP session");

        let mut session = Self {
            transport,
            program,
            state: SessionState::Disconnected,
            active_thread: None,
            active_frame: None,
            initial_snapshot: None,
            supports_step_in_targets: false,
            supports_step_back: false,
            adapter: DebugAdapterKind::Lldb,
        };

        session.initialize_and_launch()?;
        Ok(session)
    }

    /// Spawn a background thread that launches debugpy and returns the session via channel.
    pub fn launch_python_async(
        program: impl AsRef<Path> + Send + 'static,
    ) -> crossbeam_channel::Receiver<Result<DebugSession>> {
        let (tx, rx) = crossbeam_channel::unbounded();
        std::thread::spawn(move || {
            let _ = tx.send(Self::launch_python(program));
        });
        rx
    }

    fn initialize_and_launch(&mut self) -> Result<()> {
        let (adapter_id, launch_type) = match self.adapter {
            DebugAdapterKind::Debugpy => ("debugpy", "debugpy"),
            DebugAdapterKind::Lldb => ("lldb-dap", "lldb"),
        };

        let init_seq = self.transport.send_request(
            "initialize",
            json!({
                "clientID": "tui-debug",
                "clientName": "tui-debug",
                "adapterID": adapter_id,
                "pathFormat": "path",
                "linesStartAt1": true,
                "columnsStartAt1": true,
                "supportsVariableType": true,
                "supportsVariablePaging": false,
                "supportsSetVariable": true,
                "supportsRunInTerminalRequest": false,
            }),
        )?;
        let init_body = self.transport.wait_response(init_seq, REQUEST_TIMEOUT)?;
        self.merge_capabilities_from_value(&init_body);
        info!(
            "DAP initialize complete (step_back={}, step_in_targets={}), launching debuggee",
            self.supports_step_back, self.supports_step_in_targets
        );

        // debugpy expects launch in-flight before configurationDone; initialized may
        // arrive before or after configurationDone and is queued for later handling.
        let mut launch_args = json!({
            "type": launch_type,
            "request": "launch",
            "program": self.program.to_string_lossy(),
            "stopOnEntry": match self.adapter {
                DebugAdapterKind::Lldb => false,
                DebugAdapterKind::Debugpy => true,
            },
        });

        match self.adapter {
            DebugAdapterKind::Debugpy => {
                launch_args["console"] = json!("internalConsole");
                launch_args["redirectOutput"] = json!(true);
                if let Some(extras) = adapter_launch_extras().as_object() {
                    launch_args
                        .as_object_mut()
                        .expect("launch payload object")
                        .extend(extras.clone());
                }
            }
            DebugAdapterKind::Lldb => {
                launch_args["initCommands"] = json!(lldb_init_breakpoint_commands(&self.program));
                if let Some(parent) = self.program.parent() {
                    launch_args["cwd"] = json!(parent);
                }
            }
        }

        let launch_seq = self.transport.send_request("launch", launch_args)?;

        let cfg_seq = self.transport.send_request("configurationDone", json!({}))?;
        self.transport.wait_response(cfg_seq, REQUEST_TIMEOUT)?;
        self.transport.wait_response(launch_seq, REQUEST_TIMEOUT)?;

        info!("launched {}, waiting for initial stop", self.program.display());

        if let Some(snapshot) = self.poll_until_stopped()? {
            self.initial_snapshot = Some(snapshot);
        }
        Ok(())
    }

    /// Cached stop-on-entry snapshot from launch; consumed once by the UI.
    pub fn take_initial_snapshot(&mut self) -> Option<SessionSnapshot> {
        self.initial_snapshot.take()
    }

    /// Snapshot for UI sync: cached launch stop, terminal states, or a live refresh.
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

    fn merge_capabilities_from_value(&mut self, value: &serde_json::Value) {
        if let Some(capabilities) = value.get("capabilities") {
            self.merge_capabilities_from_value(capabilities);
        }
        if let Some(enabled) = value
            .get("supportsStepInTargetsRequest")
            .and_then(|value| value.as_bool())
        {
            self.supports_step_in_targets = enabled;
        }
        if let Some(enabled) = value
            .get("supportsStepBack")
            .and_then(|value| value.as_bool())
        {
            self.supports_step_back = enabled;
        }
    }

    fn attach_capabilities(&self, mut snapshot: SessionSnapshot) -> SessionSnapshot {
        snapshot.capabilities = AdapterCapabilities {
            supports_step_back: self.supports_step_back,
            supports_step_in_targets: self.supports_step_in_targets,
        };
        snapshot
    }

    fn empty_state_snapshot(&self, state: SessionState) -> SessionSnapshot {
        self.attach_capabilities(SessionSnapshot {
            state,
            threads: vec![],
            stack_frames: vec![],
            thread_stacks: vec![],
            scopes: vec![],
            variables: vec![],
            capabilities: AdapterCapabilities::default(),
        })
    }

    fn stack_traces_for_threads(&self, threads: &[Thread]) -> Vec<ThreadStackTrace> {
        threads
            .iter()
            .filter_map(|thread| {
                self.stack_trace(thread.id)
                    .ok()
                    .map(|stack_frames| ThreadStackTrace {
                        thread_id: thread.id,
                        stack_frames,
                    })
            })
            .collect()
    }

    /// True when a `terminated` event is part of a DAP restart rather than a final exit.
    fn is_restart_event(body: &serde_json::Value) -> bool {
        match body.get("restart") {
            Some(value) if value.is_boolean() => value.as_bool().unwrap_or(false),
            Some(value) if !value.is_null() => true,
            _ => false,
        }
    }

    /// Drain events until we hit a stopped state or termination.
    pub fn poll_until_stopped(&mut self) -> Result<Option<SessionSnapshot>> {
        let deadline = std::time::Instant::now() + Duration::from_secs(60);

        while std::time::Instant::now() < deadline {
            if let Some(message) = self.transport.recv_event(Duration::from_millis(100)) {
                match message {
                    InboundMessage::Event { event, body } => {
                        match event.as_str() {
                            "stopped" => {
                                let stopped = serde_json::from_value::<StoppedEventBody>(body)
                                    .context("invalid stopped event body")?;
                                self.state = SessionState::Stopped {
                                    thread_id: stopped.thread_id,
                                    reason: stopped.reason,
                                };
                                self.active_thread = Some(stopped.thread_id);
                                return Ok(Some(self.refresh_snapshot()?));
                            }
                            "terminated" | "exited" => {
                                if Self::is_restart_event(&body) {
                                    continue;
                                }
                                self.state = SessionState::Exited;
                                return Ok(None);
                            }
                            "output" => {
                                if let Ok(output) =
                                    serde_json::from_value::<OutputEventBody>(body.clone())
                                {
                                    self.transport.push_output(output);
                                }
                            }
                            "capabilities" => {
                                self.merge_capabilities_from_value(&body);
                            }
                            "thread" => {
                                if self.handle_thread_event(&body)? {
                                    return Ok(None);
                                }
                            }
                            other => {
                                info!("event: {other}");
                            }
                        }
                    }
                    _ => {}
                }
            }
        }

        bail!("timed out waiting for debuggee to stop");
    }

    /// Non-blocking poll of DAP events; returns a snapshot on stop/exit, `None` when idle.
    pub fn poll_events(&mut self) -> Result<Option<SessionSnapshot>> {
        const MAX_EVENTS: usize = 64;
        let mut processed = 0usize;

        loop {
            if processed >= MAX_EVENTS {
                return Ok(None);
            }
            processed += 1;

            let Some(message) = self.transport.try_recv_event() else {
                return Ok(None);
            };

            match message {
                InboundMessage::Event { event, body } => match event.as_str() {
                    "stopped" => {
                        let stopped = serde_json::from_value::<StoppedEventBody>(body)
                            .context("invalid stopped event body")?;
                        self.state = SessionState::Stopped {
                            thread_id: stopped.thread_id,
                            reason: stopped.reason,
                        };
                        self.active_thread = Some(stopped.thread_id);
                        return Ok(Some(self.refresh_snapshot()?));
                    }
                    "terminated" | "exited" => {
                        if Self::is_restart_event(&body) {
                            continue;
                        }
                        self.state = SessionState::Exited;
                        return Ok(Some(self.empty_state_snapshot(SessionState::Exited)));
                    }
                    "output" => {
                        if let Ok(output) = serde_json::from_value::<OutputEventBody>(body.clone())
                        {
                            self.transport.push_output(output);
                        }
                    }
                    "capabilities" => {
                        self.merge_capabilities_from_value(&body);
                    }
                    "thread" => {
                        if self.handle_thread_event(&body)? {
                            return Ok(Some(self.empty_state_snapshot(SessionState::Exited)));
                        }
                    }
                    other => {
                        info!("event: {other}");
                    }
                },
                _ => {}
            }
        }
    }

    /// Returns `true` when the debuggee has exited (detected via thread exit while running).
    fn handle_thread_event(&mut self, body: &serde_json::Value) -> Result<bool> {
        let reason = body.get("reason").and_then(|v| v.as_str());
        if reason != Some("exited") {
            if reason.is_some() {
                info!("event: thread ({reason:?})");
            }
            return Ok(false);
        }

        if !matches!(self.state, SessionState::Running) {
            return Ok(false);
        }

        let exited_id = body.get("threadId").and_then(|v| v.as_i64());
        if exited_id == self.active_thread {
            self.state = SessionState::Exited;
            return Ok(true);
        }

        match self.threads() {
            Ok(threads) if threads.is_empty() => {
                self.state = SessionState::Exited;
                Ok(true)
            }
            Err(err) => {
                warn!("threads request after thread exit failed: {err:#}");
                self.state = SessionState::Exited;
                Ok(true)
            }
            Ok(_) => Ok(false),
        }
    }

    pub fn state(&self) -> &SessionState {
        &self.state
    }

    pub fn refresh_snapshot(&mut self) -> Result<SessionSnapshot> {
        let thread_id = self
            .active_thread
            .context("no active thread — session is not stopped")?;

        let threads = self.threads()?;
        let thread_stacks = self.stack_traces_for_threads(&threads);
        let stack_frames = thread_stacks
            .iter()
            .find(|trace| trace.thread_id == thread_id)
            .map(|trace| trace.stack_frames.clone())
            .unwrap_or_default();
        let frame_id = stack_frames
            .first()
            .map(|frame| frame.id)
            .context("no stack frames available")?;
        self.active_frame = Some(frame_id);

        let scopes = self.scopes(frame_id)?;
        let variables = scopes
            .iter()
            .find(|scope| scope.variables_reference > 0)
            .map(|scope| self.variables(scope.variables_reference))
            .transpose()?
            .unwrap_or_default();

        Ok(self.attach_capabilities(SessionSnapshot {
            state: self.state.clone(),
            threads,
            stack_frames,
            thread_stacks,
            scopes,
            variables,
            capabilities: AdapterCapabilities::default(),
        }))
    }

    pub fn continue_execution(&mut self) -> Result<Option<SessionSnapshot>> {
        self.dispatch_continue()?;
        self.poll_until_stopped()
    }

    /// Send continue without blocking for the next stop (UI event loop handles that).
    pub fn dispatch_continue(&mut self) -> Result<()> {
        let thread_id = self
            .active_thread
            .context("cannot continue without an active thread")?;
        let seq = self
            .transport
            .send_request("continue", json!({ "threadId": thread_id }))?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        Ok(())
    }

    pub fn dispatch_pause(&mut self) -> Result<()> {
        let seq = self.transport.send_request("pause", json!({}))?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        Ok(())
    }

    pub fn dispatch_step(&mut self, command: &'static str) -> Result<()> {
        if command == "stepIn" {
            return self.dispatch_step_in(None);
        }

        let thread_id = self
            .active_thread
            .context("cannot step without an active thread")?;
        let seq = self
            .transport
            .send_request(command, json!({ "threadId": thread_id }))?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        Ok(())
    }

    pub fn dispatch_step_in(&mut self, target_id: Option<i64>) -> Result<()> {
        let thread_id = self
            .active_thread
            .context("cannot step without an active thread")?;
        let mut args = json!({ "threadId": thread_id });
        if let Some(target_id) = target_id {
            args["targetId"] = json!(target_id);
        }
        let seq = self.transport.send_request("stepIn", args)?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        Ok(())
    }

    pub fn supports_step_in_targets(&self) -> bool {
        self.supports_step_in_targets
    }

    pub fn active_frame_id(&self) -> Option<i64> {
        self.active_frame
    }

    pub fn step_in_targets(&self, frame_id: i64) -> Result<Vec<StepInTarget>> {
        if !self.supports_step_in_targets {
            return Ok(vec![]);
        }

        let seq = self
            .transport
            .send_request("stepInTargets", json!({ "frameId": frame_id }))?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct StepInTargetsBody {
            targets: Vec<StepInTarget>,
        }

        let parsed = serde_json::from_value::<StepInTargetsBody>(body).unwrap_or(StepInTargetsBody {
            targets: vec![],
        });
        Ok(parsed.targets)
    }

    pub fn step_over(&mut self) -> Result<SessionSnapshot> {
        self.step("next")
    }

    pub fn step_into(&mut self) -> Result<SessionSnapshot> {
        self.step("stepIn")
    }

    pub fn step_out(&mut self) -> Result<SessionSnapshot> {
        self.step("stepOut")
    }

    fn step(&mut self, command: &'static str) -> Result<SessionSnapshot> {
        if command == "stepIn" {
            self.dispatch_step_in(None)?;
        } else {
            let thread_id = self
                .active_thread
                .context("cannot step without an active thread")?;
            let seq = self
                .transport
                .send_request(command, json!({ "threadId": thread_id }))?;
            self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
            self.state = SessionState::Running;
        }
        self.poll_until_stopped()?
            .context("program exited during step")
    }

    fn threads(&self) -> Result<Vec<Thread>> {
        let seq = self.transport.send_request("threads", json!({}))?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct ThreadsBody {
            threads: Vec<Thread>,
        }

        let parsed = serde_json::from_value::<ThreadsBody>(body)?;
        Ok(parsed.threads)
    }

    fn stack_trace(&self, thread_id: i64) -> Result<Vec<StackFrame>> {
        let seq = self.transport.send_request(
            "stackTrace",
            json!({
                "threadId": thread_id,
                "startFrame": 0,
                "levels": 50,
            }),
        )?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct StackTraceBody {
            #[serde(rename = "stackFrames")]
            stack_frames: Vec<StackFrame>,
        }

        let parsed = serde_json::from_value::<StackTraceBody>(body)?;
        Ok(parsed.stack_frames)
    }

    fn scopes(&self, frame_id: i64) -> Result<Vec<Scope>> {
        let seq = self
            .transport
            .send_request("scopes", json!({ "frameId": frame_id }))?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct ScopesBody {
            scopes: Vec<Scope>,
        }

        let parsed = serde_json::from_value::<ScopesBody>(body)?;
        Ok(parsed.scopes)
    }

    pub fn variables(&self, variables_reference: i64) -> Result<Vec<Variable>> {
        let seq = self.transport.send_request(
            "variables",
            json!({
                "variablesReference": variables_reference,
            }),
        )?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct VariablesBody {
            variables: Vec<Variable>,
        }

        let parsed = serde_json::from_value::<VariablesBody>(body)?;
        Ok(parsed.variables)
    }

    pub fn set_variable(&self, variables_reference: i64, name: &str, value: &str) -> Result<Variable> {
        if !matches!(self.state, SessionState::Stopped { .. }) {
            bail!("cannot set variable while program is running");
        }

        let seq = self.transport.send_request(
            "setVariable",
            json!({
                "variablesReference": variables_reference,
                "name": name,
                "value": value,
            }),
        )?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct SetVariableBody {
            name: Option<String>,
            value: String,
            #[serde(rename = "type")]
            type_name: Option<String>,
            #[serde(rename = "variablesReference")]
            variables_reference: i64,
        }

        let parsed = serde_json::from_value::<SetVariableBody>(body).context("invalid setVariable response")?;
        Ok(Variable {
            name: parsed.name.unwrap_or_else(|| name.to_string()),
            value: parsed.value,
            type_name: parsed.type_name,
            variables_reference: parsed.variables_reference,
        })
    }

    /// Fetch source text for a DAP `sourceReference` (adapter-provided buffer).
    pub fn fetch_source(&self, source_reference: i64) -> Result<String> {
        let seq = self.transport.send_request(
            "source",
            json!({ "sourceReference": source_reference }),
        )?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct SourceBody {
            content: String,
        }

        let parsed = serde_json::from_value::<SourceBody>(body).context("invalid source response")?;
        Ok(parsed.content)
    }

    /// Push UI breakpoints for one source file to the debug adapter.
    pub fn set_source_breakpoints(
        &self,
        path: &std::path::Path,
        breakpoints: &[SourceBreakpoint],
    ) -> Result<Vec<(u32, bool)>> {
        let breakpoints: Vec<serde_json::Value> = breakpoints
            .iter()
            .map(|breakpoint| {
                let mut payload = json!({ "line": breakpoint.line });
                if let Some(condition) = &breakpoint.condition {
                    if !condition.is_empty() {
                        payload["condition"] = json!(condition);
                    }
                }
                payload
            })
            .collect();

        let seq = self.transport.send_request(
            "setBreakpoints",
            json!({
                "source": { "path": path.to_string_lossy() },
                "breakpoints": breakpoints,
            }),
        )?;
        let body = self.transport.wait_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct BpResponse {
            breakpoints: Vec<BpInfo>,
        }
        #[derive(Deserialize)]
        struct BpInfo {
            line: u32,
            verified: bool,
        }

        let parsed = serde_json::from_value::<BpResponse>(body)?;
        Ok(parsed
            .breakpoints
            .into_iter()
            .map(|bp| (bp.line, bp.verified))
            .collect())
    }

    pub fn shutdown(&mut self) -> Result<()> {
        self.transport.shutdown()?;
        self.state = SessionState::Disconnected;
        self.active_thread = None;
        self.active_frame = None;
        self.initial_snapshot = None;
        Ok(())
    }

    /// Spawn a fresh session for the same program (after exit/disconnect).
    pub fn relaunch(&mut self) -> Result<()> {
        let program = self.program.clone();
        let adapter = self.adapter;
        let _ = self.shutdown();
        *self = match adapter {
            DebugAdapterKind::Debugpy => Self::launch_python(&program)?,
            DebugAdapterKind::Lldb => Self::launch_native(&program)?,
        };
        Ok(())
    }

    pub fn pause(&mut self) -> Result<Option<SessionSnapshot>> {
        self.dispatch_pause()?;
        self.poll_until_stopped()
    }

    pub fn play_pause(&mut self) -> Result<Option<SessionSnapshot>> {
        match &self.state {
            SessionState::Stopped { .. } => self.continue_execution(),
            SessionState::Running => self.pause(),
            _ => Ok(None),
        }
    }

    /// Non-blocking play/pause for the TUI event loop.
    pub fn dispatch_play_pause(&mut self) -> Result<()> {
        match &self.state {
            SessionState::Stopped { .. } => self.dispatch_continue(),
            SessionState::Running => self.dispatch_pause(),
            _ => Ok(()),
        }
    }

    pub fn evaluate(&self, expression: &str, frame_id: i64, context: &str) -> Result<String> {
        if !matches!(self.state, SessionState::Stopped { .. }) {
            bail!("cannot evaluate while program is running");
        }

        let seq = self.transport.send_request(
            "evaluate",
            json!({
                "expression": expression,
                "frameId": frame_id,
                "context": context,
            }),
        )?;
        let timeout = if context == "watch" {
            WATCH_EVAL_TIMEOUT
        } else {
            REQUEST_TIMEOUT
        };
        let body = self.transport.wait_response(seq, timeout)?;

        #[derive(Deserialize)]
        struct EvalBody {
            result: String,
        }

        let parsed = serde_json::from_value::<EvalBody>(body)?;
        Ok(parsed.result)
    }

    pub fn terminate(&mut self) -> Result<()> {
        if matches!(self.state, SessionState::Disconnected | SessionState::Exited) {
            self.state = SessionState::Exited;
            self.active_thread = None;
            self.active_frame = None;
            return Ok(());
        }

        let seq = self.transport.send_request("terminate", json!({}))?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Exited;
        self.active_thread = None;
        self.active_frame = None;
        Ok(())
    }

    pub fn disconnect(&mut self) -> Result<()> {
        if self.state == SessionState::Disconnected {
            return Ok(());
        }
        self.shutdown()
    }

    /// Buffered program stdout/stderr from DAP output events.
    pub fn drain_console_output(&self) -> Vec<OutputEventBody> {
        self.transport.take_output()
    }

    pub fn restart(&mut self) -> Result<()> {
        // debugpy does not reliably complete the DAP `restart` request with a follow-up
        // `stopped` event; relaunching the adapter avoids hanging in poll_until_stopped.
        self.relaunch()?;
        if self.initial_snapshot.is_none() {
            if let Some(snapshot) = self.poll_until_stopped()? {
                self.initial_snapshot = Some(snapshot);
            } else {
                bail!("restart completed without a stop-on-entry breakpoint");
            }
        }
        Ok(())
    }

    pub fn dispatch_restart(&mut self) -> Result<()> {
        let seq = self.transport.send_request(
            "restart",
            json!({
                "arguments": {
                    "program": self.program.to_string_lossy(),
                }
            }),
        )?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        Ok(())
    }

    pub fn step_back(&mut self) -> Result<SessionSnapshot> {
        if !self.supports_step_back {
            bail!("step back is not supported by this debug adapter");
        }
        let thread_id = self
            .active_thread
            .context("cannot step back without an active thread")?;
        let seq = self.transport.send_request(
            "stepBack",
            json!({ "threadId": thread_id }),
        )?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        self.poll_until_stopped()?
            .context("program exited during step back")
    }

    /// Run backward until the next stop (breakpoint, step, etc.).
    pub fn dispatch_reverse_continue(&mut self) -> Result<()> {
        if !self.supports_step_back {
            bail!("reverse continue is not supported by this debug adapter");
        }
        let thread_id = self
            .active_thread
            .context("cannot reverse continue without an active thread")?;
        let seq = self.transport.send_request(
            "reverseContinue",
            json!({ "threadId": thread_id }),
        )?;
        self.transport.wait_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        Ok(())
    }

    pub fn reverse_continue(&mut self) -> Result<SessionSnapshot> {
        self.dispatch_reverse_continue()?;
        self.poll_until_stopped()?
            .context("program exited during reverse continue")
    }
}

/// Pretty-print a snapshot for CLI smoke testing.
pub fn print_snapshot(snapshot: &SessionSnapshot) {
    println!("\n=== session state: {:?} ===", snapshot.state);

    println!("\n-- threads --");
    for thread in &snapshot.threads {
        println!("  [{}] {}", thread.id, thread.name);
    }

    println!("\n-- stack --");
    for frame in &snapshot.stack_frames {
        let path = frame
            .source
            .as_ref()
            .and_then(|source| source.path.as_deref())
            .or(frame.source.as_ref().and_then(|source| source.name.as_deref()))
            .unwrap_or("<unknown>");
        println!(
            "  #{} {} @ {}:{}",
            frame.id,
            frame.name,
            path,
            frame.line
        );
    }

    println!("\n-- scopes --");
    for scope in &snapshot.scopes {
        println!(
            "  {} (ref={}, expensive={})",
            scope.name,
            scope.variables_reference,
            scope.expensive
        );
    }

    println!("\n-- variables (first scope) --");
    for variable in &snapshot.variables {
        let type_name = variable
            .type_name
            .as_deref()
            .map(|t| format!(" ({})", t))
            .unwrap_or_default();
        println!(
            "  {} = {}{}",
            variable.name,
            variable.value,
            type_name
        );
    }
}

#[cfg(test)]
mod lifecycle_tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn library_sources_enabled_in_launch_payload() {
        assert!(debuggee_includes_library_sources());
        let extras = adapter_launch_extras();
        assert_eq!(extras.get("justMyCode"), Some(&json!(false)));
    }

    #[test]
    fn step_into_python_stdlib_source() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/stdlib_step.py")
            .canonicalize()
            .expect("stdlib_step.py fixture");

        let mut session = DebugSession::launch_python(&program).expect("launch");
        session
            .set_source_breakpoints(
                &program,
                &[SourceBreakpoint {
                    line: 20,
                    condition: None,
                }],
            )
            .expect("set breakpoint on json.dumps line");

        let at_bp = session
            .continue_execution()
            .expect("continue to breakpoint")
            .expect("snapshot at breakpoint");
        assert!(
            matches!(at_bp.state, SessionState::Stopped { .. }),
            "expected stopped at breakpoint, got {:?}",
            at_bp.state
        );

        session.dispatch_step("stepIn").expect("step into json.dumps");
        let stepped = session.refresh_snapshot().expect("snapshot in stdlib");
        let top = stepped
            .stack_frames
            .first()
            .expect("stack frame after step into");
        let path = top
            .source
            .as_ref()
            .and_then(|source| source.path.as_deref())
            .unwrap_or("");
        assert!(
            path.contains("/json/") && path.ends_with(".py"),
            "expected Python stdlib json source, got: {path}"
        );

        session.shutdown().expect("shutdown");
    }

    #[test]
    fn restart_while_running() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/hello.py")
            .canonicalize()
            .expect("hello.py fixture");

        let mut session = DebugSession::launch_python(&program).expect("launch");
        session.dispatch_continue().expect("continue");

        session.restart().expect("restart while running");
        let restarted = session
            .take_initial_snapshot()
            .or_else(|| session.refresh_snapshot().ok())
            .expect("snapshot after restart");
        assert!(matches!(restarted.state, SessionState::Stopped { .. }));

        session.shutdown().expect("shutdown");
    }

    #[test]
    fn restart_while_stopped_after_step() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/hello.py")
            .canonicalize()
            .expect("hello.py fixture");

        let mut session = DebugSession::launch_python(&program).expect("launch");
        let before = session.refresh_snapshot().expect("initial snapshot");
        let before_line = before.stack_frames.first().map(|frame| frame.line).unwrap_or(0);

        session.step_over().expect("step once");
        let stepped = session.refresh_snapshot().expect("stepped snapshot");
        let stepped_line = stepped.stack_frames.first().map(|frame| frame.line).unwrap_or(0);
        assert_ne!(stepped_line, before_line);

        session.restart().expect("restart while stopped");
        let restarted = session
            .take_initial_snapshot()
            .or_else(|| session.refresh_snapshot().ok())
            .expect("snapshot after restart");
        let restarted_line = restarted
            .stack_frames
            .first()
            .map(|frame| frame.line)
            .unwrap_or(0);
        assert_eq!(restarted_line, before_line);
        assert!(matches!(restarted.state, SessionState::Stopped { .. }));

        session.shutdown().expect("shutdown");
    }

    #[test]
    fn evaluate_watch_then_continue_with_breakpoint() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/hello.py")
            .canonicalize()
            .expect("hello.py fixture");

        let mut session = DebugSession::launch_python(&program).expect("launch");
        session
            .set_source_breakpoints(
                &program,
                &[SourceBreakpoint {
                    line: 10,
                    condition: None,
                }],
            )
            .expect("set breakpoint");

        let snapshot = session.refresh_snapshot().expect("initial snapshot");
        let frame_id = snapshot
            .stack_frames
            .first()
            .map(|frame| frame.id)
            .expect("frame id");
        session
            .evaluate("1 + 1", frame_id, "watch")
            .expect("watch evaluate");

        session.dispatch_continue().expect("continue");

        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        let mut reached_breakpoint = false;
        while std::time::Instant::now() < deadline {
            if let Ok(Some(snapshot)) = session.poll_events() {
                if matches!(snapshot.state, SessionState::Stopped { .. }) {
                    reached_breakpoint = true;
                    break;
                }
            }
            std::thread::sleep(Duration::from_millis(20));
        }

        assert!(reached_breakpoint, "expected to stop at breakpoint after continue");
        session.shutdown().expect("shutdown");
    }

    #[test]
    fn disconnect_restart_and_terminate_are_safe() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/hello.py")
            .canonicalize()
            .expect("hello.py fixture");

        let mut session = DebugSession::launch_python(&program).expect("launch");
        assert!(session.refresh_snapshot().is_ok());

        session.disconnect().expect("disconnect");
        let disconnected = session.snapshot_for_sync().expect("sync after disconnect");
        assert_eq!(disconnected.state, SessionState::Disconnected);

        // terminate on an already disconnected session is a safe no-op
        session.terminate().expect("terminate after disconnect");
        let exited = session.snapshot_for_sync().expect("sync after terminate");
        assert_eq!(exited.state, SessionState::Exited);

        session.restart().expect("restart after disconnect");
        let restarted = session
            .take_initial_snapshot()
            .or_else(|| session.refresh_snapshot().ok())
            .expect("snapshot after restart");
        assert!(matches!(
            restarted.state,
            SessionState::Stopped { .. } | SessionState::Running
        ));

        session.disconnect().expect("final disconnect");
    }
}

#[cfg(test)]
mod lldb_launch_tests {
    use super::*;
    use std::path::PathBuf;

    #[test]
    fn launch_native_reverse_demo_stops_in_main() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/reverse_demo")
            .canonicalize()
            .expect("reverse_demo fixture");

        let mut session = DebugSession::launch_native(&program).expect("lldb launch");
        let snap = session.snapshot_for_sync().expect("snapshot");
        println!("state={:?}", snap.state);
        for frame in &snap.stack_frames {
            let path = frame
                .source
                .as_ref()
                .and_then(|s| s.path.as_deref())
                .unwrap_or("");
            println!("frame {} line={} path={}", frame.name, frame.line, path);
        }
        assert!(matches!(snap.state, SessionState::Stopped { .. }), "{:?}", snap.state);
        assert!(!snap.stack_frames.is_empty(), "expected stack frames");
        session.shutdown().expect("shutdown");
    }

    #[test]
    fn launch_native_step_over_reverse_demo() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/reverse_demo")
            .canonicalize()
            .expect("reverse_demo fixture");

        let mut session = DebugSession::launch_native(&program).expect("lldb launch");
        let snap = session.step_over().expect("step over");
        for frame in &snap.stack_frames {
            let path = frame
                .source
                .as_ref()
                .and_then(|s| s.path.as_deref())
                .unwrap_or("");
            println!("after step_over: {} line={} path={}", frame.name, frame.line, path);
        }
        assert!(matches!(snap.state, SessionState::Stopped { .. }));
        assert!(
            !snap.capabilities.supports_step_back,
            "unexpected step-back support on this host"
        );
        let err = session
            .dispatch_reverse_continue()
            .expect_err("reverse continue should fail without trace support");
        assert!(
            err.to_string().contains("not supported"),
            "unexpected error: {err:#}"
        );
        session.shutdown().expect("shutdown");
    }
}
