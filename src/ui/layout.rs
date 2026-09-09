//! nvim-dap-ui default layout translated to ratatui constraints.

use ratatui::layout::{Constraint, Direction, Layout, Rect};
use ratatui::style::Style;
use ratatui::Frame;

use crate::ui::state::{AppState, Focus, LayoutConfig};
use crate::ui::theme;

/// Bottom tray: controls row + repl/console (like nvim winbar + console).
pub const BOTTOM_TRAY_MIN: u16 = 6;
pub const BOTTOM_TRAY_MAX: u16 = 40;

/// Pixels around a divider that respond to mouse grab (terminals are imprecise).
const DIVIDER_GRAB: u16 = 8;

/// Main regions: sidebar | source, bottom tray, status bar.
pub fn root_layout(area: Rect, layout: &LayoutConfig) -> [Rect; 4] {
    let vertical = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Min(8),
            Constraint::Length(layout.bottom_height),
            Constraint::Length(1),
        ])
        .split(area);

    let main = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage(layout.sidebar_pct),
            Constraint::Min(20),
        ])
        .split(vertical[0]);

    [main[0], main[1], vertical[1], vertical[2]]
}

/// Sidebar: scopes + stacks dominant; hide breakpoints/watches when empty.
pub fn sidebar_layout(
    area: Rect,
    show_breakpoints: bool,
    show_watches: bool,
    layout: &LayoutConfig,
) -> [Rect; 4] {
    let bp = if show_breakpoints { 1u32 } else { 0 };
    let watches = if show_watches { 1u32 } else { 0 };
    let scopes = layout.scopes_weight.max(1);
    let stacks = layout.stacks_weight.max(1);
    let total = scopes + bp + stacks + watches;
    let chunks = Layout::default()
        .direction(Direction::Vertical)
        .constraints([
            Constraint::Ratio(scopes, total),
            Constraint::Ratio(bp, total),
            Constraint::Ratio(stacks, total),
            Constraint::Ratio(watches, total),
        ])
        .split(area);
    [chunks[0], chunks[1], chunks[2], chunks[3]]
}

/// Bottom: full-width controls row, then repl + console.
pub fn bottom_tray_layout(area: Rect, layout: &LayoutConfig) -> (Rect, [Rect; 2]) {
    let vertical = Layout::default()
        .direction(Direction::Vertical)
        .constraints([Constraint::Length(1), Constraint::Min(3)])
        .split(area);

    let horizontal = Layout::default()
        .direction(Direction::Horizontal)
        .constraints([
            Constraint::Percentage(layout.repl_pct),
            Constraint::Min(10),
        ])
        .split(vertical[1]);

    (vertical[0], [horizontal[0], horizontal[1]])
}

pub fn is_focused(state: &AppState, focus: Focus) -> bool {
    state.focus == focus
}

/// Which resizable divider sits on this screen coordinate.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum DividerHit {
    Sidebar,
    BottomTray,
    ReplConsole,
    ScopesStacks,
}

pub fn divider_at(x: u16, y: u16, areas: &LayoutAreas) -> Option<DividerHit> {
    // Check smaller / inner dividers first so overlapping zones prefer the intended split.
    if grab_zone(areas.repl_divider, x, y) {
        return Some(DividerHit::ReplConsole);
    }
    if grab_zone(areas.scopes_divider, x, y) {
        return Some(DividerHit::ScopesStacks);
    }
    if grab_zone(areas.sidebar_divider, x, y) {
        return Some(DividerHit::Sidebar);
    }
    if grab_zone(areas.bottom_divider, x, y) {
        return Some(DividerHit::BottomTray);
    }
    None
}

fn grab_zone(zone: Rect, x: u16, y: u16) -> bool {
    if zone.width == 0 || zone.height == 0 {
        return false;
    }
    x >= zone.x && x < zone.x + zone.width && y >= zone.y && y < zone.y + zone.height
}

