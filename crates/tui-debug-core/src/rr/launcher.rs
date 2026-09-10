use std::net::TcpListener;
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result, bail};
use tracing::info;

pub const DEFAULT_REPLAY_PORT: u16 = 50505;

pub fn ensure_rr_available() -> Result<()> {
    Command::new("rr")
        .arg("--version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .context(
            "rr not found — install it first (Fedora: sudo dnf install rr, then retry with --rr)",
        )?;
    Ok(())
}

pub fn ensure_gdb_available() -> Result<()> {
    Command::new("gdb")
        .arg("--version")
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .context("gdb not found — install gdb (required for rr replay debugging)")?;
    Ok(())
}

pub fn pick_replay_port(preferred: u16) -> Result<u16> {
    if TcpListener::bind(("127.0.0.1", preferred)).is_ok() {
        return Ok(preferred);
    }
    let listener = TcpListener::bind("127.0.0.1:0").context("failed to bind ephemeral port for rr replay")?;
    Ok(listener.local_addr()?.port())
}

pub fn record_program(program: &Path) -> Result<()> {
    info!("recording {} with rr", program.display());
    let output = Command::new("rr")
        .arg("record")
        .arg("--")
        .arg(program)
        .output()
        .context("failed to run rr record — install rr from https://rr-project.org/")?;

    if !output.status.success() {
        let stderr = String::from_utf8_lossy(&output.stderr);
        let stdout = String::from_utf8_lossy(&output.stdout);
        let detail = if !stderr.trim().is_empty() {
            stderr.trim().to_string()
        } else {
            stdout.trim().to_string()
        };
        if detail.is_empty() {
            bail!("rr record failed with status {}", output.status);
        }
        bail!("rr record failed: {detail}");
    }
    Ok(())
}

pub fn spawn_replay_server(port: u16) -> Result<Child> {
    info!("starting rr replay gdbserver on 127.0.0.1:{port}");
    let child = Command::new("rr")
        .args(["replay", "-s", &port.to_string(), "-k"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .context("failed to spawn rr replay — is rr installed and is there a recent recording?")?;

    wait_for_replay_port(port)?;
    Ok(child)
}

fn wait_for_replay_port(port: u16) -> Result<()> {
    let deadline = std::time::Instant::now() + Duration::from_secs(30);
    while std::time::Instant::now() < deadline {
        if TcpListener::bind(("127.0.0.1", port)).is_err() {
            return Ok(());
        }
        thread::sleep(Duration::from_millis(50));
    }
    bail!("timed out waiting for rr replay server on port {port}")
}
