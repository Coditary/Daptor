mod dap;
mod session;
mod ui;

use std::path::PathBuf;

use anyhow::Result;
use clap::{Parser, Subcommand};
use tracing::info;
use tracing_subscriber::EnvFilter;

use crate::session::{DebugSession, print_snapshot};
use crate::ui::run_tui;

#[derive(Parser)]
#[command(name = "tui-debug", about = "Standalone DAP debugger")]
struct Cli {
    #[command(subcommand)]
    command: Command,
}

#[derive(Subcommand)]
enum Command {
    /// Launch the interactive TUI debugger.
    Run {
        #[arg(long, default_value = "fixtures/hello.py")]
        program: PathBuf,
    },
    /// Smoke-test the DAP server layer against debugpy + a Python program.
    Test {
        #[arg(long, default_value = "fixtures/hello.py")]
        program: PathBuf,
    },
}

fn main() -> Result<()> {
    let cli = Cli::parse();

    match &cli.command {
        Command::Run { .. } => {
            // TUI owns stdout — keep logging off unless RUST_LOG explicitly set to stderr.
            tracing_subscriber::fmt()
                .with_writer(std::io::stderr)
                .with_env_filter(
                    EnvFilter::try_from_default_env()
                        .unwrap_or_else(|_| EnvFilter::new("warn")),
                )
                .init();
        }
        Command::Test { .. } => {
            tracing_subscriber::fmt()
                .with_env_filter(EnvFilter::from_default_env().add_directive("tui_debug=info".parse()?))
                .init();
        }
    }

    match cli.command {
        Command::Run { program } => run_tui(program),
        Command::Test { program } => run_test(program),
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
    } else if let Some(snapshot) = session.refresh_snapshot().ok() {
        print_snapshot(&snapshot);
    }

    session.shutdown()?;
    info!("session closed");
    Ok(())
}
