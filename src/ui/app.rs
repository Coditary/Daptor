//! Main TUI application loop — integrates all panels, controls, floats.

use std::path::PathBuf;
use std::time::{Duration, Instant};

use anyhow::Result;
use crossbeam_channel::TryRecvError;
use crossbeam_channel::Receiver;
use ratatui::crossterm::event::{Event, KeyCode, KeyModifiers, MouseButton, MouseEventKind};
use ratatui::style::{Color, Style};
use ratatui::text::{Line, Span};
use ratatui::widgets::{Block, Borders, Clear, Paragraph};
use ratatui::Frame;

use crate::session::{DebugSession, SessionSnapshot, SessionState};
use crate::ui::controls::{self, render_controls};
use crate::ui::float::{self, eval_expression, render_float};
use crate::ui::icons;
use crate::ui::input_debug;
use crate::ui::layout::{bottom_tray_layout, root_layout, sidebar_layout, LayoutAreas};
use crate::ui::panel::{load_source, Panel, PanelAction};
use crate::ui::panels::source::SourcePanel;
use crate::ui::panels::AllPanels;
use crate::ui::resize;
use crate::ui::state::{AppState, ConnectionState, FloatElement, Focus};
use crate::ui::sync;
use crate::ui::terminal::TuiTerminal;

enum SessionSlot {
    Connecting(Receiver<Result<DebugSession>>),
    Ready(DebugSession),
    Failed(String),
}

const CONNECT_TIMEOUT: Duration = Duration::from_secs(45);

fn should_quit(key: &ratatui::crossterm::event::KeyEvent) -> bool {
    key.code == KeyCode::Char('q')
        || key.code == KeyCode::Esc
        || (key.modifiers.contains(KeyModifiers::CONTROL) && key.code == KeyCode::Char('c'))
}

fn drain_console(session: &DebugSession, state: &mut AppState) {
    for output in session.drain_console_output() {
        sync::push_output(state, &output);
    }
}

pub fn run_tui(program: PathBuf) -> Result<()> {
    // Own the TTY *before* spawning debugpy — prevents zsh `suspended (tty output)`.
    let mut tui = TuiTerminal::enter()?;
    let terminal = &mut tui.terminal;

    let mut state = AppState::new(program.clone());
    load_source(&mut state, &program);

    let mut panels = AllPanels::default();
    let session_rx = DebugSession::launch_python_async(program.clone());
    let mut slot = SessionSlot::Connecting(session_rx);

    let result = run_loop(terminal, &mut state, &mut slot, &mut panels);

    if let SessionSlot::Ready(mut session) = slot {
        let _ = session.shutdown();
    }

    tui.leave()?;
    result
}

fn try_adopt_session(slot: &mut SessionSlot, state: &mut AppState) {
    let SessionSlot::Connecting(rx) = slot else {
        return;
    };

    let result = match rx.try_recv() {
        Ok(result) => result,
        Err(TryRecvError::Empty) => return,
        Err(TryRecvError::Disconnected) => {
            let err = "debugpy connection thread exited unexpectedly".to_string();
            state.connection_state = ConnectionState::Failed(err.clone());
            state.status_message = format!("Connection failed: {err}");
            *slot = SessionSlot::Failed(err);
            return;
        }
    };

    match result {
        Ok(mut session) => {
            state.connection_state = ConnectionState::Connected;
            if let Some(snapshot) = session.take_initial_snapshot() {
                sync::apply_snapshot(state, &snapshot, &mut session);
            } else if matches!(session.state(), SessionState::Running) {
                state.session_state = SessionState::Running;
                state.status_message = "Running".into();
            } else if matches!(session.state(), SessionState::Exited) {
                state.session_state = SessionState::Exited;
                state.status_message = "Session ended".into();
            } else if let Err(err) = sync::sync_from_session(state, &mut session) {
                state.connection_state = ConnectionState::Failed(err.to_string());
                state.status_message = format!("Sync failed: {err:#}");
                *slot = SessionSlot::Failed(err.to_string());
                return;
            }
            if state.source_lines.is_empty() {
                let program = state.program.clone();
                load_source(state, &program);
            }
            *slot = SessionSlot::Ready(session);
        }
        Err(err) => {
            state.connection_state = ConnectionState::Failed(err.to_string());
            state.status_message = format!("Connection failed: {err:#}");
            *slot = SessionSlot::Failed(err.to_string());
        }
    }
}