fn divider_strip(x: u16, y: u16, width: u16, height: u16) -> Rect {
    Rect::new(x, y, width.max(1), height.max(1))
}

/// Cached geometry for mouse hit-testing (updated each frame).
#[derive(Debug, Clone, Default)]
pub struct LayoutAreas {
    pub root: Rect,
    pub sidebar: Rect,
    pub source: Rect,
    pub bottom: Rect,
    pub scopes: Rect,
    pub stacks: Rect,
    pub repl: Rect,
    pub console: Rect,
    pub controls: Rect,
    pub sidebar_divider: Rect,
    pub bottom_divider: Rect,
    pub repl_divider: Rect,
    pub scopes_divider: Rect,
}

impl LayoutAreas {
    pub fn compute(area: Rect, state: &AppState) -> Self {
        let show_bp = !state.breakpoints.is_empty();
        let show_watches = !state.watches.is_empty();
        let [sidebar, source, bottom, _status] = root_layout(area, &state.layout);
        let [scopes, _bp, stacks, _watches] =
            sidebar_layout(sidebar, show_bp, show_watches, &state.layout);
        let (controls, [repl, console]) = bottom_tray_layout(bottom, &state.layout);

        // Boundary between sidebar and source (vertical).
        let split_x = source.x;
        let sidebar_divider = divider_strip(
            split_x.saturating_sub(DIVIDER_GRAB),
            sidebar.y.min(source.y),
            DIVIDER_GRAB * 2 + 1,
            sidebar.height.max(source.height),
        );

        // Boundary between main area and bottom tray (horizontal).
        let split_y = bottom.y;
        let bottom_divider = divider_strip(
            area.x,
            split_y.saturating_sub(DIVIDER_GRAB),
            area.width,
            DIVIDER_GRAB * 2 + 1,
        );

        // Boundary between REPL and console (vertical).
        let split_repl_x = console.x;
        let repl_divider = divider_strip(
            split_repl_x.saturating_sub(DIVIDER_GRAB),
            repl.y.min(console.y),
            DIVIDER_GRAB * 2 + 1,
            repl.height.max(console.height),
        );

        // Boundary between scopes and stacks (horizontal split in sidebar).
        let scopes_stacks_y = stacks.y;
        let scopes_divider = if scopes.height > 0 && stacks.height > 0 {
            divider_strip(
                sidebar.x,
                scopes_stacks_y.saturating_sub(DIVIDER_GRAB),
                sidebar.width,
                DIVIDER_GRAB * 2 + 1,
            )
        } else {
            Rect::default()
        };

        Self {
            root: area,
            sidebar,
            source,
            bottom,
            scopes,
            stacks,
            repl,
            console,
            controls,
            sidebar_divider,
            bottom_divider,
            repl_divider,
            scopes_divider,
        }
    }

    /// Draw grab handles on split boundaries so dividers are visible and easy to hit.
    pub fn render_dividers(
        frame: &mut Frame,
        areas: &LayoutAreas,
        hover: Option<DividerHit>,
        active: Option<DividerHit>,
    ) {
        render_divider_strip(
            frame,
            areas.sidebar_divider,
            DividerHit::Sidebar,
            hover,
            active,
        );
        render_divider_strip(
            frame,
            areas.bottom_divider,
            DividerHit::BottomTray,
            hover,
            active,
        );
        render_divider_strip(
            frame,
            areas.repl_divider,
            DividerHit::ReplConsole,
            hover,
            active,
        );
        if areas.scopes_divider.width > 0 && areas.scopes_divider.height > 0 {
            render_divider_strip(
                frame,
                areas.scopes_divider,
                DividerHit::ScopesStacks,
                hover,
                active,
            );
        }
    }

