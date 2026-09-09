//! Mouse + keyboard helpers for resizing panes.

use ratatui::crossterm::event::{
    KeyCode, KeyEvent, KeyModifiers, MouseButton, MouseEvent, MouseEventKind,
};

use crate::ui::layout::{DividerHit, LayoutAreas, BOTTOM_TRAY_MAX, BOTTOM_TRAY_MIN};
use crate::ui::state::{AppState, DividerDrag, Focus};

fn layout_resize_modifier(key: &KeyEvent) -> bool {
    let alt = key.modifiers.contains(KeyModifiers::ALT);
    let ctrl_shift = key.modifiers.contains(KeyModifiers::CONTROL)
        && key.modifiers.contains(KeyModifiers::SHIFT);
    alt || ctrl_shift
}

pub fn handle_layout_key(key: KeyEvent, state: &mut AppState) -> bool {
    if !layout_resize_modifier(&key) {
        return false;
    }

    match key.code {
        KeyCode::Left => {
            state.layout.narrow_sidebar();
            state.status_message = format!("Sidebar {}%", state.layout.sidebar_pct);
            true
        }
        KeyCode::Right => {
            state.layout.widen_sidebar();
            state.status_message = format!("Sidebar {}%", state.layout.sidebar_pct);
            true
        }
        KeyCode::Up => {
            state.layout.shrink_bottom();
            state.status_message = format!("Bottom tray {} rows", state.layout.bottom_height);
            true
        }
        KeyCode::Down => {
            state.layout.grow_bottom();
            state.status_message = format!("Bottom tray {} rows", state.layout.bottom_height);
            true
        }
        KeyCode::Char('[') => {
            state.layout.narrow_repl();
            state.status_message = format!("REPL {}%", state.layout.repl_pct);
            true
        }
        KeyCode::Char(']') => {
            state.layout.widen_repl();
            state.status_message = format!("REPL {}%", state.layout.repl_pct);
            true
        }
        _ => false,
    }
}

pub fn handle_scopes_resize_key(key: KeyEvent, state: &mut AppState) -> bool {
    if state.focus != Focus::Scopes {
        return false;
    }
    let ctrl = key.modifiers.contains(KeyModifiers::CONTROL);
    let ctrl_shift = ctrl && key.modifiers.contains(KeyModifiers::SHIFT);
    if !ctrl && !ctrl_shift {
        return false;
    }
    match key.code {
        KeyCode::Up => {
            state.layout.grow_scopes();
            state.status_message = "Scopes pane enlarged".into();
            true
        }
        KeyCode::Down => {
            state.layout.shrink_scopes();
            state.status_message = "Scopes pane shrunk".into();
            true
        }
        _ => false,
    }
}

pub fn divider_at_mouse(mouse: &MouseEvent, areas: &LayoutAreas) -> Option<DividerHit> {
    crate::ui::layout::divider_at(mouse.column, mouse.row, areas)
}

pub fn update_hover_divider(state: &mut AppState, mouse: &MouseEvent, areas: &LayoutAreas) {
    state.hover_divider = divider_at_mouse(mouse, areas);
}

fn drag_message(divider: DividerHit) -> String {
    match divider {
        DividerHit::Sidebar => "Dragging sidebar divider".into(),
        DividerHit::BottomTray => "Dragging bottom divider".into(),
        DividerHit::ReplConsole => "Dragging REPL divider".into(),
        DividerHit::ScopesStacks => "Dragging scopes divider".into(),
    }
}

/// Map a screen coordinate to layout weights while dragging a divider.
pub fn apply_divider_position(
    state: &mut AppState,
    divider: DividerHit,
    x: u16,
    y: u16,
    areas: &LayoutAreas,
) {
    match divider {
        DividerHit::Sidebar => {
            let root = areas.root;
            if root.width <= 1 {
                return;
            }
            let rel = (x.saturating_sub(root.x) as f32) / root.width as f32;
            state.layout.sidebar_pct = (rel * 100.0).clamp(15.0, 45.0) as u16;
        }
        DividerHit::BottomTray => {
            let root = areas.root;
            let max_main = root.height.saturating_sub(BOTTOM_TRAY_MIN + 1);
            let min_main = 8;
            let main_h = y.saturating_sub(root.y).clamp(min_main, max_main);
            let new_bottom = root.height.saturating_sub(main_h).saturating_sub(1);
            state.layout.bottom_height = new_bottom.clamp(BOTTOM_TRAY_MIN, BOTTOM_TRAY_MAX);
        }
        DividerHit::ReplConsole => {
            let total_w = areas.repl.width.saturating_add(areas.console.width);
            if total_w == 0 {
                return;
            }
            let rel = (x.saturating_sub(areas.repl.x) as f32) / total_w as f32;
            state.layout.repl_pct = (rel * 100.0).clamp(20.0, 60.0) as u16;
        }
        DividerHit::ScopesStacks => {
            let total_h = areas.scopes.height.saturating_add(areas.stacks.height);
            if total_h == 0 {
                return;
            }
            let rel = (y.saturating_sub(areas.scopes.y) as f32) / total_h as f32;
            let scopes = (rel * 8.0).round().clamp(1.0, 7.0) as u32;
            state.layout.scopes_weight = scopes;
            state.layout.stacks_weight = (8 - scopes).max(1);
        }
    }
}