fn check_connect_timeout(slot: &mut SessionSlot, state: &mut AppState, started: Instant) {
    if !matches!(state.connection_state, ConnectionState::Connecting) {
        return;
    }
    if !matches!(slot, SessionSlot::Connecting(_)) {
        return;
    }
    if started.elapsed() < CONNECT_TIMEOUT {
        return;
    }

    let err = format!(
        "timed out connecting to debugpy after {} seconds — is debugpy installed? (`python3 -m pip install debugpy`)",
        CONNECT_TIMEOUT.as_secs()
    );
    state.connection_state = ConnectionState::Failed(err.clone());
    state.status_message = format!("Connection failed: {err}");
    *slot = SessionSlot::Failed(err);
}

fn poll_session(slot: &mut SessionSlot, state: &mut AppState) -> Result<()> {
    let SessionSlot::Ready(session) = slot else {
        return Ok(());
    };

    drain_console(session, state);

    match session.poll_events()? {
        Some(snapshot) => sync::apply_snapshot(state, &snapshot, session),
        None if matches!(session.state(), SessionState::Exited) => {
            sync::apply_snapshot(
                state,
                &SessionSnapshot {
                    state: SessionState::Exited,
                    threads: vec![],
                    stack_frames: vec![],
                    scopes: vec![],
                    variables: vec![],
                },
                session,
            );
        }
        None => {}
    }

    Ok(())
}

fn run_loop(
    terminal: &mut ratatui::Terminal<ratatui::backend::CrosstermBackend<std::io::Stdout>>,
    state: &mut AppState,
    slot: &mut SessionSlot,
    panels: &mut AllPanels,
) -> Result<()> {
    let spinners = ['⠋', '⠙', '⠹', '⠸', '⠼', '⠴', '⠦', '⠧'];
    let mut spinner_tick: u8 = 0;
    let connect_started = Instant::now();

    loop {
        input_debug::tick_frame(&mut state.input_debug);

        try_adopt_session(slot, state);
        check_connect_timeout(slot, state, connect_started);
        poll_session(slot, state)?;

        if matches!(state.connection_state, ConnectionState::Connecting) {
            spinner_tick = spinner_tick.wrapping_add(1);
            let ch = spinners[(spinner_tick as usize) % spinners.len()];
            state.status_message =
                format!("{ch} Connecting to debugpy… (q/Esc/Ctrl+C to cancel)");
        }

        let events = crate::ui::terminal::poll_events(Duration::from_millis(50))?;
        if events.is_empty() {
            input_debug::tick_poll_empty(&mut state.input_debug);
        } else {
            for event in events {
                if handle_event(terminal, state, slot, panels, event)? {
                    return Ok(());
                }
            }
        }

        if sync::should_auto_exit(state) {
            return Ok(());
        }

        terminal.draw(|frame| {
            let area = frame.area();
            frame.render_widget(Clear, area);
            crate::ui::chrome::fill_background(frame, area);

            if state.ui_open {
                let [sidebar, source_area, bottom, status_area] =
                    root_layout(area, &state.layout);
                let show_bp = !state.breakpoints.is_empty();
                let show_watches = !state.watches.is_empty();
                let [scopes_a, bp_a, stacks_a, watches_a] =
                    sidebar_layout(sidebar, show_bp, show_watches, &state.layout);
                let (controls_area, [repl_a, console_a]) =
                    bottom_tray_layout(bottom, &state.layout);

                panels.scopes.render(frame, scopes_a, state);
                if show_bp {
                    panels.breakpoints.render(frame, bp_a, state);
                }
                panels.stacks.render(frame, stacks_a, state);
                if show_watches {
                    panels.watches.render(frame, watches_a, state);
                }
                panels.source.render(frame, source_area, state);
                render_controls(frame, controls_area, state);
                panels.repl.render(frame, repl_a, state);
                panels.console.render(frame, console_a, state);

                state.layout_areas = LayoutAreas::compute(area, state);
                LayoutAreas::render_dividers(
                    frame,
                    &state.layout_areas,
                    state.hover_divider,
                    resize::active_divider(state),
                );

                render_status(frame, status_area, state);
            } else {
                let closed = Block::default()
                    .borders(Borders::ALL)
                    .title(format!(" {} tui-debug — UI closed ", icons::controls::DISCONNECT));
                let inner = closed.inner(area);
                frame.render_widget(closed, area);
                panels.source.render(frame, inner, state);
                let hint = ratatui::layout::Rect::new(
                    area.x,
                    area.y + area.height.saturating_sub(1),
                    area.width,
                    1,
                );
                frame.render_widget(
                    Paragraph::new(" o: reopen UI | q: quit ")
                        .style(Style::default().fg(Color::DarkGray)),
                    hint,
                );
            }

            if float::is_open(state) {
                if let SessionSlot::Ready(session) = slot {
                    render_float(frame, area, state, panels, session);
                }
            }

            if matches!(state.connection_state, ConnectionState::Failed(_)) {
                render_connection_error(frame, area, state);
            }

            input_debug::render(frame, area, state, slot_label(slot));
        })?;
    }
}