    pub fn focus_at(&self, x: u16, y: u16) -> Option<Focus> {
        if contains(self.controls, x, y) {
            return None;
        }
        if contains(self.scopes, x, y) {
            return Some(Focus::Scopes);
        }
        if contains(self.stacks, x, y) {
            return Some(Focus::Stacks);
        }
        if contains(self.repl, x, y) {
            return Some(Focus::Repl);
        }
        if contains(self.console, x, y) {
            return Some(Focus::Console);
        }
        if contains(self.source, x, y) {
            return Some(Focus::Source);
        }
        None
    }

    /// Map a click in the source panel to a 1-based line number (gutter/code area).
    pub fn source_line_at(&self, state: &AppState, x: u16, y: u16) -> Option<u32> {
        if !contains(self.source, x, y) {
            return None;
        }
        // Account for block border (1 row) + title (1 row) in typical panel block.
        let inner_top = self.source.y + 1;
        if y < inner_top {
            return None;
        }
        let inner_y = y.saturating_sub(inner_top);
        let line = state.source_scroll as u32 + inner_y as u32 + 1;
        if line <= state.source_lines.len() as u32 {
            Some(line)
        } else {
            None
        }
    }

    pub fn is_gutter_click(&self, x: u16, y: u16) -> bool {
        if !contains(self.source, x, y) {
            return false;
        }
        let rel_x = x.saturating_sub(self.source.x + 1);
        rel_x <= 8
    }
}

fn divider_style(hit: DividerHit, hover: Option<DividerHit>, active: Option<DividerHit>) -> Style {
    if active == Some(hit) {
        theme::divider_active()
    } else if hover == Some(hit) {
        theme::divider_hover()
    } else {
        theme::divider_idle()
    }
}

fn render_divider_strip(
    frame: &mut Frame,
    zone: Rect,
    hit: DividerHit,
    hover: Option<DividerHit>,
    active: Option<DividerHit>,
) {
    if zone.width == 0 || zone.height == 0 {
        return;
    }

    let style = divider_style(hit, hover, active);
    let ch = match hit {
        DividerHit::Sidebar | DividerHit::ReplConsole => '┃',
        DividerHit::BottomTray | DividerHit::ScopesStacks => '━',
    };

    let center_x = zone.x + zone.width / 2;
    let center_y = zone.y + zone.height / 2;

    match hit {
        DividerHit::Sidebar | DividerHit::ReplConsole => {
            for y in zone.y..zone.y + zone.height {
                if let Some(cell) = frame.buffer_mut().cell_mut((center_x, y)) {
                    cell.set_char(ch).set_style(style);
                }
            }
        }
        DividerHit::BottomTray | DividerHit::ScopesStacks => {
            for x in zone.x..zone.x + zone.width {
                if let Some(cell) = frame.buffer_mut().cell_mut((x, center_y)) {
                    cell.set_char(ch).set_style(style);
                }
            }
        }
    }
}

fn contains(rect: Rect, x: u16, y: u16) -> bool {
    rect.width > 0
        && rect.height > 0
        && x >= rect.x
        && x < rect.x + rect.width
        && y >= rect.y
        && y < rect.y + rect.height
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::state::AppState;
    use std::path::PathBuf;

    #[test]
    fn divider_hitboxes_cover_layout_splits() {
        let state = AppState::new(PathBuf::from("fixtures/hello.py"));
        let area = Rect::new(0, 0, 120, 40);
        let areas = LayoutAreas::compute(area, &state);

        assert!(divider_at(areas.source.x, areas.source.y + 2, &areas).is_some());
        assert_eq!(
            divider_at(areas.source.x, areas.source.y + 2, &areas),
            Some(DividerHit::Sidebar)
        );

        assert_eq!(
            divider_at(areas.console.x, areas.console.y + 1, &areas),
            Some(DividerHit::ReplConsole)
        );

        assert_eq!(
            divider_at(areas.source.x + 10, areas.bottom.y, &areas),
            Some(DividerHit::BottomTray)
        );

        assert_eq!(
            divider_at(areas.sidebar.x + 2, areas.stacks.y, &areas),
            Some(DividerHit::ScopesStacks)
        );
    }
}
