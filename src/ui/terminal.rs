//! Terminal setup helpers — avoid zsh `suspended (tty output)` and similar job-control traps.

use std::io::{self, IsTerminal, Write};
use std::time::Duration;

use anyhow::{Context, Result, bail};
use ratatui::crossterm::event::{self, Event, KeyCode, KeyModifiers};
use ratatui::crossterm::execute;
use ratatui::crossterm::terminal::{
    disable_raw_mode, enable_raw_mode, EnterAlternateScreen, LeaveAlternateScreen,
};
use ratatui::layout::{Alignment, Rect};
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph};
use ratatui::{backend::CrosstermBackend, Terminal};

pub struct TuiTerminal {
    pub terminal: Terminal<CrosstermBackend<io::Stdout>>,
    active: bool,
}

impl Drop for TuiTerminal {
    fn drop(&mut self) {
        if self.active {
            let _ = disable_raw_mode();
            let _ = execute!(
                self.terminal.backend_mut(),
                LeaveAlternateScreen,
            );
            let _ = disable_mouse(self.terminal.backend_mut());
            let _ = self.terminal.show_cursor();
            self.active = false;
        }
    }
}

impl TuiTerminal {
    pub fn enter() -> Result<Self> {
        ensure_tty()?;
        ignore_job_control_signals();

        enable_raw_mode().context("failed to enable raw mode")?;
        let mut stdout = io::stdout();
        execute!(stdout, EnterAlternateScreen).context("failed to enter alternate screen")?;
        enable_mouse(&mut stdout).context("failed to enable mouse capture")?;
        // Ensure stdin is ready to deliver mouse events.
        let _ = stdout.flush();

        let backend = CrosstermBackend::new(stdout);
        let mut terminal = Terminal::new(backend).context("failed to create terminal")?;
        terminal.clear()?;

        Ok(Self {
            terminal,
            active: true,
        })
    }

    pub fn leave(mut self) -> Result<()> {
        disable_raw_mode()?;
        execute!(
            self.terminal.backend_mut(),
            LeaveAlternateScreen,
        )?;
        disable_mouse(self.terminal.backend_mut())?;
        self.terminal.show_cursor()?;
        self.active = false;
        Ok(())
    }
}

pub fn ensure_tty() -> Result<()> {
    if !io::stdin().is_terminal() || !io::stdout().is_terminal() {
        bail!(
            "tui-debug run requires an interactive terminal (TTY).\n\
             Start it directly in a terminal — not piped, not backgrounded (`&`), not from a non-TTY runner."
        );
    }
    Ok(())
}

/// Prevent zsh/bash from suspending the process on background TTY writes (SIGTTOU/SIGTTIN).
fn ignore_job_control_signals() {
    unsafe {
        libc::signal(libc::SIGTTOU, libc::SIG_IGN);
        libc::signal(libc::SIGTTIN, libc::SIG_IGN);
    }
}

/// Click + drag tracking in SGR mode — omit ?1003 (all motion) so noisy terminals
/// don't flood the event loop and swallow button presses.
pub fn enable_mouse(out: &mut impl Write) -> io::Result<()> {
    write!(out, "\x1b[?1000h\x1b[?1002h\x1b[?1006h")?;
    out.flush()
}

pub fn disable_mouse(out: &mut impl Write) -> io::Result<()> {
    write!(out, "\x1b[?1006l\x1b[?1002l\x1b[?1000l")?;
    out.flush()
}

/// Block up to `timeout` for the first event, then drain any others already queued.
/// Processing every pending event before the next redraw avoids TTY back-pressure deadlocks
/// when the terminal floods us with mouse reports.
pub fn poll_events(timeout: Duration) -> std::io::Result<Vec<ratatui::crossterm::event::Event>> {
    use ratatui::crossterm::event;

    if !event::poll(timeout)? {
        return Ok(Vec::new());
    }

    let mut events = vec![event::read()?];
    while event::poll(Duration::from_millis(0))? {
        events.push(event::read()?);
    }
    Ok(events)
}

pub fn show_boot_message(terminal: &mut Terminal<CrosstermBackend<io::Stdout>>, message: &str) -> Result<()> {
    terminal.draw(|frame| {
        let area = frame.area();
        frame.render_widget(Clear, area);
        frame.render_widget(
            Block::default().style(Style::default().bg(Color::Black)),
            area,
        );

        let popup = centered_rect(60, 20, area);
        frame.render_widget(Clear, popup);
        frame.render_widget(
            Block::default()
                .borders(Borders::ALL)
                .border_style(Style::default().fg(Color::Cyan))
                .title(" tui-debug ")
                .title_alignment(Alignment::Center),
            popup,
        );

        let inner = Block::default().borders(Borders::NONE).inner(popup);
        frame.render_widget(
            Paragraph::new(vec![
                Line::from(""),
                Line::from(Span::styled(message, Style::default().fg(Color::White))),
            ])
            .alignment(Alignment::Center),
            inner,
        );
    })?;
    terminal.flush()?;
    Ok(())
}

/// Block until the user presses q/Esc/Ctrl+C after a fatal in-TUI error.
pub fn wait_for_quit(
    terminal: &mut Terminal<CrosstermBackend<io::Stdout>>,
    message: &str,
) -> Result<()> {
    loop {
        terminal.draw(|frame| {
            show_boot_message_content(frame, frame.area(), message);
        })?;

        if event::poll(std::time::Duration::from_millis(100))? {
            if let Event::Key(key) = event::read()? {
                if key.code == KeyCode::Char('q')
                    || key.code == KeyCode::Esc
                    || (key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c'))
                {
                    break;
                }
            }
        }
    }
    Ok(())
}

fn show_boot_message_content(frame: &mut ratatui::Frame, area: Rect, message: &str) {
    frame.render_widget(
        Block::default().style(Style::default().bg(Color::Black)),
        area,
    );
    let popup = centered_rect(60, 20, area);
    frame.render_widget(
        Block::default()
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Red))
            .title(" tui-debug ")
            .title_alignment(Alignment::Center),
        popup,
    );
    let inner = Block::default().inner(popup);
    frame.render_widget(
        Paragraph::new(message).alignment(Alignment::Center),
        inner,
    );
}

fn centered_rect(percent_x: u16, percent_y: u16, area: Rect) -> Rect {
    let vertical = ratatui::layout::Layout::default()
        .direction(ratatui::layout::Direction::Vertical)
        .constraints([
            ratatui::layout::Constraint::Percentage((100 - percent_y) / 2),
            ratatui::layout::Constraint::Percentage(percent_y),
            ratatui::layout::Constraint::Percentage((100 - percent_y) / 2),
        ])
        .split(area);

    ratatui::layout::Layout::default()
        .direction(ratatui::layout::Direction::Horizontal)
        .constraints([
            ratatui::layout::Constraint::Percentage((100 - percent_x) / 2),
            ratatui::layout::Constraint::Percentage(percent_x),
            ratatui::layout::Constraint::Percentage((100 - percent_x) / 2),
        ])
        .split(vertical[1])[1]
}
