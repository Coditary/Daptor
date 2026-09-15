use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use serde::{Deserialize, Serialize};
use serde_json::{Value, json};
use tracing::{info, warn};

use crate::dap::protocol::{
    ExceptionInfo, InboundMessage, OutputEventBody, Scope, StackFrame, StoppedEventBody, Thread,
    Variable,
};
use crate::terminal::{DebuggeeIo, DebuggeeTerminal, LldbStdioChannels, parse_run_in_terminal};
use std::sync::{Arc, Mutex};
pub use crate::dap::protocol::{GotoTarget, StepInTarget};
use crate::dap::{DapTransport, spawn_debugpy_adapter, spawn_lldb_dap_adapter};
use crate::network::NetworkCapture;

mod rr_session;
pub use rr_session::RrDebugSession;

fn is_heavy_register_group(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    ["vector", "simd", "mmx", "sse", "avx", "neon"]
        .iter()
        .any(|token| lower.contains(token))
}

fn is_simd_register_name(name: &str) -> bool {
    let lower = name.to_ascii_lowercase();
    lower.starts_with("xmm")
        || lower.starts_with("ymm")
        || lower.starts_with("zmm")
        || (lower.starts_with("mm")
            && lower.len() >= 3
            && lower[2..].chars().all(|ch| ch.is_ascii_digit()))
}

fn bytes_to_hex(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        use std::fmt::Write as _;
        let _ = write!(out, "{:02x}", byte);
    }
    out
}

fn hex_to_bytes(hex: &str) -> Result<Vec<u8>> {
    let cleaned = hex
        .chars()
        .filter(|ch| !ch.is_whitespace())
        .collect::<String>();
    if cleaned.is_empty() {
        return Ok(Vec::new());
    }
    if cleaned.len() % 2 != 0 {
        bail!("hex input must have an even number of digits");
    }
    let mut out = Vec::with_capacity(cleaned.len() / 2);
    let chars = cleaned.as_bytes();
    for i in (0..chars.len()).step_by(2) {
        let pair = std::str::from_utf8(&chars[i..i + 2]).context("invalid hex digit")?;
        let byte = u8::from_str_radix(pair, 16).context("invalid hex digit")?;
        out.push(byte);
    }
    Ok(out)
}

/// Which DAP adapter backs this session.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DebugAdapterKind {
    Debugpy,
    Lldb,
}

/// One enabled exception breakpoint filter (and optional per-filter condition).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExceptionBreakpointSetting {
    pub filter: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub condition: Option<String>,
}

/// One exception breakpoint filter from DAP `exceptionBreakpointFilters`.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ExceptionBreakpointFilter {
    pub filter: String,
    pub label: String,
    #[serde(default)]
    pub description: Option<String>,
    #[serde(default)]
    pub default: bool,
    #[serde(default)]
    pub supports_condition: bool,
}

/// Adapter features advertised to the UI (from DAP initialize).
#[derive(Debug, Clone, PartialEq, Eq, Default, Serialize, Deserialize)]
pub struct AdapterCapabilities {
    #[serde(default)]
    pub supports_step_back: bool,
    #[serde(default)]
    pub supports_step_in_targets: bool,
    #[serde(default)]
    pub supports_goto_targets: bool,
    #[serde(default)]
    pub supports_data_breakpoints: bool,
    #[serde(default)]
    pub supports_function_breakpoints: bool,
    #[serde(default)]
    pub supports_completions_request: bool,
    #[serde(default)]
    pub supports_exception_info_request: bool,
    #[serde(default)]
    pub supports_read_memory_request: bool,
    #[serde(default)]
    pub supports_write_memory_request: bool,
    #[serde(default)]
    pub supports_disassemble_request: bool,
    #[serde(default)]
    pub exception_breakpoint_filters: Vec<ExceptionBreakpointFilter>,
}

/// One disassembled machine instruction from DAP `disassemble`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DisassembledInstruction {
    pub address: String,
    #[serde(rename = "instructionBytes", default, skip_serializing_if = "Option::is_none")]
    pub instruction_bytes: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub instruction: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub symbol: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub line: Option<i64>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub column: Option<i64>,
}

/// Result of DAP `readMemory` (hex-encoded bytes for the C API layer).
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ReadMemoryResult {
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub address: Option<String>,
    /// Raw bytes as lowercase hex (two digits per byte).
    pub data: String,
    #[serde(rename = "unreadableBytes", default, skip_serializing_if = "Option::is_none")]
    pub unreadable_bytes: Option<i64>,
}

/// Result of DAP `writeMemory`.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct WriteMemoryResult {
    pub offset: i64,
    #[serde(rename = "bytesWritten")]
    pub bytes_written: i64,
}

/// A single REPL completion candidate from the debug adapter.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct CompletionItem {
    pub label: String,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub sort_text: Option<String>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub detail: Option<String>,
    #[serde(default)]
    pub selection_start: usize,
    #[serde(default)]
    pub selection_length: usize,
}

const REQUEST_TIMEOUT: Duration = Duration::from_secs(30);
const WATCH_EVAL_TIMEOUT: Duration = Duration::from_secs(15);

/// When true, the debug adapter may stop/step into library / runtime source the adapter
/// exposes (e.g. debugpy `justMyCode: false` for Python stdlib). Adapter-specific launch
/// keys belong in [`Self::adapter_launch_extras`].
pub(crate) fn debuggee_includes_library_sources() -> bool {
    true
}

fn is_runtime_library_source_path(path: &str) -> bool {
    path.is_empty()
        || path.contains(".so")
        || path.contains("/lib/")
        || path.contains("/usr/lib")
        || path.contains("/lib64/")
}