/// Returns `true` when the app should exit.
fn handle_event(
    terminal: &mut ratatui::Terminal<ratatui::backend::CrosstermBackend<std::io::Stdout>>,
    state: &mut AppState,
    slot: &mut SessionSlot,
    panels: &mut AllPanels,
    event: Event,
) -> Result<bool> {
    input_debug::record_event(state, &event);

    match event {
        Event::Key(key) => {
            if key.code == KeyCode::F(12) {
                input_debug::toggle_visible(state);
                return Ok(false);
            }

            if should_quit(&key) {
                input_debug::set_action(state, "quit");
                return Ok(true);
            }

            if float::handle_key(key, state) {
                input_debug::set_action(state, "handled by float overlay");
                return Ok(false);
            }

            if let SessionSlot::Ready(session) = slot {
                if handle_global_key(key, state, session)? {
                    input_debug::set_action(state, "handled by global key");
                    return Ok(false);
                }
            } else if key.code == KeyCode::Char('o') {
                if state.ui_open {
                    state.close_ui();
                } else {
                    state.open_ui();
                }
                input_debug::set_action(state, "toggle UI (o)");
                return Ok(false);
            }

            if !state.ui_open {
                input_debug::set_action(state, "ignored — UI closed (o to open)");
                return Ok(false);
            }

            if key.code == KeyCode::Tab {
                state.cycle_focus_next();
                input_debug::set_action(state, format!("Tab → focus {:?}", state.focus));
                return Ok(false);
            }
            if key.code == KeyCode::BackTab {
                state.cycle_focus_prev();
                input_debug::set_action(
                    state,
                    format!("Shift+Tab → focus {:?}", state.focus),
                );
                return Ok(false);
            }

            if resize::handle_layout_key(key, state) {
                input_debug::set_action(state, "layout resize key");
                return Ok(false);
            }
            if resize::handle_scopes_resize_key(key, state) {
                input_debug::set_action(state, "scopes resize key");
                return Ok(false);
            }

            if !state.is_connected() {
                input_debug::set_action(state, "ignored — not connected to debugpy yet");
                return Ok(false);
            }

            let SessionSlot::Ready(session) = slot else {
                input_debug::set_action(state, "ignored — session not ready");
                return Ok(false);
            };

            if let Some(action) = controls::handle_key(state, key) {
                apply_action(action, state, session)?;
                drain_console(session, state);
                input_debug::set_action(state, "controls winbar key");
                return Ok(false);
            }

            let action = match state.focus {
                Focus::Source => panels.source.handle_key(key, state, session),
                Focus::Scopes => panels.scopes.handle_key(key, state, session),
                Focus::Breakpoints => panels.breakpoints.handle_key(key, state, session),
                Focus::Stacks => panels.stacks.handle_key(key, state, session),
                Focus::Watches => panels.watches.handle_key(key, state, session),
                Focus::Repl => panels.repl.handle_key(key, state, session),
                Focus::Console => panels.console.handle_key(key, state, session),
            };
            apply_action(action, state, session)?;
            drain_console(session, state);
            input_debug::set_action(state, format!("panel {:?} handled key", state.focus));
        }
        Event::Mouse(mouse) => {
            if !state.ui_open {
                input_debug::set_action(state, "mouse ignored — UI closed");
                return Ok(false);
            }

            let areas = state.layout_areas.clone();
            resize::update_hover_divider(state, &mouse, &areas);

            match mouse.kind {
                MouseEventKind::Up(_) => {
                    resize::handle_mouse_up(state);
                    input_debug::set_action(state, "mouse up");
                }
                MouseEventKind::Down(MouseButton::Left) => {
                    resize::clear_stale_drag_on_press(state);

                    if resize::begin_divider_drag(state, &mouse, &areas) {
                        input_debug::set_action(state, "divider drag started");
                        return Ok(false);
                    }

                    if state.is_connected() {
                        if let SessionSlot::Ready(session) = slot {
                            if let Some(action) = controls::handle_mouse(state, mouse) {
                                apply_action(action, state, session)?;
                                drain_console(session, state);
                                input_debug::set_action(state, "controls winbar click");
                                return Ok(false);
                            }

                            if areas.is_gutter_click(mouse.column, mouse.row) {
                                if let Some(line) =
                                    areas.source_line_at(state, mouse.column, mouse.row)
                                {
                                    state.focus = Focus::Source;
                                    let action =
                                        SourcePanel::toggle_breakpoint_at_line(state, line);
                                    apply_action(action, state, session)?;
                                    drain_console(session, state);
                                    input_debug::set_action(
                                        state,
                                        format!("gutter click line {line}"),
                                    );
                                    return Ok(false);
                                }
                            }
                        }
                    }

                    if resize::set_focus_from_mouse(state, &areas, &mouse) {
                        input_debug::set_action(
                            state,
                            format!("mouse → focus {:?}", state.focus),
                        );
                    } else {
                        input_debug::set_action(state, "mouse down (no hit)");
                    }
                }
                MouseEventKind::Drag(_) => {
                    resize::handle_mouse_motion(state, &mouse, &areas);
                    if resize::is_dragging(state) {
                        input_debug::set_action(state, "divider dragging");
                    }
                }
                MouseEventKind::Moved => {
                    controls::update_hover(state, &mouse);
                }
                _ => {
                    input_debug::set_action(state, "mouse scroll/other");
                }
            }
        }
        Event::Resize(_, _) => {
            let _ = crate::ui::terminal::enable_mouse(terminal.backend_mut());
            input_debug::set_action(state, "terminal resized");
        }
        _ => {}
    }

    Ok(false)
}

