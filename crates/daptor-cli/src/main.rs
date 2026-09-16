use std::path::{Path, PathBuf};
use std::process::{Command as ProcessCommand, Stdio};

use anyhow::{bail, Context, Result};
use clap::{Parser, Subcommand};
use daptor_core::{print_snapshot, DebugSession};
use tracing::info;
use tracing_subscriber::EnvFilter;

#[derive(Parser)]
#[command(name = "daptor", about = "Standalone DAP debugger")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Launch the interactive TUI debugger.
    Run {
        /// Program to debug
        program: PathBuf,
        /// Frontend-only mock session (no Rust/DAP backend).
        #[arg(long)]
        mock: bool,
    },
    /// Smoke-test the DAP server layer against debugpy + a Python program.
    Test {
        /// Program to debug
        program: PathBuf,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    match &cli.command {
        Command::Run { .. } => {
            tracing_subscriber::fmt()
                .with_writer(std::io::stderr)
                .with_env_filter(
                    EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("warn")),
                )
                .init();
        }
        Command::Test { .. } => {
            tracing_subscriber::fmt()
                .with_env_filter(
                    EnvFilter::from_default_env().add_directive("tui_debug=info".parse()?),
                )
                .init();
        }
    }

    match cli.command {
        Command::Run { program, mock } => run_ui(&program, mock),
        Command::Test { program } => run_test(program),
    }
}

fn ui_binary_candidates() -> [PathBuf; 3] {
    [
        PathBuf::from("cpp/build/daptor"),
        PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../cpp/build/daptor"),
        PathBuf::from("daptor"),
    ]
}

fn find_ui_binary() -> Option<PathBuf> {
    ui_binary_candidates().into_iter().find(|path| path.is_file())
}

fn run_ui(program: &Path, mock: bool) -> Result<()> {
    let ui = find_ui_binary().context(
        "Tuinator UI binary not found.\n\
         Build it with:\n\
           cmake -S cpp -B cpp/build && cmake --build cpp/build",
    )?;

    let mut command = ProcessCommand::new(&ui);
    if mock {
        command.arg("--mock");
    }
    command.arg(program).stdin(Stdio::inherit()).stdout(Stdio::inherit()).stderr(Stdio::inherit());

    // Replace this process with the UI binary so the TTY stays in the foreground.
    #[cfg(unix)]
    {
        use std::os::unix::process::CommandExt;

        let err = command.exec();
        bail!("failed to exec {}: {err}", ui.display());
    }

    #[cfg(not(unix))]
    {
        let status =
            command.status().with_context(|| format!("failed to launch {}", ui.display()))?;

        if status.success() {
            Ok(())
        } else {
            bail!("daptor exited with {}", status);
        }
    }
}

fn run_test(program: PathBuf) -> Result<()> {
    info!("connecting to debugpy for {}", program.display());

    let mut session = DebugSession::launch_python(&program)?;
    let snapshot = session.refresh_snapshot()?;
    print_snapshot(&snapshot);

    info!("stepping over once");
    let snapshot = session.step_over()?;
    print_snapshot(&snapshot);

    info!("continuing to completion");
    if session.continue_execution()?.is_none() {
        info!("program exited");
    } else if let Ok(snapshot) = session.refresh_snapshot() {
        print_snapshot(&snapshot);
    }

    info!("restarting after exit");
    session.restart()?;
    if let Some(snapshot) = session.take_initial_snapshot() {
        print_snapshot(&snapshot);
    } else if let Ok(snapshot) = session.refresh_snapshot() {
        print_snapshot(&snapshot);
    }

    session.shutdown()?;
    info!("session closed");
    Ok(())
}