fn preferred_stack_frame(stack_frames: &[StackFrame]) -> Option<&StackFrame> {
    stack_frames
        .iter()
        .find(|frame| {
            let path = frame
                .source
                .as_ref()
                .and_then(|source| source.path.as_deref())
                .unwrap_or("");
            !is_runtime_library_source_path(path) && frame.line > 0
        })
        .or_else(|| stack_frames.first())
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

fn parse_lldb_breakpoint_list(output: &str) -> Vec<(String, u32, u32)> {
    let mut results = Vec::new();
    for line in output.lines() {
        if !line.contains("hit count =") {
            continue;
        }
        let Some(file_start) = line.find("file = '") else {
            continue;
        };
        let file_body = &line[file_start + 8..];
        let Some(file_end) = file_body.find('\'') else {
            continue;
        };
        let path = file_body[..file_end].to_string();

        let Some(line_start) = line.find("line = ") else {
            continue;
        };
        let line_body = line[line_start + 7..].trim_start();
        let line_digits = line_body
            .chars()
            .take_while(|ch| ch.is_ascii_digit())
            .collect::<String>();
        let Ok(bp_line) = line_digits.parse::<u32>() else {
            continue;
        };

        let Some(hit_start) = line.find("hit count = ") else {
            continue;
        };
        let hit_body = line[hit_start + 12..].trim_start();
        let hit_digits = hit_body
            .chars()
            .take_while(|ch| ch.is_ascii_digit())
            .collect::<String>();
        let Ok(hit_count) = hit_digits.parse::<u32>() else {
            continue;
        };

        if bp_line > 0 {
            results.push((path, bp_line, hit_count));
        }
    }
    results
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
    pub hit_condition: Option<String>,
}

/// Adapter response for one source breakpoint.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SourceBreakpointResult {
    pub id: Option<i64>,
    pub line: u32,
    pub verified: bool,
    pub hit_count: Option<u32>,
}

/// Cached hit count for a source breakpoint (from DAP `hitCount` / `breakpoint` events).
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct BreakpointHitInfo {
    pub path: String,
    pub line: u32,
    pub hit_count: u32,
}

/// Data breakpoint sent to the debug adapter (`setDataBreakpoints`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataBreakpoint {
    pub data_id: String,
    pub description: String,
    pub access_type: String,
    pub condition: Option<String>,
}

/// Result of `dataBreakpointInfo`.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataBreakpointInfoResult {
    pub data_id: String,
    pub description: Option<String>,
    pub access_types: Vec<String>,
}

/// Adapter response for one data breakpoint.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct DataBreakpointResult {
    pub data_id: String,
    pub verified: bool,
    pub message: Option<String>,
}

/// Function breakpoint sent to the debug adapter (`setFunctionBreakpoints`).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FunctionBreakpoint {
    pub name: String,
    pub condition: Option<String>,
    pub hit_condition: Option<String>,
}

/// Adapter response for one function breakpoint.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct FunctionBreakpointResult {
    pub name: String,
    pub verified: bool,
    pub message: Option<String>,
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
    #[serde(default)]
    pub breakpoint_hits: Vec<BreakpointHitInfo>,
    #[serde(default, skip_serializing_if = "Option::is_none")]
    pub exception_info: Option<ExceptionInfo>,
    /// OS process IDs from DAP `process` events (`systemProcessId`), aggregated for metrics.
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub debug_process_ids: Vec<u32>,
}

/// Owns the Dap transport and implements the debug session lifecycle.
pub struct DebugSession {
    transport: DapTransport,
    program: PathBuf,
    program_args: Vec<String>,
    state: SessionState,
    active_thread: Option<i64>,
    active_frame: Option<i64>,
    /// Snapshot from launch (stop-on-entry) — avoids re-fetching on UI connect.
    initial_snapshot: Option<SessionSnapshot>,
    supports_step_in_targets: bool,
    supports_step_back: bool,
    supports_goto_targets: bool,
    supports_data_breakpoints: bool,
    supports_function_breakpoints: bool,
    supports_completions_request: bool,
    supports_exception_info_request: bool,
    supports_read_memory_request: bool,
    supports_write_memory_request: bool,
    supports_disassemble_request: bool,
    exception_breakpoint_filters: Vec<ExceptionBreakpointFilter>,
    adapter: DebugAdapterKind,
    breakpoint_hit_counts: HashMap<(String, u32), u32>,
    breakpoint_ids: HashMap<i64, (String, u32)>,
    last_breakpoint_requests: HashMap<String, Vec<SourceBreakpoint>>,
    debuggee_io: Arc<Mutex<Option<DebuggeeIo>>>,
    /// Tracked debuggee OS PIDs from DAP `process` events (multiple for subprocesses).
    debug_process_ids: Vec<u32>,
    network_capture: Option<NetworkCapture>,
}

fn start_network_capture() -> Option<NetworkCapture> {
    match NetworkCapture::start() {
        Ok(capture) => {
            info!("network capture proxy listening on {}", capture.proxy_address());
            Some(capture)
        }
        Err(error) => {
            warn!("network capture unavailable: {error:#}");
            None
        }
    }
}

fn inject_network_proxy_env(launch_args: &mut Value, capture: &NetworkCapture) {
    let proxy = format!("http://{}", capture.proxy_address());
    let ca = capture.ca_cert_path().to_string_lossy();
    launch_args["env"] = json!({
        "HTTP_PROXY": proxy,
        "HTTPS_PROXY": proxy,
        "http_proxy": proxy,
        "https_proxy": proxy,
        "SSL_CERT_FILE": ca,
        "REQUESTS_CA_BUNDLE": ca,
    });
}

impl DebugSession {
    /// Connect to debugpy and launch `program`.
    pub fn launch_python(program: impl AsRef<Path>) -> Result<Self> {
        Self::launch_python_with_args(program, &[])
    }

    /// Connect to debugpy and launch `program` with CLI arguments.
    pub fn launch_python_with_args(program: impl AsRef<Path>, args: &[String]) -> Result<Self> {
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
            program_args: args.to_vec(),
            state: SessionState::Disconnected,
            active_thread: None,
            active_frame: None,
            initial_snapshot: None,
            supports_step_in_targets: false,
            supports_step_back: false,
            supports_goto_targets: false,
            supports_data_breakpoints: false,
            supports_function_breakpoints: false,
            supports_completions_request: false,
            supports_exception_info_request: false,
            supports_read_memory_request: false,
            supports_write_memory_request: false,
            supports_disassemble_request: false,
            exception_breakpoint_filters: Vec::new(),
            adapter: DebugAdapterKind::Debugpy,
            breakpoint_hit_counts: HashMap::new(),
            breakpoint_ids: HashMap::new(),
            last_breakpoint_requests: HashMap::new(),
            debuggee_io: Arc::new(Mutex::new(None)),
            debug_process_ids: Vec::new(),
            network_capture: start_network_capture(),
        };