fn slot_label(slot: &SessionSlot) -> &'static str {
    match slot {
        SessionSlot::Connecting(_) => "connecting",
        SessionSlot::Ready(_) => "ready",
        SessionSlot::Failed(_) => "failed",
    }
}

fn render_connection_error(frame: &mut Frame, area: ratatui::layout::Rect, state: &AppState) {
    let msg = match &state.connection_state {
        ConnectionState::Failed(err) => format!("Connection failed:\n{err}\n\nPress q, Esc, or Ctrl+C to quit"),
        _ => return,
    };

    let popup = float::centered_rect(50, 30, area);
    frame.render_widget(Clear, popup);
    frame.render_widget(
        Block::default()
            .borders(Borders::ALL)
            .border_style(Style::default().fg(Color::Red))
            .title(" tui-debug "),
        popup,
    );
    let inner = Block::default().inner(popup);
    frame.render_widget(Paragraph::new(msg), inner);
}

fn render_status(frame: &mut Frame, area: ratatui::layout::Rect, state: &AppState) {
    let focus = format!("{:?}", state.focus);
    let status = Line::from(vec![
        Span::styled(
            format!(" {} ", state.status_message),
            crate::ui::theme::status_bar(),
        ),
        Span::styled(" | ", crate::ui::theme::control_disabled()),
        Span::styled(focus, crate::ui::theme::frame_current()),
        Span::styled(
            " | Tab: panel | Ctrl+Shift+arrows: resize | drag ┃━ dividers | Scopes: Ctrl+↑↓ | 1-8: controls",
            crate::ui::theme::control_disabled(),
        ),
    ]);
    frame.render_widget(
        Paragraph::new(status).style(Style::default().bg(crate::ui::theme::bg::ROOT)),
        area,
    );
}

fn handle_global_key(
    key: ratatui::crossterm::event::KeyEvent,
    state: &mut AppState,
    session: &mut DebugSession,
) -> Result<bool> {
    match key.code {
        KeyCode::Char('r') => {
            state.focus = Focus::Repl;
            state.status_message = "REPL — type expression, Enter to evaluate".into();
            return Ok(true);
        }
        KeyCode::Char('o') => {
            if state.ui_open {
                state.close_ui();
            } else {
                state.open_ui();
            }
            return Ok(true);
        }
        KeyCode::Char('K') => {
            if matches!(state.session_state, SessionState::Stopped { .. }) {
                let _ = eval_expression(state, session, None);
            }
            return Ok(true);
        }
        KeyCode::Char('f') => {
            let element = focus_to_float_element(state.focus);
            float::open_element(state, element);
            return Ok(true);
        }
        KeyCode::F(1) => {
            float::open_element(state, FloatElement::Scopes);
            return Ok(true);
        }
        KeyCode::F(2) => {
            float::open_element(state, FloatElement::Stacks);
            return Ok(true);
        }
        KeyCode::F(3) => {
            float::open_element(state, FloatElement::Breakpoints);
            return Ok(true);
        }
        KeyCode::F(4) => {
            float::open_element(state, FloatElement::Watches);
            return Ok(true);
        }
        KeyCode::F(5) => {
            float::open_element(state, FloatElement::Repl);
            return Ok(true);
        }
        KeyCode::F(6) => {
            float::open_element(state, FloatElement::Console);
            return Ok(true);
        }
        _ => {}
    }
    Ok(false)
}

