use std::collections::{HashMap, VecDeque};
use std::io::{BufReader, BufWriter};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use anyhow::{bail, Context, Result};
use crossbeam_channel::{unbounded, Receiver};
use serde_json::{json, Value};
use tracing::{debug, error, warn};

use crate::dap::protocol::Request;
use crate::dap::protocol::{InboundMessage, OutputEventBody};
use crate::dap::transport;

static SEQ: AtomicI64 = AtomicI64::new(1);

fn next_seq() -> i64 {
    SEQ.fetch_add(1, Ordering::SeqCst)
}

#[derive(Debug, Clone)]
enum PendingResponse {
    Success(Value),
    Failure { command: String, message: String },
}

/// Spawn a DAP adapter process over stdio.
pub fn spawn_adapter(command: &str, args: &[String]) -> Result<Child> {
    let mut process = Command::new(command);
    if !args.is_empty() {
        process.args(args);
    }
    process.stdin(Stdio::piped()).stdout(Stdio::piped()).stderr(Stdio::null());

    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        unsafe {
            process.pre_exec(|| {
                if libc::setsid() == -1 {
                    return Err(std::io::Error::last_os_error());
                }
                Ok(())
            });
        }
    }

    let child = process.spawn().with_context(|| format!("failed to spawn adapter '{command}'"))?;
    Ok(child)
}

/// Spawn the `lldb-dap` binary and return the child process.
pub fn spawn_lldb_dap_adapter() -> Result<Child> {
    spawn_adapter("lldb-dap", &[]).context(
        "failed to spawn lldb-dap — install the LLVM lldb package (provides /usr/bin/lldb-dap)",
    )
}

/// Spawn `python3 -m debugpy.adapter` and return the child process.
pub fn spawn_debugpy_adapter() -> Result<Child> {
    spawn_adapter("python3", &["-u".to_string(), "-m".to_string(), "debugpy.adapter".to_string()])
        .context("failed to spawn debugpy adapter — install with: python3 -m pip install debugpy")
}

/// Low-level DAP transport bound to a child process stdio.
pub struct DapTransport {
    writer: Mutex<BufWriter<ChildStdin>>,
    pending: Arc<Mutex<HashMap<i64, PendingResponse>>>,
    output_buffer: Arc<Mutex<Vec<OutputEventBody>>>,
    event_backlog: Arc<Mutex<VecDeque<InboundMessage>>>,
    inbound: Receiver<InboundMessage>,
    reader_thread: Option<thread::JoinHandle<()>>,
    child: Child,
    shut_down: bool,
}

impl DapTransport {
    pub fn spawn(mut child: Child) -> Result<Self> {
        let stdin = child.stdin.take().context("debugpy adapter missing stdin")?;
        let stdout = child.stdout.take().context("debugpy adapter missing stdout")?;

        let writer = Mutex::new(BufWriter::new(stdin));
        let pending: Arc<Mutex<HashMap<i64, PendingResponse>>> =
            Arc::new(Mutex::new(HashMap::new()));
        let output_buffer: Arc<Mutex<Vec<OutputEventBody>>> = Arc::new(Mutex::new(Vec::new()));
        let (inbound_tx, inbound_rx) = unbounded();

        let pending_reader = Arc::clone(&pending);
        let reader_thread = thread::spawn(move || {
            let mut reader = BufReader::new(stdout);
            loop {
                match transport::read_message(&mut reader) {
                    Ok(value) => {
                        debug!(target: "dap", "recv: {}", value);
                        match InboundMessage::parse(value.clone()) {
                            Ok(InboundMessage::Response {
                                request_seq,
                                success,
                                command,
                                body,
                                message,
                            }) => {
                                let response = if success {
                                    PendingResponse::Success(body)
                                } else {
                                    let message = message.unwrap_or_default();
                                    error!("DAP response error for {}: {}", command, message);
                                    PendingResponse::Failure { command, message }
                                };
                                pending_reader
                                    .lock()
                                    .expect("pending lock poisoned")
                                    .insert(request_seq, response);
                            }
                            Ok(
                                event @ (InboundMessage::Event { .. }
                                | InboundMessage::Request { .. }),
                            ) => {
                                if inbound_tx.send(event).is_err() {
                                    break;
                                }
                            }
                            Err(err) => {
                                warn!("failed to parse inbound DAP message: {err:#}");
                            }
                        }
                    }
                    Err(err) => {
                        debug!("DAP reader exiting: {err:#}");
                        break;
                    }
                }
            }
        });

        Ok(Self {
            writer,
            pending,
            output_buffer,
            event_backlog: Arc::new(Mutex::new(VecDeque::new())),
            inbound: inbound_rx,
            reader_thread: Some(reader_thread),
            child,
            shut_down: false,
        })
    }

    fn pop_queued_event(&self) -> Option<InboundMessage> {
        self.event_backlog.lock().expect("event backlog lock poisoned").pop_front()
    }