        session.initialize_and_launch()?;
        Ok(session)
    }

    /// Connect to lldb-dap and launch a native binary.
    pub fn launch_native(program: impl AsRef<Path>) -> Result<Self> {
        Self::launch_native_with_args(program, &[])
    }

    /// Connect to lldb-dap and launch a native binary with CLI arguments.
    pub fn launch_native_with_args(program: impl AsRef<Path>, args: &[String]) -> Result<Self> {
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
            program_args: args.to_vec(),
            state: SessionState::Disconnected,
            active_thread: None,
            active_frame: None,
            initial_snapshot: None,
            supports_step_in_targets: false,
            supports_step_back: false,
            supports_goto_targets: false,
            supports_data_breakpoints: false,
            supports_function_breakpoints: false,
            supports_completions_request: false,
            supports_exception_info_request: false,
            supports_read_memory_request: false,
            supports_write_memory_request: false,
            supports_disassemble_request: false,
            exception_breakpoint_filters: Vec::new(),
            adapter: DebugAdapterKind::Lldb,
            breakpoint_hit_counts: HashMap::new(),
            breakpoint_ids: HashMap::new(),
            last_breakpoint_requests: HashMap::new(),
            debuggee_io: Arc::new(Mutex::new(None)),
            debug_process_ids: Vec::new(),
            network_capture: start_network_capture(),
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

    fn wait_dap_response(&self, request_seq: i64, timeout: Duration) -> Result<Value> {
        let debuggee_io = Arc::clone(&self.debuggee_io);
        let transport = &self.transport;
        self.transport.wait_response(request_seq, timeout, |message| {
            handle_adapter_request(message, &debuggee_io, transport)
        })
    }

    pub fn terminal_write_input(&self, bytes: &[u8]) -> Result<()> {
        let mut guard = self
            .debuggee_io
            .lock()
            .expect("debuggee io lock poisoned");
        if let Some(io) = guard.as_mut() {
            io.write_input(bytes)?;
        }
        Ok(())
    }

    pub fn terminal_resize(&self, rows: u16, cols: u16) {
        let guard = self
            .debuggee_io
            .lock()
            .expect("debuggee io lock poisoned");
        if let Some(io) = guard.as_ref() {
            io.resize(rows, cols);
        }
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
                "supportsRunInTerminalRequest": true,
                "supportsMemoryReferences": true,
                "supportsMemoryEvent": true,
            }),
        )?;
        let init_body = self.wait_dap_response(init_seq, REQUEST_TIMEOUT)?;
        self.merge_capabilities_from_value(&init_body);
        if self.adapter == DebugAdapterKind::Lldb {
            if !self.supports_function_breakpoints {
                // lldb-dap supports setFunctionBreakpoints even when initialize omits the flag.
                self.supports_function_breakpoints = true;
            }
            if !self.supports_read_memory_request {
                self.supports_read_memory_request = true;
            }
            if !self.supports_write_memory_request {
                self.supports_write_memory_request = true;
            }
            if !self.supports_disassemble_request {
                self.supports_disassemble_request = true;
            }
        }
        info!(
            "DAP initialize complete (step_back={}, step_in_targets={}, goto_targets={}, function_bps={}), launching debuggee",
            self.supports_step_back,
            self.supports_step_in_targets,
            self.supports_goto_targets,
            self.supports_function_breakpoints
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

        if !self.program_args.is_empty() {
            launch_args["args"] = json!(self.program_args);
        }

        if let Some(capture) = &self.network_capture {
            inject_network_proxy_env(&mut launch_args, capture);
        }

        match self.adapter {
            DebugAdapterKind::Debugpy => {
                launch_args["console"] = json!("integratedTerminal");
                launch_args["redirectOutput"] = json!(false);
                if let Some(extras) = adapter_launch_extras().as_object() {
                    launch_args
                        .as_object_mut()
                        .expect("launch payload object")
                        .extend(extras.clone());
                }
            }
            DebugAdapterKind::Lldb => {
                let channels = LldbStdioChannels::open()?;
                let stdin = channels.stdin_path();
                {
                    let mut guard = self
                        .debuggee_io
                        .lock()
                        .expect("debuggee io lock poisoned");
                    *guard = Some(DebuggeeIo::LldbStdio(channels));
                }
                // Redirect only stdin; null keeps stdout/stderr on lldb's DAP output channel.
                launch_args["stdio"] = json!([stdin, Value::Null, Value::Null]);
                launch_args["initCommands"] = json!(lldb_init_breakpoint_commands(&self.program));
                if let Some(parent) = self.program.parent() {
                    launch_args["cwd"] = json!(parent);
                }
            }
        }

        let launch_seq = self.transport.send_request("launch", launch_args)?;
        let cfg_seq = self.transport.send_request("configurationDone", json!({}))?;
        self.wait_dap_response(cfg_seq, REQUEST_TIMEOUT)?;
        self.wait_dap_response(launch_seq, REQUEST_TIMEOUT)?;

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
            SessionState::Stopped { .. } => self.enrich_stopped_snapshot(),
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
            .get("supportsGotoTargetsRequest")
            .and_then(|value| value.as_bool())
        {
            self.supports_goto_targets = enabled;
        }
        if let Some(enabled) = value
            .get("supportsStepBack")
            .and_then(|value| value.as_bool())
        {
            self.supports_step_back = enabled;
        }
        if let Some(enabled) = value
            .get("supportsDataBreakpoints")
            .and_then(|value| value.as_bool())
        {
            self.supports_data_breakpoints = enabled;
        }
        if let Some(enabled) = value
            .get("supportsFunctionBreakpoints")
            .and_then(|value| value.as_bool())
        {
            self.supports_function_breakpoints = enabled;
        }
        if let Some(enabled) = value
            .get("supportsCompletionsRequest")
            .and_then(|value| value.as_bool())
        {
            self.supports_completions_request = enabled;
        }
        if let Some(enabled) = value
            .get("supportsExceptionInfoRequest")
            .and_then(|value| value.as_bool())
        {
            self.supports_exception_info_request = enabled;
        }
        if let Some(enabled) = value
            .get("supportsReadMemoryRequest")
            .and_then(|value| value.as_bool())
        {
            self.supports_read_memory_request = enabled;
        }
        if let Some(enabled) = value
            .get("supportsWriteMemoryRequest")
            .and_then(|value| value.as_bool())
        {
            self.supports_write_memory_request = enabled;
        }
        if let Some(enabled) = value
            .get("supportsDisassembleRequest")
            .and_then(|value| value.as_bool())
        {
            self.supports_disassemble_request = enabled;
        }
        if let Some(filters) = value
            .get("exceptionBreakpointFilters")
            .and_then(|value| value.as_array())
        {
            self.exception_breakpoint_filters = filters
                .iter()
                .filter_map(|item| {
                    let filter = item
                        .get("filter")
                        .and_then(|value| value.as_str())
                        .map(str::to_string)
                        .filter(|value| !value.is_empty())?;
                    let label = item
                        .get("label")
                        .and_then(|value| value.as_str())
                        .map(str::to_string)
                        .filter(|value| !value.is_empty())
                        .unwrap_or_else(|| filter.clone());
                    let description = item
                        .get("description")
                        .and_then(|value| value.as_str())
                        .map(str::to_string)
                        .filter(|value| !value.is_empty());
                    let default = item
                        .get("default")
                        .and_then(|value| value.as_bool())
                        .unwrap_or(false);
                    let supports_condition = item
                        .get("supportsCondition")
                        .and_then(|value| value.as_bool())
                        .unwrap_or(false);
                    Some(ExceptionBreakpointFilter {
                        filter,
                        label,
                        description,
                        default,
                        supports_condition,
                    })
                })
                .collect();
        }
    }

    fn attach_capabilities(&self, mut snapshot: SessionSnapshot) -> SessionSnapshot {
        snapshot.capabilities = AdapterCapabilities {
            supports_step_back: self.supports_step_back,
            supports_step_in_targets: self.supports_step_in_targets,
            supports_goto_targets: self.supports_goto_targets,
            supports_data_breakpoints: self.supports_data_breakpoints,
            supports_function_breakpoints: self.supports_function_breakpoints,
            supports_completions_request: self.supports_completions_request,
            supports_exception_info_request: self.supports_exception_info_request,
            supports_read_memory_request: self.supports_read_memory_request,
            supports_write_memory_request: self.supports_write_memory_request,
            supports_disassemble_request: self.supports_disassemble_request,
            exception_breakpoint_filters: self.exception_breakpoint_filters.clone(),
        };
        snapshot.breakpoint_hits = self.collect_breakpoint_hits();
        snapshot.debug_process_ids = self.debug_process_ids.clone();
        snapshot
    }

    fn handle_process_event(&mut self, body: &serde_json::Value) {
        let Some(pid) = body.get("systemProcessId").and_then(|value| value.as_u64()) else {
            return;
        };
        if pid == 0 || pid > u32::MAX as u64 {
            return;
        }
        let pid = pid as u32;
        if self.debug_process_ids.contains(&pid) {
            return;
        }
        self.debug_process_ids.push(pid);
        info!("tracking debug process pid={pid}");
    }

    fn clear_debug_process_ids(&mut self) {
        if !self.debug_process_ids.is_empty() {
            self.debug_process_ids.clear();
        }
    }

    fn record_breakpoint_hit_count(&mut self, path: &str, line: u32, hit_count: u32) {
        if line == 0 {
            return;
        }
        let key = (path.to_string(), line);
        self.breakpoint_hit_counts
            .entry(key)
            .and_modify(|count| *count = (*count).max(hit_count))
            .or_insert(hit_count);
    }

    fn collect_breakpoint_hits(&self) -> Vec<BreakpointHitInfo> {
        self.breakpoint_hit_counts
            .iter()
            .map(|((path, line), hit_count)| BreakpointHitInfo {
                path: path.clone(),
                line: *line,
                hit_count: *hit_count,
            })
            .collect()
    }

    fn apply_source_breakpoint_results(
        &mut self,
        path: &str,
        request_lines: &[u32],
        results: &[SourceBreakpointResult],
    ) {
        for (index, breakpoint) in results.iter().enumerate() {
            let line = if breakpoint.line > 0 {
                breakpoint.line
            } else {
                request_lines.get(index).copied().unwrap_or(0)
            };
            if line == 0 {
                continue;
            }
            if let Some(id) = breakpoint.id {
                self.breakpoint_ids.insert(id, (path.to_string(), line));
            }
            if let Some(hit_count) = breakpoint.hit_count {
                self.record_breakpoint_hit_count(path, line, hit_count);
            }
        }
    }

    fn handle_breakpoint_event(&mut self, body: &serde_json::Value) {
        let breakpoint = body.get("breakpoint");
        let line = breakpoint
            .and_then(|value| value.get("line"))
            .and_then(|value| value.as_u64())
            .map(|value| value as u32);
        let hit_count = breakpoint
            .and_then(|value| value.get("hitCount"))
            .and_then(|value| value.as_u64())
            .map(|value| value as u32);
        let path = breakpoint
            .and_then(|value| value.get("source"))
            .and_then(|value| value.get("path"))
            .and_then(|value| value.as_str())
            .map(str::to_string);
        let id = breakpoint
            .and_then(|value| value.get("id"))
            .and_then(|value| value.as_i64());

        let resolved = match (path, line) {
            (Some(path), Some(line)) if line > 0 => Some((path, line)),
            _ => id.and_then(|breakpoint_id| self.breakpoint_ids.get(&breakpoint_id).cloned()),
        };

        if let (Some((path, line)), Some(hit_count)) = (resolved, hit_count) {
            if line > 0 {
                self.breakpoint_hit_counts.insert((path, line), hit_count);
            }
        }
    }

    fn refresh_breakpoint_hit_counts(&mut self) {
        let requests = self
            .last_breakpoint_requests
            .iter()
            .map(|(path, breakpoints)| (path.clone(), breakpoints.clone()))
            .collect::<Vec<_>>();

        for (path, breakpoints) in requests {
            if breakpoints.is_empty() {
                continue;
            }
            let _ = self.set_source_breakpoints(Path::new(&path), &breakpoints);
        }
    }

    fn update_lldb_hit_counts_from_break_list(&mut self, frame_id: i64) {
        for command in ["!breakpoint list", "`breakpoint list`", "!break list", "breakpoint list"] {
            let output = match self.evaluate(command, frame_id, "repl") {
                Ok(output) => output,
                Err(_) => continue,
            };
            let parsed = parse_lldb_breakpoint_list(&output);
            if parsed.is_empty() {
                continue;
            }
            for (path, line, hit_count) in parsed {
                if line > 0 {
                    self.breakpoint_hit_counts.insert((path, line), hit_count);
                }
            }
            return;
        }
        warn!("failed to query lldb breakpoint hit counts");
    }

    fn enrich_stopped_snapshot(&mut self) -> Result<SessionSnapshot> {
        self.refresh_breakpoint_hit_counts();

        let mut snapshot = self.refresh_snapshot()?;
        if self.adapter == DebugAdapterKind::Lldb {
            if let Some(frame_id) = snapshot.stack_frames.first().map(|frame| frame.id) {
                self.update_lldb_hit_counts_from_break_list(frame_id);
                snapshot.breakpoint_hits = self.collect_breakpoint_hits();
            }
        }
        Ok(snapshot)
    }

    fn snapshot_after_stop(&mut self, stopped: StoppedEventBody) -> Result<SessionSnapshot> {
        self.state = SessionState::Stopped {
            thread_id: stopped.thread_id,
            reason: stopped.reason.clone(),
        };
        self.active_thread = Some(stopped.thread_id);
        let mut snapshot = self.enrich_stopped_snapshot()?;
        if stopped.reason == "exception" && self.supports_exception_info_request {
            snapshot.exception_info = self.exception_info(stopped.thread_id).ok();
        }
        Ok(snapshot)
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
            breakpoint_hits: vec![],
            exception_info: None,
            debug_process_ids: vec![],
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
                                return Ok(Some(self.snapshot_after_stop(stopped)?));
                            }
                            "terminated" | "exited" => {
                                if Self::is_restart_event(&body) {
                                    continue;
                                }
                                self.clear_debug_process_ids();
                                self.state = SessionState::Exited;
                                return Ok(None);
                            }
                            "process" => {
                                self.handle_process_event(&body);
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
                            "breakpoint" => {
                                self.handle_breakpoint_event(&body);
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
                        return Ok(Some(self.snapshot_after_stop(stopped)?));
                    }
                    "terminated" | "exited" => {
                        if Self::is_restart_event(&body) {
                            continue;
                        }
                        self.clear_debug_process_ids();
                        self.state = SessionState::Exited;
                        return Ok(Some(self.empty_state_snapshot(SessionState::Exited)));
                    }
                    "process" => {
                        self.handle_process_event(&body);
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
                    "breakpoint" => {
                        self.handle_breakpoint_event(&body);
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
        let frame = preferred_stack_frame(&stack_frames).context("no stack frames available")?;
        let frame_id = frame.id;
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
            breakpoint_hits: vec![],
            exception_info: None,
            debug_process_ids: vec![],
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
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        Ok(())
    }

    pub fn dispatch_pause(&mut self) -> Result<()> {
        let seq = self.transport.send_request("pause", json!({}))?;
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
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
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
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
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
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
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct StepInTargetsBody {
            targets: Vec<StepInTarget>,
        }

        let parsed = serde_json::from_value::<StepInTargetsBody>(body).unwrap_or(StepInTargetsBody {
            targets: vec![],
        });
        Ok(parsed.targets)
    }

    pub fn supports_goto_targets(&self) -> bool {
        self.supports_goto_targets
    }

    pub fn goto_targets(
        &self,
        path: &str,
        line: i64,
        column: Option<i64>,
        source_reference: Option<i64>,
    ) -> Result<Vec<GotoTarget>> {
        let mut source = json!({});
        if !path.is_empty() {
            source["path"] = json!(path);
        }
        if let Some(source_reference) = source_reference.filter(|value| *value > 0) {
            source["sourceReference"] = json!(source_reference);
        }

        let mut args = json!({ "source": source, "line": line });
        if let Some(column) = column.filter(|value| *value > 0) {
            args["column"] = json!(column);
        }

        let seq = self.transport.send_request("gotoTargets", args)?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct GotoTargetsBody {
            targets: Vec<GotoTarget>,
        }

        let parsed = serde_json::from_value::<GotoTargetsBody>(body).unwrap_or(GotoTargetsBody {
            targets: vec![],
        });
        Ok(parsed.targets)
    }

    pub fn dispatch_goto(&mut self, target_id: i64) -> Result<()> {
        let thread_id = self
            .active_thread
            .context("cannot goto without an active thread")?;
        let seq = self.transport.send_request(
            "goto",
            json!({ "threadId": thread_id, "targetId": target_id }),
        )?;
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
        self.state = SessionState::Running;
        Ok(())
    }

    pub fn dispatch_goto_line(&mut self, path: &str, line: i64) -> Result<()> {
        match self.adapter {
            DebugAdapterKind::Lldb => {}
            DebugAdapterKind::Debugpy => bail!("jump to line is not supported for Python debug sessions"),
        }

        let frame_id = self.active_frame.context("no active frame")?;
        let expression = if path.is_empty() {
            format!("jump {line}")
        } else {
            format!("jump '{path}':{line}")
        };
        self.evaluate(&expression, frame_id, "repl")
            .with_context(|| format!("failed to jump to {path}:{line}"))?;
        Ok(())
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
            self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
            self.state = SessionState::Running;
        }
        self.poll_until_stopped()?
            .context("program exited during step")
    }

    fn threads(&self) -> Result<Vec<Thread>> {
        let seq = self.transport.send_request("threads", json!({}))?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

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
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

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
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

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
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct VariablesBody {
            variables: Vec<Variable>,
        }

        let parsed = serde_json::from_value::<VariablesBody>(body)?;
        Ok(parsed.variables)
    }

    /// Flatten empty container variables for display (locals, globals, etc.).
    pub fn variables_for_display(&self, variables_reference: i64) -> Result<Vec<Variable>> {
        self.variables_expanded(variables_reference, 0)
    }

    /// Show the useful subset of LLDB register groups (GPR / flags), not full SIMD dumps.
    pub fn variables_registers_display(&self, variables_reference: i64) -> Result<Vec<Variable>> {
        let groups = self.variables(variables_reference)?;
        let mut flattened = Vec::new();
        for group in groups {
            if group.variables_reference > 0 && group.value.trim().is_empty() {
                if is_heavy_register_group(&group.name) {
                    continue;
                }
                let children = self.variables(group.variables_reference)?;
                for child in children {
                    if !is_simd_register_name(&child.name) {
                        flattened.push(child);
                    }
                }
            } else if !is_simd_register_name(&group.name) {
                flattened.push(group);
            }
        }
        Ok(flattened)
    }

    pub fn variables_for_scope_display(
        &self,
        variables_reference: i64,
        scope_name: &str,
    ) -> Result<Vec<Variable>> {
        if scope_name == "Registers" || scope_name.starts_with("Register") {
            self.variables_registers_display(variables_reference)
        } else {
            self.variables_for_display(variables_reference)
        }
    }

    fn variables_expanded(&self, variables_reference: i64, depth: u32) -> Result<Vec<Variable>> {
        const MAX_DEPTH: u32 = 2;
        let variables = self.variables(variables_reference)?;
        if depth >= MAX_DEPTH {
            return Ok(variables);
        }

        let mut flattened = Vec::new();
        for var in variables {
            if var.variables_reference > 0 && var.value.trim().is_empty() {
                let children = self.variables_expanded(var.variables_reference, depth + 1)?;
                flattened.extend(children);
            } else {
                flattened.push(var);
            }
        }
        Ok(flattened)
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
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

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
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct SourceBody {
            content: String,
        }

        let parsed = serde_json::from_value::<SourceBody>(body).context("invalid source response")?;
        Ok(parsed.content)
    }

    /// Read raw memory via DAP `readMemory`.
    pub fn read_memory(&self, memory_reference: &str, offset: i64, count: i64) -> Result<ReadMemoryResult> {
        if !self.supports_read_memory_request {
            bail!("debug adapter does not support readMemory");
        }
        if memory_reference.is_empty() {
            bail!("memory reference must not be empty");
        }
        if count <= 0 {
            bail!("readMemory count must be positive");
        }

        let seq = self.transport.send_request(
            "readMemory",
            json!({
                "memoryReference": memory_reference,
                "offset": offset,
                "count": count,
            }),
        )?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct ReadMemoryBody {
            address: Option<String>,
            data: String,
            #[serde(rename = "unreadableBytes")]
            unreadable_bytes: Option<i64>,
        }

        let parsed = serde_json::from_value::<ReadMemoryBody>(body).context("invalid readMemory response")?;
        let bytes = base64::Engine::decode(&base64::engine::general_purpose::STANDARD, parsed.data.as_bytes())
            .context("invalid base64 in readMemory response")?;
        Ok(ReadMemoryResult {
            address: parsed.address,
            data: bytes_to_hex(&bytes),
            unreadable_bytes: parsed.unreadable_bytes,
        })
    }

    /// Write raw memory via DAP `writeMemory`. `data` is lowercase hex (two digits per byte).
    pub fn write_memory(&self, memory_reference: &str, offset: i64, data: &str) -> Result<WriteMemoryResult> {
        if !self.supports_write_memory_request {
            bail!("debug adapter does not support writeMemory");
        }
        if memory_reference.is_empty() {
            bail!("memory reference must not be empty");
        }

        let bytes = hex_to_bytes(data)?;
        let encoded = base64::Engine::encode(&base64::engine::general_purpose::STANDARD, &bytes);
        let seq = self.transport.send_request(
            "writeMemory",
            json!({
                "memoryReference": memory_reference,
                "offset": offset,
                "data": encoded,
                "allowPartial": true,
            }),
        )?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
        serde_json::from_value::<WriteMemoryResult>(body).context("invalid writeMemory response")
    }

    /// Disassemble instructions via DAP `disassemble`.
    pub fn disassemble(
        &self,
        memory_reference: &str,
        instruction_offset: i64,
        offset: i64,
        instruction_count: i64,
    ) -> Result<Vec<DisassembledInstruction>> {
        if !self.supports_disassemble_request {
            bail!("debug adapter does not support disassemble");
        }
        if memory_reference.is_empty() {
            bail!("memory reference must not be empty");
        }
        if instruction_count <= 0 {
            bail!("disassemble instructionCount must be positive");
        }

        let seq = self.transport.send_request(
            "disassemble",
            json!({
                "memoryReference": memory_reference,
                "instructionOffset": instruction_offset,
                "offset": offset,
                "instructionCount": instruction_count,
            }),
        )?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct DisassembleBody {
            instructions: Vec<DisassembledInstruction>,
        }

        let parsed =
            serde_json::from_value::<DisassembleBody>(body).context("invalid disassemble response")?;
        Ok(parsed.instructions)
    }

    /// Push UI breakpoints for one source file to the debug adapter.
    pub fn set_source_breakpoints(
        &mut self,
        path: &std::path::Path,
        breakpoints: &[SourceBreakpoint],
    ) -> Result<Vec<SourceBreakpointResult>> {
        let path_key = path.to_string_lossy().to_string();
        let request_lines: Vec<u32> = breakpoints.iter().map(|breakpoint| breakpoint.line).collect();
        self.last_breakpoint_requests
            .insert(path_key.clone(), breakpoints.to_vec());

        let breakpoints: Vec<serde_json::Value> = breakpoints
            .iter()
            .map(|breakpoint| {
                let mut payload = json!({ "line": breakpoint.line });
                if let Some(condition) = &breakpoint.condition {
                    if !condition.is_empty() {
                        payload["condition"] = json!(condition);
                    }
                }
                if let Some(hit_condition) = &breakpoint.hit_condition {
                    if !hit_condition.is_empty() {
                        payload["hitCondition"] = json!(hit_condition);
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
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct BpResponse {
            breakpoints: Vec<BpInfo>,
        }
        #[derive(Deserialize)]
        struct BpInfo {
            id: Option<i64>,
            line: u32,
            verified: bool,
            #[serde(rename = "hitCount")]
            hit_count: Option<u32>,
        }

        let parsed = serde_json::from_value::<BpResponse>(body)?;
        let results = parsed
            .breakpoints
            .into_iter()
            .map(|bp| SourceBreakpointResult {
                id: bp.id,
                line: bp.line,
                verified: bp.verified,
                hit_count: bp.hit_count,
            })
            .collect::<Vec<_>>();
        self.apply_source_breakpoint_results(&path_key, &request_lines, &results);
        Ok(results)
    }

    /// Resolve a variable to a DAP `dataId` for `setDataBreakpoints`.
    /// Fetch structured exception details for the thread that raised `stopped`.
    pub fn exception_info(&self, thread_id: i64) -> Result<ExceptionInfo> {
        if !self.supports_exception_info_request {
            bail!("debug adapter does not support exceptionInfo");
        }
        if !matches!(self.state, SessionState::Stopped { .. }) {
            bail!("cannot query exception info while program is running");
        }

        let seq = self.transport.send_request(
            "exceptionInfo",
            json!({
                "threadId": thread_id,
            }),
        )?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
        serde_json::from_value::<ExceptionInfo>(body).context("invalid exceptionInfo response")
    }

    pub fn data_breakpoint_info(
        &mut self,
        variables_reference: i64,
        frame_id: i64,
        name: Option<&str>,
    ) -> Result<DataBreakpointInfoResult> {
        if !self.supports_data_breakpoints {
            bail!("debug adapter does not support data breakpoints");
        }

        let mut args = json!({
            "variablesReference": variables_reference,
            "frameId": frame_id,
        });
        if let Some(name) = name.filter(|value| !value.is_empty()) {
            args["name"] = json!(name);
        }

        let seq = self.transport.send_request("dataBreakpointInfo", args)?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct InfoBody {
            #[serde(rename = "dataId")]
            data_id: Option<String>,
            description: Option<String>,
            #[serde(rename = "accessTypes", default)]
            access_types: Vec<String>,
        }

        let parsed = serde_json::from_value::<InfoBody>(body).context("invalid dataBreakpointInfo response")?;
        let data_id = parsed
            .data_id
            .filter(|value| !value.is_empty())
            .context("dataBreakpointInfo returned no dataId")?;

        Ok(DataBreakpointInfoResult {
            data_id,
            description: parsed.description,
            access_types: parsed.access_types,
        })
    }

    /// Push UI data breakpoints to the debug adapter.
    pub fn set_data_breakpoints(&mut self, breakpoints: &[DataBreakpoint]) -> Result<Vec<DataBreakpointResult>> {
        if !self.supports_data_breakpoints {
            bail!("debug adapter does not support data breakpoints");
        }

        let payload: Vec<serde_json::Value> = breakpoints
            .iter()
            .map(|breakpoint| {
                let mut entry = json!({
                    "dataId": breakpoint.data_id,
                    "accessType": breakpoint.access_type,
                });
                if let Some(condition) = &breakpoint.condition {
                    if !condition.is_empty() {
                        entry["condition"] = json!(condition);
                    }
                }
                entry
            })
            .collect();

        let seq = self
            .transport
            .send_request("setDataBreakpoints", json!({ "breakpoints": payload }))?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct BpResponse {
            breakpoints: Vec<BpInfo>,
        }
        #[derive(Deserialize)]
        struct BpInfo {
            verified: Option<bool>,
            message: Option<String>,
        }

        let parsed = serde_json::from_value::<BpResponse>(body).context("invalid setDataBreakpoints response")?;
        Ok(parsed
            .breakpoints
            .into_iter()
            .enumerate()
            .map(|(index, bp)| {
                let data_id = breakpoints
                    .get(index)
                    .map(|entry| entry.data_id.clone())
                    .unwrap_or_default();
                DataBreakpointResult {
                    data_id,
                    verified: bp.verified.unwrap_or(false),
                    message: bp.message,
                }
            })
            .collect())
    }

    /// Build the DAP `setExceptionBreakpoints` request body.
    pub fn build_exception_breakpoints_request(settings: &[ExceptionBreakpointSetting]) -> Value {
        let filters: Vec<String> = settings
            .iter()
            .map(|setting| setting.filter.clone())
            .collect();
        let filter_options: Vec<Value> = settings
            .iter()
            .filter_map(|setting| {
                setting
                    .condition
                    .as_ref()
                    .map(|condition| condition.trim())
                    .filter(|condition| !condition.is_empty())
                    .map(|condition| {
                        json!({
                            "filterId": setting.filter,
                            "condition": condition,
                        })
                    })
            })
            .collect();

        let mut payload = json!({ "filters": filters });
        if !filter_options.is_empty() {
            payload["filterOptions"] = Value::Array(filter_options);
        }
        payload
    }

    /// Configure which exception breakpoint filters are active.
    pub fn set_exception_breakpoints(&mut self, settings: &[ExceptionBreakpointSetting]) -> Result<()> {
        if self.exception_breakpoint_filters.is_empty() {
            bail!("debug adapter does not advertise exception breakpoint filters");
        }

        let payload = Self::build_exception_breakpoints_request(settings);
        let seq = self
            .transport
            .send_request("setExceptionBreakpoints", payload)?;
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
        Ok(())
    }

    /// Push UI function breakpoints to the debug adapter.
    pub fn set_function_breakpoints(
        &mut self,
        breakpoints: &[FunctionBreakpoint],
    ) -> Result<Vec<FunctionBreakpointResult>> {
        if !self.supports_function_breakpoints && self.adapter != DebugAdapterKind::Lldb {
            bail!("debug adapter does not support function breakpoints");
        }

        let payload: Vec<serde_json::Value> = breakpoints
            .iter()
            .map(|breakpoint| {
                let mut entry = json!({ "name": breakpoint.name });
                if let Some(condition) = &breakpoint.condition {
                    if !condition.is_empty() {
                        entry["condition"] = json!(condition);
                    }
                }
                if let Some(hit_condition) = &breakpoint.hit_condition {
                    if !hit_condition.is_empty() {
                        entry["hitCondition"] = json!(hit_condition);
                    }
                }
                entry
            })
            .collect();

        let seq = self
            .transport
            .send_request("setFunctionBreakpoints", json!({ "breakpoints": payload }))?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct BpResponse {
            breakpoints: Vec<BpInfo>,
        }
        #[derive(Deserialize)]
        struct BpInfo {
            verified: Option<bool>,
            message: Option<String>,
        }

        let parsed =
            serde_json::from_value::<BpResponse>(body).context("invalid setFunctionBreakpoints response")?;
        Ok(parsed
            .breakpoints
            .into_iter()
            .enumerate()
            .map(|(index, bp)| {
                let name = breakpoints
                    .get(index)
                    .map(|entry| entry.name.clone())
                    .unwrap_or_default();
                FunctionBreakpointResult {
                    name,
                    verified: bp.verified.unwrap_or(false),
                    message: bp.message,
                }
            })
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
        let program_args = self.program_args.clone();
        let adapter = self.adapter;
        let _ = self.shutdown();
        *self = match adapter {
            DebugAdapterKind::Debugpy => Self::launch_python_with_args(&program, &program_args)?,
            DebugAdapterKind::Lldb => Self::launch_native_with_args(&program, &program_args)?,
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
        let body = self.wait_dap_response(seq, timeout)?;

        #[derive(Deserialize)]
        struct EvalBody {
            result: String,
        }

        let parsed = serde_json::from_value::<EvalBody>(body)?;
        Ok(parsed.result)
    }

    pub fn completions(&self, text: &str, column: i64, frame_id: i64) -> Result<Vec<CompletionItem>> {
        if !self.supports_completions_request {
            return Ok(vec![]);
        }
        if !matches!(self.state, SessionState::Stopped { .. }) {
            bail!("cannot complete while program is running");
        }

        let seq = self.transport.send_request(
            "completions",
            json!({
                "frameId": frame_id,
                "text": text,
                "column": column,
            }),
        )?;
        let body = self.wait_dap_response(seq, REQUEST_TIMEOUT)?;

        #[derive(Deserialize)]
        struct CompletionsBody {
            targets: Vec<CompletionTarget>,
        }

        #[derive(Deserialize)]
        struct CompletionTarget {
            label: String,
            #[serde(rename = "sortText", default)]
            sort_text: Option<String>,
            #[serde(default)]
            detail: Option<String>,
            #[serde(rename = "selectionStart", default)]
            selection_start: Option<i64>,
            #[serde(rename = "selectionLength", default)]
            selection_length: Option<i64>,
            #[serde(default)]
            start: Option<i64>,
            #[serde(default)]
            length: Option<i64>,
        }

        let parsed = serde_json::from_value::<CompletionsBody>(body)?;
        Ok(parsed
            .targets
            .into_iter()
            .map(|target| {
                let selection_start = target
                    .selection_start
                    .or(target.start)
                    .unwrap_or(0)
                    .max(0) as usize;
                let selection_length = target
                    .selection_length
                    .or(target.length)
                    .unwrap_or(0)
                    .max(0) as usize;
                CompletionItem {
                    label: target.label,
                    sort_text: target.sort_text,
                    detail: target.detail,
                    selection_start,
                    selection_length,
                }
            })
            .collect())
    }

    pub fn terminate(&mut self) -> Result<()> {
        if matches!(self.state, SessionState::Disconnected | SessionState::Exited) {
            self.state = SessionState::Exited;
            self.active_thread = None;
            self.active_frame = None;
            return Ok(());
        }

        let seq = self.transport.send_request("terminate", json!({}))?;
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
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

    pub fn network_proxy_address(&self) -> Option<String> {
        self.network_capture
            .as_ref()
            .map(|capture| capture.proxy_address().to_string())
    }

    pub fn drain_network_json(&mut self) -> Result<String> {
        let Some(capture) = &self.network_capture else {
            return Ok(String::new());
        };
        let drain = capture.drain();
        if drain.exchanges.is_empty() {
            return Ok(String::new());
        }
        serde_json::to_string(&drain).context("failed to serialize network capture drain")
    }

    pub fn send_network_compose_json(
        &mut self,
        method: &str,
        url: &str,
        headers: &str,
        body: &str,
        timeout_ms: i32,
    ) -> Result<String> {
        let exchange =
            crate::network::send_compose_request(None, method, url, headers, body, timeout_ms)?;
        let drain = crate::network::NetworkDrain {
            proxy_address: String::new(),
            intercept_enabled: false,
            exchanges: vec![exchange],
        };
        serde_json::to_string(&drain).context("failed to serialize compose network result")
    }

    /// Buffered program stdout/stderr from DAP output events and the debuggee PTY.
    pub fn drain_console_output(&self) -> Vec<OutputEventBody> {
        let mut output = self.transport.take_output();
        let guard = self
            .debuggee_io
            .lock()
            .expect("debuggee io lock poisoned");
        if let Some(io) = guard.as_ref() {
            output.extend(io.take_output());
        }
        output
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
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
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
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
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
        self.wait_dap_response(seq, REQUEST_TIMEOUT)?;
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

fn handle_adapter_request(
    message: InboundMessage,
    debuggee_io: &Arc<Mutex<Option<DebuggeeIo>>>,
    transport: &DapTransport,
) -> Result<()> {
    let InboundMessage::Request { seq, command, arguments } = message else {
        return Ok(());
    };

    if command == "runInTerminal" {
        let (args, cwd, env) = parse_run_in_terminal(&arguments)?;
        info!("runInTerminal: args={:?} cwd={:?}", args, cwd);
        let spawned = DebuggeeTerminal::spawn(&args, cwd.as_deref(), &env)
            .with_context(|| format!("failed to spawn runInTerminal: {:?}", args))?;
        let mut guard = debuggee_io.lock().expect("debuggee io lock poisoned");
        *guard = Some(DebuggeeIo::Terminal(spawned));
        transport.send_response(seq, &command, true, json!({}))?;
        return Ok(());
    }

    warn!("unhandled DAP adapter request: {command}");
    transport.send_response(seq, &command, false, json!({}))?;
    Ok(())
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
                    hit_condition: None,
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
                    hit_condition: None,
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
mod breakpoint_hit_count_tests {
    use super::*;

    #[test]
    fn parse_lldb_breakpoint_list_extracts_hit_counts() {
        let output = r#"Current breakpoints:
1: file = '/tmp/reverse_demo.c', line = 15, exact_match = 0, locations = 1, resolved = 1, hit count = 3
2: file = '/tmp/reverse_demo.c', line = 22, exact_match = 0, locations = 1, resolved = 1, hit count = 1
"#;
        let parsed = parse_lldb_breakpoint_list(output);
        assert_eq!(
            parsed,
            vec![
                ("/tmp/reverse_demo.c".into(), 15, 3),
                ("/tmp/reverse_demo.c".into(), 22, 1),
            ]
        );
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

    #[test]
    fn launch_native_interactive_demo_accepts_fifo_stdin() {
        let program = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
            .join("../../fixtures/interactive_demo")
            .canonicalize()
            .expect("interactive_demo fixture");

        let mut session = DebugSession::launch_native(&program).expect("lldb launch");
        session.dispatch_continue().expect("continue from main");

        let deadline = std::time::Instant::now() + Duration::from_secs(10);
        let mut saw_prompt = false;
        while std::time::Instant::now() < deadline {
            let _ = session.poll_events();
            for line in session.drain_console_output() {
                if line.output.contains("Your name:") {
                    saw_prompt = true;
                    break;
                }
            }
            if saw_prompt {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        assert!(saw_prompt, "expected program prompt on console output");

        session
            .terminal_write_input(b"Alice\n")
            .expect("write stdin fifo");

        let mut saw_followup = false;
        while std::time::Instant::now() < deadline {
            let _ = session.poll_events();
            for line in session.drain_console_output() {
                if line.output.contains("How many greetings") {
                    saw_followup = true;
                    break;
                }
            }
            if saw_followup {
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }

        session.shutdown().expect("shutdown");
        assert!(
            saw_followup,
            "expected fgets to accept fifo stdin and continue to next prompt"
        );
    }
}

#[cfg(test)]
mod exception_breakpoint_request_tests {
    use super::*;
    use serde_json::json;

    #[test]
    fn build_exception_breakpoints_request_without_conditions() {
        let payload = DebugSession::build_exception_breakpoints_request(&[
            ExceptionBreakpointSetting {
                filter: "cxx-throw".to_string(),
                condition: None,
            },
            ExceptionBreakpointSetting {
                filter: "cxx-catch".to_string(),
                condition: None,
            },
        ]);
        assert_eq!(
            payload,
            json!({
                "filters": ["cxx-throw", "cxx-catch"]
            })
        );
    }

    #[test]
    fn build_exception_breakpoints_request_with_filter_options() {
        let payload = DebugSession::build_exception_breakpoints_request(&[
            ExceptionBreakpointSetting {
                filter: "cxx-throw".to_string(),
                condition: Some("std::runtime_error".to_string()),
            },
        ]);
        assert_eq!(
            payload,
            json!({
                "filters": ["cxx-throw"],
                "filterOptions": [{
                    "filterId": "cxx-throw",
                    "condition": "std::runtime_error"
                }]
            })
        );
    }
}