pub fn begin_divider_drag(state: &mut AppState, mouse: &MouseEvent, areas: &LayoutAreas) -> bool {
    let Some(divider) = divider_at_mouse(mouse, areas) else {
        return false;
    };
    state.divider_drag = Some(DividerDrag {
        divider,
        last_x: mouse.column,
        last_y: mouse.row,
    });
    apply_divider_position(state, divider, mouse.column, mouse.row, areas);
    state.status_message = drag_message(divider);
    true
}

pub fn set_focus_from_mouse(state: &mut AppState, areas: &LayoutAreas, mouse: &MouseEvent) -> bool {
    let Some(focus) = areas.focus_at(mouse.column, mouse.row) else {
        return false;
    };
    state.focus = focus;
    state.status_message = format!("Focus: {focus:?} — type to interact");
    true
}

pub fn handle_mouse_drag(state: &mut AppState, mouse: &MouseEvent, areas: &LayoutAreas) {
    let Some(drag) = state.divider_drag else {
        return;
    };

    if mouse.column == drag.last_x && mouse.row == drag.last_y {
        return;
    }

    apply_divider_position(state, drag.divider, mouse.column, mouse.row, areas);

    state.divider_drag = Some(DividerDrag {
        divider: drag.divider,
        last_x: mouse.column,
        last_y: mouse.row,
    });
}

pub fn handle_mouse_up(state: &mut AppState) {
    if state.divider_drag.is_some() {
        state.status_message = "Resize done".into();
    }
    state.divider_drag = None;
}

pub fn is_dragging(state: &AppState) -> bool {
    state.divider_drag.is_some()
}

pub fn active_divider(state: &AppState) -> Option<DividerHit> {
    state.divider_drag.map(|drag| drag.divider)
}

/// Some terminals emit `Drag` without a preceding `Down`, or lose the matching `Up`.
/// Only `Drag` events are handled — never enable xterm `?1003` all-motion (TTY deadlock risk).
pub fn handle_mouse_motion(state: &mut AppState, mouse: &MouseEvent, areas: &LayoutAreas) {
    match mouse.kind {
        MouseEventKind::Drag(MouseButton::Left) if !is_dragging(state) => {
            begin_divider_drag(state, mouse, areas);
        }
        MouseEventKind::Drag(_) if is_dragging(state) => {
            handle_mouse_drag(state, mouse, areas);
        }
        _ => {}
    }
}

pub fn clear_stale_drag_on_press(state: &mut AppState) {
    if is_dragging(state) {
        handle_mouse_up(state);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::ui::state::AppState;
    use ratatui::layout::Rect;
    use std::path::PathBuf;

    #[test]
    fn apply_sidebar_drag_sets_percentage_from_x() {
        let mut state = AppState::new(PathBuf::from("fixtures/hello.py"));
        let areas = LayoutAreas {
            root: Rect::new(0, 0, 100, 40),
            sidebar: Rect::new(0, 0, 25, 30),
            source: Rect::new(25, 0, 75, 30),
            ..Default::default()
        };

        apply_divider_position(&mut state, DividerHit::Sidebar, 30, 0, &areas);
        assert_eq!(state.layout.sidebar_pct, 30);
    }

    #[test]
    fn apply_bottom_drag_sets_row_height() {
        let mut state = AppState::new(PathBuf::from("fixtures/hello.py"));
        let areas = LayoutAreas {
            root: Rect::new(0, 0, 100, 40),
            ..Default::default()
        };

        apply_divider_position(&mut state, DividerHit::BottomTray, 0, 26, &areas);
        assert_eq!(state.layout.bottom_height, 13);
    }
}
