use std::collections::{HashMap, VecDeque};
use std::io::{BufReader, BufWriter};
use std::process::{Child, ChildStdin, Command, Stdio};
use std::sync::atomic::{AtomicI64, Ordering};
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result, bail};
use crossbeam_channel::{Receiver, unbounded};
use serde_json::{Value, json};
use tracing::{debug, error, warn};

use crate::dap::protocol::{InboundMessage, OutputEventBody};
use crate::dap::protocol::Request;
use crate::dap::transport;

static SEQ: AtomicI64 = AtomicI64::new(1);

fn next_seq() -> i64 {
    SEQ.fetch_add(1, Ordering::SeqCst)
}

/// Spawn `python3 -m debugpy.adapter` and return the child process.
pub fn spawn_debugpy_adapter() -> Result<Child> {
    let mut command = Command::new("python3");
    command
        .args(["-u", "-m", "debugpy.adapter"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null());

    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;
        unsafe {
            command.pre_exec(|| {
                // Detach debugpy from the controlling terminal entirely so ^C / keyboard
                // events routed to the TUI never become KeyboardInterrupt in pydevd.
                if libc::setsid() == -1 {
                    return Err(std::io::Error::last_os_error());
                }
                Ok(())
            });
        }
    }

    let child = command.spawn().context(
        "failed to spawn debugpy adapter — install with: python3 -m pip install debugpy",
    )?;

    Ok(child)
}

/// Low-level DAP transport bound to a child process stdio.
pub struct DapTransport {
    writer: Mutex<BufWriter<ChildStdin>>,
    pending: Arc<Mutex<HashMap<i64, Value>>>,
    output_buffer: Arc<Mutex<Vec<OutputEventBody>>>,
    event_backlog: Arc<Mutex<VecDeque<InboundMessage>>>,
    inbound: Receiver<InboundMessage>,
    reader_thread: Option<thread::JoinHandle<()>>,
    child: Child,
    shut_down: bool,
}

impl DapTransport {
    pub fn spawn(mut child: Child) -> Result<Self> {
        let stdin = child
            .stdin
            .take()
            .context("debugpy adapter missing stdin")?;
        let stdout = child
            .stdout
            .take()
            .context("debugpy adapter missing stdout")?;

        let writer = Mutex::new(BufWriter::new(stdin));
        let pending: Arc<Mutex<HashMap<i64, Value>>> = Arc::new(Mutex::new(HashMap::new()));
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
                                if success {
                                    pending_reader
                                        .lock()
                                        .expect("pending lock poisoned")
                                        .insert(request_seq, body);
                                } else {
                                    error!(
                                        "DAP response error for {}: {}",
                                        command,
                                        message.unwrap_or_default()
                                    );
                                }
                            }
                            Ok(event) => {
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
        self.event_backlog
            .lock()
            .expect("event backlog lock poisoned")
            .pop_front()
    }

    fn take_queued_event(&self, event_name: &str) -> Option<Value> {
        let mut backlog = self
            .event_backlog
            .lock()
            .expect("event backlog lock poisoned");
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
                    self.output_buffer
                        .lock()
                        .expect("output buffer lock poisoned")
                        .push(output);
                }
            }
        }
        self.event_backlog
            .lock()
            .expect("event backlog lock poisoned")
            .push_back(message);
    }

    pub fn send_request(&self, command: &'static str, arguments: Value) -> Result<i64> {
        let seq = next_seq();
        let request = Request {
            seq,
            message_type: "request",
            command,
            arguments,
        };

        let payload = serde_json::to_value(&request)?;
        debug!(target: "dap", "send: {}", payload);

        let mut writer = self.writer.lock().expect("writer lock poisoned");
        transport::write_message(&mut *writer, &payload)?;
        Ok(seq)
    }

    pub fn wait_response(&self, request_seq: i64, timeout: Duration) -> Result<Value> {
        let deadline = std::time::Instant::now() + timeout;

        while std::time::Instant::now() < deadline {
            if let Some(body) = self
                .pending
                .lock()
                .expect("pending lock poisoned")
                .remove(&request_seq)
            {
                return Ok(body);
            }

            // Do not drain inbound events here — they must survive until the
            // session loop handles stopped/terminated/exited.
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
        std::mem::take(
            &mut *self
                .output_buffer
                .lock()
                .expect("output buffer lock poisoned"),
        )
    }

    pub fn push_output(&self, output: OutputEventBody) {
        self.output_buffer
            .lock()
            .expect("output buffer lock poisoned")
            .push(output);
    }

    pub fn wait_for_event(
        &self,
        event_name: &str,
        timeout: Duration,
    ) -> Result<Option<Value>> {
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