    fn take_queued_event(&self, event_name: &str) -> Option<Value> {
        let mut backlog = self.event_backlog.lock().expect("event backlog lock poisoned");
        let index = backlog.iter().position(|message| {
            matches!(
                message,
                InboundMessage::Event { event, .. } if event == event_name
            )
        })?;
        let message = backlog.remove(index)?;
        match message {
            InboundMessage::Event { body, .. } => Some(body),
            _ => None,
        }
    }

    fn enqueue_event(&self, message: InboundMessage) {
        if let InboundMessage::Event { event, body } = &message {
            debug!("queued event: {event}");
            if event == "output" {
                if let Ok(output) = serde_json::from_value::<OutputEventBody>(body.clone()) {
                    self.output_buffer.lock().expect("output buffer lock poisoned").push(output);
                }
            }
        }
        self.event_backlog.lock().expect("event backlog lock poisoned").push_back(message);
    }

    pub fn send_response(
        &self,
        request_seq: i64,
        command: &str,
        success: bool,
        body: Value,
    ) -> Result<()> {
        let seq = next_seq();
        let payload = json!({
            "seq": seq,
            "type": "response",
            "request_seq": request_seq,
            "success": success,
            "command": command,
            "body": body,
        });
        debug!(target: "dap", "send response: {}", payload);
        let mut writer = self.writer.lock().expect("writer lock poisoned");
        transport::write_message(&mut *writer, &payload)?;
        Ok(())
    }

    pub fn try_take_response(&self, request_seq: i64) -> Option<Result<Value>> {
        self.pending.lock().expect("pending lock poisoned").remove(&request_seq).map(|response| {
            match response {
                PendingResponse::Success(body) => Ok(body),
                PendingResponse::Failure { command, message } => {
                    bail!("DAP {command} failed: {message}");
                }
            }
        })
    }

    pub fn send_request(&self, command: &'static str, arguments: Value) -> Result<i64> {
        let seq = next_seq();
        let request = Request { seq, message_type: "request", command, arguments };

        let payload = serde_json::to_value(&request)?;
        debug!(target: "dap", "send: {}", payload);

        let mut writer = self.writer.lock().expect("writer lock poisoned");
        transport::write_message(&mut *writer, &payload)?;
        Ok(seq)
    }

    pub fn wait_response<F>(
        &self,
        request_seq: i64,
        timeout: Duration,
        mut on_adapter_request: F,
    ) -> Result<Value>
    where
        F: FnMut(InboundMessage) -> Result<()>,
    {
        let deadline = std::time::Instant::now() + timeout;

        while std::time::Instant::now() < deadline {
            if let Some(response) = self.try_take_response(request_seq) {
                return response;
            }

            if let Some(message) = self.try_recv_event() {
                if matches!(message, InboundMessage::Request { .. }) {
                    on_adapter_request(message)?;
                    continue;
                }
                self.enqueue_event(message);
            }

            thread::sleep(Duration::from_millis(2));
        }

        bail!("timed out waiting for DAP response to request {request_seq}");
    }

    pub fn try_recv_event(&self) -> Option<InboundMessage> {
        if let Some(message) = self.pop_queued_event() {
            return Some(message);
        }
        self.inbound.try_recv().ok()
    }

    pub fn recv_event(&self, timeout: Duration) -> Option<InboundMessage> {
        if let Some(message) = self.pop_queued_event() {
            return Some(message);
        }
        match self.inbound.recv_timeout(timeout) {
            Ok(message) => Some(message),
            Err(_) => None,
        }
    }

    /// Take buffered DAP output events for the console panel.
    pub fn take_output(&self) -> Vec<OutputEventBody> {
        std::mem::take(&mut *self.output_buffer.lock().expect("output buffer lock poisoned"))
    }

    pub fn push_output(&self, output: OutputEventBody) {
        self.output_buffer.lock().expect("output buffer lock poisoned").push(output);
    }

    pub fn wait_for_event(&self, event_name: &str, timeout: Duration) -> Result<Option<Value>> {
        let deadline = std::time::Instant::now() + timeout;

        while std::time::Instant::now() < deadline {
            if let Some(body) = self.take_queued_event(event_name) {
                return Ok(Some(body));
            }

            match self.inbound.recv_timeout(Duration::from_millis(50)) {
                Ok(InboundMessage::Event { event, body }) => {
                    if event == event_name {
                        return Ok(Some(body));
                    }
                    self.enqueue_event(InboundMessage::Event { event, body });
                }
                Ok(_) => {}
                Err(_) => {}
            }
        }

        Ok(None)
    }

    pub fn shutdown(&mut self) -> Result<()> {
        if self.shut_down {
            return Ok(());
        }
        self.shut_down = true;

        let _ = self.send_request("disconnect", json!({ "terminateDebuggee": true }));
        // Kill the adapter so stdout closes and the reader thread can exit.
        let _ = self.child.kill();
        if let Some(handle) = self.reader_thread.take() {
            let _ = handle.join();
        }
        let _ = self.child.wait();
        Ok(())
    }
}
