//! Host-side pseudo-terminal for DAP `runInTerminal` and lldb stdin FIFO redirection.

use std::ffi::CString;
use std::fs::{remove_file, File};
use std::io::{Read, Write};
use std::os::fd::{AsRawFd, FromRawFd};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime, UNIX_EPOCH};

use anyhow::{bail, Context, Result};
use serde_json::Value;
use tracing::{debug, warn};

use crate::dap::protocol::OutputEventBody;

use libc::{self, winsize};

/// Active debuggee I/O channel (PTY child or lldb stdin FIFO).
pub enum DebuggeeIo {
    Terminal(DebuggeeTerminal),
    LldbStdio(LldbStdioChannels),
}

impl DebuggeeIo {
    pub fn write_input(&mut self, bytes: &[u8]) -> Result<()> {
        match self {
            Self::Terminal(terminal) => terminal.write_input(bytes),
            Self::LldbStdio(channels) => channels.write_input(bytes),
        }
    }

    pub fn resize(&self, rows: u16, cols: u16) {
        match self {
            Self::Terminal(terminal) => terminal.resize(rows, cols),
            Self::LldbStdio(channels) => channels.resize(rows, cols),
        }
    }

    pub fn take_output(&self) -> Vec<OutputEventBody> {
        match self {
            Self::Terminal(terminal) => terminal.take_output(),
            Self::LldbStdio(channels) => channels.take_output(),
        }
    }
}

/// lldb stdio redirection: FIFO for stdin; stdout/stderr stay on lldb's default terminal.
pub struct LldbStdioChannels {
    stdin_fifo: File,
    stdin_fifo_path: PathBuf,
}

impl LldbStdioChannels {
    pub fn open() -> Result<Self> {
        let unique =
            SystemTime::now().duration_since(UNIX_EPOCH).map(|value| value.as_nanos()).unwrap_or(0);
        let stdin_fifo_path = std::env::temp_dir().join(format!("daptor-stdin-{unique}"));
        let fifo_c = CString::new(stdin_fifo_path.to_string_lossy().as_ref())
            .context("invalid fifo path")?;
        if unsafe { libc::mkfifo(fifo_c.as_ptr(), 0o600) } != 0 {
            bail!("mkfifo failed: {}", std::io::Error::last_os_error());
        }

        let fd = unsafe { libc::open(fifo_c.as_ptr(), libc::O_RDWR | libc::O_NONBLOCK) };
        if fd < 0 {
            let _ = remove_file(&stdin_fifo_path);
            bail!("failed to open stdin fifo: {}", std::io::Error::last_os_error());
        }
        let stdin_fifo = unsafe { File::from_raw_fd(fd) };

        debug!("lldb stdin fifo: {}", stdin_fifo_path.display());

        Ok(Self { stdin_fifo, stdin_fifo_path })
    }

    pub fn stdin_path(&self) -> String {
        self.stdin_fifo_path.to_string_lossy().into_owned()
    }

    pub fn write_input(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.is_empty() {
            return Ok(());
        }

        let normalized: Vec<u8> =
            bytes.iter().map(|byte| if *byte == b'\r' { b'\n' } else { *byte }).collect();
        let bytes = normalized.as_slice();

        let mut offset = 0usize;
        while offset < bytes.len() {
            match self.stdin_fifo.write(&bytes[offset..]) {
                Ok(0) => std::thread::sleep(Duration::from_millis(1)),
                Ok(count) => offset += count,
                Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {
                    std::thread::sleep(Duration::from_millis(1));
                }
                Err(err) => {
                    bail!("failed to write to debuggee stdin fifo: {err}");
                }
            }
        }
        self.stdin_fifo.flush().ok();
        Ok(())
    }

    pub fn resize(&self, _rows: u16, _cols: u16) {}

    pub fn take_output(&self) -> Vec<OutputEventBody> {
        Vec::new()
    }
}

impl Drop for LldbStdioChannels {
    fn drop(&mut self) {
        let _ = remove_file(&self.stdin_fifo_path);
    }
}

/// Active debuggee terminal session (PTY master end).
pub struct DebuggeeTerminal {
    master: File,
    child_pid: libc::pid_t,
    reader: Option<JoinHandle<()>>,
    output_sink: Arc<Mutex<Vec<OutputEventBody>>>,
}

impl DebuggeeTerminal {
    pub fn spawn(args: &[String], cwd: Option<&str>, env: &[(String, String)]) -> Result<Self> {
        Self::spawn_internal(args, cwd, env, false)
    }

    /// Spawn a debuggee in a PTY but stop it with `SIGSTOP` before `exec` so lldb can attach.
    pub fn spawn_stopped(
        args: &[String],
        cwd: Option<&str>,
        env: &[(String, String)],
    ) -> Result<Self> {
        Self::spawn_internal(args, cwd, env, true)
    }

