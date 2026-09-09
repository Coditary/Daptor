#pragma once

#include <tuinator/render/style.hpp>

namespace tui_debug_ui {

/// Ignore SIGTTIN/SIGTTOU so zsh/bash do not suspend the TUI when debugpy or
/// ncurses read/write the controlling terminal.
void ignore_job_control_tty_signals();

/// Ignore SIGPIPE so writes to a closed debugpy adapter do not kill the UI.
void ignore_sigpipe();

/// Put the UI in its own foreground process group so keyboard signals stay here.
void claim_terminal_for_ui();

/// Reclaim the terminal if debugpy/debuggee stole foreground (cheap no-op when already ok).
void reclaim_terminal_for_ui();

/// Route terminal ^C (SIGINT) to a pollable quit flag instead of the debuggee.
void install_sigint_quit_handler();

/// Returns true once after the user pressed ^C; clears the latch.
bool consume_sigint_quit_request();

/// Sync LINES/COLUMNS from the real TTY winsize before ncurses initializes.
/// Stale env vars (often 80x24) otherwise cap the UI to a corner of the screen.
void sync_terminal_size_from_tty();

/// After ncurses init, call resizeterm() to match the real TTY dimensions.
void apply_ncurses_winsize();

/// Match the terminal's default background to the UI theme (OSC 11).
void set_terminal_theme_background(tuinator::Rgb background);

}  // namespace tui_debug_ui
