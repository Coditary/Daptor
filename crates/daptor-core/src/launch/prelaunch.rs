use std::process::Command;

use anyhow::{bail, Context, Result};

use super::resolve::{expand_template, LaunchResolveContext};

pub fn run_prelaunch_commands(commands: &[String], ctx: &LaunchResolveContext) -> Result<()> {
    for command in commands {
        let expanded = expand_template(command, ctx);
        if expanded.trim().is_empty() {
            continue;
        }

        let status = Command::new("sh")
            .arg("-c")
            .arg(&expanded)
            .current_dir(&ctx.workspace)
            .status()
            .with_context(|| format!("failed to run prelaunch command: {expanded}"))?;

        if !status.success() {
            bail!("prelaunch command failed (exit {}): {expanded}", status);
        }
    }

    Ok(())
}