    fn spawn_internal(
        args: &[String],
        cwd: Option<&str>,
        env: &[(String, String)],
        stop_before_exec: bool,
    ) -> Result<Self> {
        if args.is_empty() {
            bail!("runInTerminal requires at least one argument");
        }

        let mut master_fd: libc::c_int = -1;
        let mut ws = winsize { ws_row: 24, ws_col: 80, ws_xpixel: 0, ws_ypixel: 0 };

        let pid = unsafe {
            libc::forkpty(&mut master_fd, std::ptr::null_mut(), std::ptr::null_mut(), &mut ws)
        };
        if pid < 0 {
            bail!("forkpty failed: {}", std::io::Error::last_os_error());
        }

        if pid == 0 {
            if stop_before_exec {
                unsafe {
                    libc::raise(libc::SIGSTOP);
                }
            }
            child_main(args, cwd, env);
            unreachable!();
        }

        let master = unsafe { File::from_raw_fd(master_fd) };
        let output_sink: Arc<Mutex<Vec<OutputEventBody>>> = Arc::new(Mutex::new(Vec::new()));
        let reader_master = master.try_clone().context("failed to clone PTY master fd")?;
        let sink = Arc::clone(&output_sink);
        let reader = thread::spawn(move || reader_loop(reader_master, sink));

        debug!("debuggee terminal started pid={} args={:?}", pid, args);

        Ok(Self { master, child_pid: pid, reader: Some(reader), output_sink })
    }

    pub fn write_input(&mut self, bytes: &[u8]) -> Result<()> {
        if bytes.is_empty() {
            return Ok(());
        }
        self.master.write_all(bytes).context("failed to write to debuggee PTY")?;
        self.master.flush().ok();
        Ok(())
    }

    pub fn resize(&self, rows: u16, cols: u16) {
        let ws = winsize {
            ws_row: rows.max(1) as libc::c_ushort,
            ws_col: cols.max(1) as libc::c_ushort,
            ws_xpixel: 0,
            ws_ypixel: 0,
        };
        let fd = self.master.as_raw_fd();
        unsafe {
            libc::ioctl(fd, libc::TIOCSWINSZ, &ws);
            libc::kill(self.child_pid, libc::SIGWINCH);
        }
    }

    pub fn take_output(&self) -> Vec<OutputEventBody> {
        let mut guard = self.output_sink.lock().expect("terminal output lock poisoned");
        std::mem::take(&mut *guard)
    }

    pub fn is_running(&self) -> bool {
        let mut status: libc::c_int = 0;
        let result = unsafe { libc::waitpid(self.child_pid, &mut status, libc::WNOHANG) };
        result == 0
    }

    pub fn child_pid(&self) -> libc::pid_t {
        self.child_pid
    }
}

impl Drop for DebuggeeTerminal {
    fn drop(&mut self) {
        unsafe {
            libc::kill(self.child_pid, libc::SIGHUP);
        }
        if let Some(handle) = self.reader.take() {
            let _ = handle.join();
        }
    }
}

fn child_main(args: &[String], cwd: Option<&str>, env: &[(String, String)]) {
    unsafe {
        libc::setsid();
    }

    if let Some(dir) = cwd {
        if std::env::set_current_dir(dir).is_err() {
            eprintln!("daptor: failed to chdir to {dir}");
        }
    }

    for (key, value) in env {
        unsafe {
            let key = CString::new(key.as_str()).unwrap_or_default();
            let value = CString::new(value.as_str()).unwrap_or_default();
            libc::setenv(key.as_ptr(), value.as_ptr(), 1);
        }
    }

    unsafe {
        libc::setenv(
            CString::new("TERM").unwrap().as_ptr(),
            CString::new("xterm-256color").unwrap().as_ptr(),
            1,
        );
        libc::setenv(
            CString::new("COLORTERM").unwrap().as_ptr(),
            CString::new("truecolor").unwrap().as_ptr(),
            1,
        );
    }

    let c_args: Vec<CString> =
        args.iter().map(|arg| CString::new(arg.as_str()).unwrap_or_default()).collect();
    let mut ptrs: Vec<*const libc::c_char> = c_args.iter().map(|s| s.as_ptr()).collect();
    ptrs.push(std::ptr::null());

    unsafe {
        libc::execvp(ptrs[0], ptrs.as_ptr());
        eprintln!("daptor: execvp failed for {}: {}", args[0], std::io::Error::last_os_error());
        libc::_exit(127);
    }
}

fn reader_loop(mut master: File, sink: Arc<Mutex<Vec<OutputEventBody>>>) {
    let mut buffer = [0u8; 4096];
    loop {
        match master.read(&mut buffer) {
            Ok(0) => break,
            Ok(count) => {
                let text = String::from_utf8_lossy(&buffer[..count]).into_owned();
                let mut guard = sink.lock().expect("terminal output lock poisoned");
                guard.push(OutputEventBody {
                    category: Some("stdout".into()),
                    output: text,
                    ..Default::default()
                });
            }
            Err(err) if err.kind() == std::io::ErrorKind::WouldBlock => {
                thread::sleep(Duration::from_millis(10));
            }
            Err(err) => {
                warn!("debuggee terminal read error: {err}");
                break;
            }
        }
    }
}

/// Parse a DAP `runInTerminal` request body into spawn parameters.
pub fn parse_run_in_terminal(
    body: &Value,
) -> Result<(Vec<String>, Option<String>, Vec<(String, String)>)> {
    let args = body
        .get("args")
        .and_then(Value::as_array)
        .context("runInTerminal missing args")?
        .iter()
        .map(|value| {
            value.as_str().context("runInTerminal args must be strings").map(str::to_string)
        })
        .collect::<Result<Vec<_>>>()?;

    let cwd = body.get("cwd").and_then(Value::as_str).map(str::to_string);

    let env = body
        .get("env")
        .and_then(Value::as_object)
        .map(|map| {
            map.iter()
                .filter_map(|(key, value)| value.as_str().map(|v| (key.clone(), v.to_string())))
                .collect()
        })
        .unwrap_or_default();

    Ok((args, cwd, env))
}
