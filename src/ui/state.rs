//! Shared UI state consumed by all panels.
//!
//! Layout mirrors nvim-dap-ui default config (see `reference/nvim-dap-ui/lua/dapui/config/init.lua`):
//! - Left sidebar: scopes, breakpoints, stacks, watches (25% each)
//! - Bottom tray: repl + console
//! - Center: source viewer (standalone addition)

use std::collections::{HashMap, HashSet};
use std::path::PathBuf;

use ratatui::layout::Rect;

use crate::dap::protocol::{Scope, StackFrame, Thread, Variable};
use crate::session::SessionState;

/// DAP connection lifecycle (UI may render before debugpy is ready).
#[derive(Debug, Clone, PartialEq, Eq, Default)]
pub enum ConnectionState {
    #[default]
    Connecting,
    Connected,
    Failed(String),
}

/// Which panel currently receives keyboard input.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash, Default)]
pub enum Focus {
    #[default]
    Source,
    Scopes,
    Breakpoints,
    Stacks,
    Watches,
    Repl,
    Console,
}

/// nvim-dap-ui element IDs available as floating windows.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum FloatElement {
    Scopes,
    Stacks,
    Breakpoints,
    Watches,
    Repl,
    Console,
}

impl std::fmt::Display for FloatElement {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Scopes => write!(f, "scopes"),
            Self::Stacks => write!(f, "stacks"),
            Self::Breakpoints => write!(f, "breakpoints"),
            Self::Watches => write!(f, "watches"),
            Self::Repl => write!(f, "repl"),
            Self::Console => write!(f, "console"),
        }
    }
}

/// Active floating window (eval hover or float_element).
#[derive(Debug, Clone, Default)]
pub enum FloatKind {
    #[default]
    None,
    Eval {
        expression: String,
        result: String,
        error: Option<String>,
    },
    Element {
        element: FloatElement,
        enter: bool,
    },
}

/// Auto open/close behaviour (mirrors nvim-dap-ui dap.listeners hooks).
#[derive(Debug, Clone)]
pub struct UiConfig {
    pub auto_open: bool,
    pub auto_close: bool,
}

impl Default for UiConfig {
    fn default() -> Self {
        Self {
            auto_open: true,
            auto_close: true,
        }
    }
}

/// Source panel interaction mode.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum SourceMode {
    #[default]
    View,
    FilePicker,
}

/// A breakpoint shown in the breakpoints panel.
#[derive(Debug, Clone)]
pub struct BreakpointEntry {
    pub id: i64,
    pub path: PathBuf,
    pub line: u32,
    pub enabled: bool,
    pub verified: bool,
}

/// A watch expression shown in the watches panel.
#[derive(Debug, Clone)]
pub struct WatchEntry {
    pub id: u64,
    pub expression: String,
    pub value: String,
    pub error: Option<String>,
}

/// One line of captured debug output.
#[derive(Debug, Clone)]
pub struct ConsoleLine {
    pub category: String,
    pub text: String,
}

/// Resizable layout weights (percentages and ratios).
#[derive(Debug, Clone)]
pub struct LayoutConfig {
    /// Sidebar width as percentage of main row (15–45).
    pub sidebar_pct: u16,
    /// Vertical split inside sidebar: scopes vs stacks.
    pub scopes_weight: u32,
    pub stacks_weight: u32,
    /// Bottom tray height in terminal rows.
    pub bottom_height: u16,
    /// REPL width as percentage of bottom tray (20–60).
    pub repl_pct: u16,
}

impl Default for LayoutConfig {
    fn default() -> Self {
        Self {
            sidebar_pct: 25,
            scopes_weight: 3,
            stacks_weight: 2,
            bottom_height: 14,
            repl_pct: 30,
        }
    }
}

impl LayoutConfig {
    pub fn widen_sidebar(&mut self) {
        self.sidebar_pct = (self.sidebar_pct + 2).min(45);
    }

    pub fn narrow_sidebar(&mut self) {
        self.sidebar_pct = self.sidebar_pct.saturating_sub(2).max(15);
    }

    pub fn grow_scopes(&mut self) {
        self.scopes_weight = self.scopes_weight.saturating_add(1).min(8);
    }

    pub fn shrink_scopes(&mut self) {
        if self.scopes_weight > 1 {
            self.scopes_weight -= 1;
        }
    }

    pub fn grow_bottom(&mut self) {
        self.bottom_height = (self.bottom_height + 1).min(crate::ui::layout::BOTTOM_TRAY_MAX);
    }

    pub fn shrink_bottom(&mut self) {
        self.bottom_height = self
            .bottom_height
            .saturating_sub(1)
            .max(crate::ui::layout::BOTTOM_TRAY_MIN);
    }

    pub fn widen_repl(&mut self) {
        self.repl_pct = (self.repl_pct + 5).min(60);
    }

    pub fn narrow_repl(&mut self) {
        self.repl_pct = self.repl_pct.saturating_sub(5).max(20);
    }
}

/// Active divider drag (mouse resize).
#[derive(Debug, Clone, Copy)]
pub struct DividerDrag {
    pub divider: crate::ui::layout::DividerHit,
    pub last_x: u16,
    pub last_y: u16,
}

/// Snapshot of everything the UI needs to render one frame.
#[derive(Debug, Clone, Default)]
pub struct AppState {
    pub connection_state: ConnectionState,
    pub session_state: SessionState,
    pub focus: Focus,
    pub program: PathBuf,