fn focus_to_float_element(focus: Focus) -> FloatElement {
    match focus {
        Focus::Scopes => FloatElement::Scopes,
        Focus::Breakpoints => FloatElement::Breakpoints,
        Focus::Stacks => FloatElement::Stacks,
        Focus::Watches => FloatElement::Watches,
        Focus::Repl => FloatElement::Repl,
        Focus::Console => FloatElement::Console,
        Focus::Source => FloatElement::Scopes,
    }
}

fn apply_action(action: PanelAction, state: &mut AppState, session: &mut DebugSession) -> Result<()> {
    match action {
        PanelAction::None | PanelAction::CloseFloat | PanelAction::OpenEval | PanelAction::ToggleUi => {}
        PanelAction::SyncBreakpoints => {
            let Some(path) = state.source_path.clone() else {
                return Ok(());
            };
            let lines: Vec<u32> = state
                .breakpoints
                .iter()
                .filter(|bp| bp.path == path)
                .map(|bp| bp.line)
                .collect();
            match session.set_source_breakpoints(&path, &lines) {
                Ok(verified) => {
                    for (line, ok) in verified {
                        if let Some(bp) = state
                            .breakpoints
                            .iter_mut()
                            .find(|bp| bp.path == path && bp.line == line)
                        {
                            bp.verified = ok;
                        }
                    }
                    state.status_message =
                        format!("Synced {} breakpoint(s) with debugpy", lines.len());
                }
                Err(err) => {
                    state.status_message = format!("Breakpoint sync failed: {err:#}");
                }
            }
        }
        PanelAction::EvaluateRepl => {
            let expression = state.repl_input.trim().to_string();
            state.repl_input.clear();

            let frame_id = match state.stack_frames.first() {
                Some(frame) => frame.id,
                None => {
                    state
                        .repl_history
                        .push(format!("> {expression}\n= (no active frame)"));
                    state.status_message = "No active frame for eval".into();
                    return Ok(());
                }
            };

            match session.evaluate(&expression, frame_id, "repl") {
                Ok(result) => {
                    state
                        .repl_history
                        .push(format!("> {expression}\n= {result}"));
                    state.status_message = format!("REPL: {expression}");
                }
                Err(err) => {
                    state
                        .repl_history
                        .push(format!("> {expression}\n= error: {err}"));
                    state.status_message = format!("REPL error: {err:#}");
                }
            }
        }
        PanelAction::Quit => return Ok(()),
        PanelAction::Refresh => {
            sync::sync_from_session(state, session)?;
        }
        PanelAction::Continue => {
            session.dispatch_continue()?;
            state.session_state = SessionState::Running;
            state.status_message = "Running".into();
        }
        PanelAction::PlayPause => {
            match &state.session_state {
                SessionState::Stopped { .. } => {
                    session.dispatch_continue()?;
                    state.session_state = SessionState::Running;
                    state.status_message = "Running".into();
                }
                SessionState::Running => {
                    session.dispatch_pause()?;
                    state.status_message = "Pausing…".into();
                }
                _ => {}
            }
        }
        PanelAction::Pause => {
            session.dispatch_pause()?;
            state.status_message = "Pausing…".into();
        }
        PanelAction::StepOver => {
            session.dispatch_step("next")?;
            state.session_state = SessionState::Running;
            state.status_message = "Running".into();
        }
        PanelAction::StepInto => {
            session.dispatch_step("stepIn")?;
            state.session_state = SessionState::Running;
            state.status_message = "Running".into();
        }
        PanelAction::StepOut => {
            session.dispatch_step("stepOut")?;
            state.session_state = SessionState::Running;
            state.status_message = "Running".into();
        }
        PanelAction::StepBack => {
            match session.step_back() {
                Ok(snapshot) => sync::apply_snapshot(state, &snapshot, session),
                Err(err) => state.status_message = format!("Step back unsupported: {err:#}"),
            }
        }
        PanelAction::Restart => {
            match session.dispatch_restart() {
                Ok(()) => {
                    state.session_state = SessionState::Running;
                    state.status_message = "Restarting…".into();
                }
                Err(err) => state.status_message = format!("Restart unsupported: {err:#}"),
            }
        }
        PanelAction::Terminate => {
            session.terminate()?;
            state.close_ui();
        }
        PanelAction::Disconnect => {
            session.disconnect()?;
            state.close_ui();
        }
    }
    Ok(())
}