    // Source panel
    pub source_path: Option<PathBuf>,
    pub source_lines: Vec<String>,
    /// Debugger execution line (from DAP stack frame).
    pub current_line: u32,
    /// Path where the debugger is currently stopped.
    pub execution_path: Option<PathBuf>,
    /// User cursor line in the open source file (1-based).
    pub source_cursor_line: u32,
    pub source_scroll: u16,
    pub source_mode: SourceMode,

    // File picker
    pub workspace_root: PathBuf,
    pub workspace_files: Vec<PathBuf>,
    pub file_picker_filter: String,
    pub file_picker_selected: usize,

    // DAP data
    pub threads: Vec<Thread>,
    pub stack_frames: Vec<StackFrame>,
    pub scopes: Vec<Scope>,
    pub variables: HashMap<i64, Vec<Variable>>,
    pub expanded_refs: HashSet<i64>,

    // Sidebar extras
    pub breakpoints: Vec<BreakpointEntry>,
    pub watches: Vec<WatchEntry>,
    pub next_watch_id: u64,

    // Bottom tray
    pub console_lines: Vec<ConsoleLine>,
    pub repl_input: String,
    pub repl_history: Vec<String>,

    // Selection indices per list panel
    pub scopes_selected: usize,
    pub stacks_selected: usize,
    pub breakpoints_selected: usize,
    pub watches_selected: usize,
    pub console_scroll: u16,

    pub status_message: String,

    /// Whether debug UI panels are visible (dapui.open/close).
    pub ui_open: bool,
    pub ui_config: UiConfig,

    /// Active floating window overlay.
    pub float: FloatKind,

    /// Hit areas for controls winbar buttons (updated each frame).
    pub control_areas: Vec<(Rect, crate::ui::panel::PanelAction)>,
    pub controls_hover: Option<usize>,

    /// Resizable layout + mouse interaction state.
    pub layout: LayoutConfig,
    pub layout_areas: crate::ui::layout::LayoutAreas,
    pub divider_drag: Option<DividerDrag>,
    pub hover_divider: Option<crate::ui::layout::DividerHit>,

    /// Live keyboard/mouse event inspector (F12 to toggle).
    pub input_debug: crate::ui::input_debug::InputDebug,
}

impl AppState {
    pub fn new(program: PathBuf) -> Self {
        let workspace_root = program
            .parent()
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("."));

        Self {
            program,
            workspace_root,
            source_cursor_line: 1,
            status_message: "Connecting to debugpy…".into(),
            connection_state: ConnectionState::Connecting,
            ui_open: true,
            ui_config: UiConfig::default(),
            layout: LayoutConfig::default(),
            input_debug: crate::ui::input_debug::InputDebug::new(),
            ..Default::default()
        }
    }

    pub fn is_connected(&self) -> bool {
        matches!(self.connection_state, ConnectionState::Connected)
    }

    pub fn refresh_workspace_files(&mut self) {
        self.workspace_files = crate::ui::workspace::list_source_files(&self.workspace_root);
        self.file_picker_selected = 0;
    }

    pub fn open_file_picker(&mut self) {
        self.refresh_workspace_files();
        self.file_picker_filter.clear();
        self.file_picker_selected = 0;
        self.source_mode = SourceMode::FilePicker;
        self.status_message = "File picker — type to filter, Enter to open, Esc to cancel".into();
    }

    pub fn close_file_picker(&mut self) {
        self.source_mode = SourceMode::View;
        self.file_picker_filter.clear();
    }

    /// Word at cursor line for eval (simple whitespace split).
    pub fn word_under_cursor(&self) -> String {
        let line_idx = self.source_cursor_line.saturating_sub(1) as usize;
        let Some(line) = self.source_lines.get(line_idx) else {
            return String::new();
        };
        line.split_whitespace()
            .find(|w| !w.is_empty())
            .unwrap_or("")
            .trim_matches(|c: char| !c.is_alphanumeric() && c != '_' && c != '.')
            .to_string()
    }

    pub fn open_ui(&mut self) {
        self.ui_open = true;
        self.status_message = "UI opened".into();
    }

    pub fn close_ui(&mut self) {
        self.ui_open = false;
        self.float = FloatKind::None;
        self.status_message = "UI closed (o to reopen)".into();
    }

    pub fn active_frame(&self) -> Option<&StackFrame> {
        self.stack_frames.first()
    }

    pub fn cycle_focus_next(&mut self) {
        self.focus = match self.focus {
            Focus::Source => Focus::Scopes,
            Focus::Scopes => Focus::Breakpoints,
            Focus::Breakpoints => Focus::Stacks,
            Focus::Stacks => Focus::Watches,
            Focus::Watches => Focus::Repl,
            Focus::Repl => Focus::Console,
            Focus::Console => Focus::Source,
        };
    }

    pub fn cycle_focus_prev(&mut self) {
        self.focus = match self.focus {
            Focus::Source => Focus::Console,
            Focus::Console => Focus::Repl,
            Focus::Repl => Focus::Watches,
            Focus::Watches => Focus::Stacks,
            Focus::Stacks => Focus::Breakpoints,
            Focus::Breakpoints => Focus::Scopes,
            Focus::Scopes => Focus::Source,
        };
    }
}
