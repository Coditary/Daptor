//! Scopes and variables tree panel.

use ratatui::crossterm::event::{KeyCode, KeyEvent, KeyModifiers};
use ratatui::layout::Rect;
use ratatui::widgets::{List, ListItem};
use ratatui::Frame;

use crate::dap::protocol::Variable;
use crate::session::DebugSession;
use crate::ui::chrome::panel_block;
use crate::ui::icons::{COLLAPSED, EXPANDED};
use crate::ui::layout::is_focused;
use crate::ui::panel::{Panel, PanelAction};
use crate::ui::state::{AppState, Focus};
use crate::ui::theme;

/// One visible row in the scopes/variables list.
#[derive(Debug, Clone)]
struct FlatRow {
    label: String,
    /// When set, Enter/Space toggles expansion for this reference.
    toggle_ref: Option<i64>,
    /// Variable name for edit/repl actions.
    var_name: Option<String>,
}

#[derive(Debug, Default)]
pub struct ScopesPanel;

impl ScopesPanel {
    fn build_rows(state: &AppState) -> Vec<FlatRow> {
        let mut rows = Vec::new();

        for scope in &state.scopes {
            rows.push(FlatRow {
                label: format!("{}:", scope.name),
                toggle_ref: None,
                var_name: None,
            });

            if let Some(vars) = state.variables.get(&scope.variables_reference) {
                append_variables(state, vars, 0, &mut rows);
            }
        }

        rows
    }

    fn clamp_selection(state: &mut AppState, row_count: usize) {
        if row_count == 0 {
            state.scopes_selected = 0;
        } else if state.scopes_selected >= row_count {
            state.scopes_selected = row_count - 1;
        }
    }

    fn toggle_expand(
        state: &mut AppState,
        session: &mut DebugSession,
        variables_reference: i64,
    ) {
        if state.expanded_refs.contains(&variables_reference) {
            state.expanded_refs.remove(&variables_reference);
        } else {
            state.expanded_refs.insert(variables_reference);
            if !state.variables.contains_key(&variables_reference) {
                if let Ok(vars) = session.variables(variables_reference) {
                    state.variables.insert(variables_reference, vars);
                }
            }
        }
    }
}

fn append_variables(state: &AppState, vars: &[Variable], depth: usize, rows: &mut Vec<FlatRow>) {
    let indent = "  ".repeat(depth);

    for var in vars {
        let prefix = if var.variables_reference > 0 {
            if state.expanded_refs.contains(&var.variables_reference) {
                format!("{} ", EXPANDED)
            } else {
                format!("{} ", COLLAPSED)
            }
        } else {
            "  ".into()
        };

        let type_suffix = var
            .type_name
            .as_ref()
            .map(|t| format!(" ({t})"))
            .unwrap_or_default();

        rows.push(FlatRow {
            label: format!("{}{}{} = {}{}", indent, prefix, var.name, var.value, type_suffix),
            toggle_ref: if var.variables_reference > 0 {
                Some(var.variables_reference)
            } else {
                None
            },
            var_name: Some(var.name.clone()),
        });

        if var.variables_reference > 0 && state.expanded_refs.contains(&var.variables_reference) {
            if let Some(children) = state.variables.get(&var.variables_reference) {
                append_variables(state, children, depth + 1, rows);
            } else {
                rows.push(FlatRow {
                    label: format!("{}  …", indent),
                    toggle_ref: None,
                    var_name: None,
                });
            }
        }
    }
}

impl Panel for ScopesPanel {
    fn id(&self) -> &'static str {
        "scopes"
    }

    fn title(&self) -> &'static str {
        "Scopes"
    }

    fn render(&mut self, frame: &mut Frame, area: Rect, state: &AppState) {
        let focused = is_focused(state, Focus::Scopes);
        let rows = Self::build_rows(state);

        let items: Vec<ListItem> = rows
            .iter()
            .enumerate()
            .map(|(i, row)| {
                let is_header = row.toggle_ref.is_none() && row.var_name.is_none();
                let mut style = if is_header {
                    theme::scope_header()
                } else {
                    theme::source_text()
                };

                if i == state.scopes_selected && focused {
                    style = theme::selection();
                }

                ListItem::new(row.label.as_str()).style(style)
            })
            .collect();

        let list = List::new(items).block(panel_block("Scopes", focused));
        frame.render_widget(list, area);
    }

    fn handle_key(
        &mut self,
        key: KeyEvent,
        state: &mut AppState,
        session: &mut DebugSession,
    ) -> PanelAction {
        let rows = Self::build_rows(state);
        Self::clamp_selection(state, rows.len());

        if key.modifiers.contains(KeyModifiers::CONTROL)
            || key.modifiers.contains(KeyModifiers::ALT)
        {
            return PanelAction::None;
        }

        match key.code {
            KeyCode::Char('j') | KeyCode::Down => {
                if !rows.is_empty() && state.scopes_selected + 1 < rows.len() {
                    state.scopes_selected += 1;
                }
            }
            KeyCode::Char('k') | KeyCode::Up => {
                if state.scopes_selected > 0 {
                    state.scopes_selected -= 1;
                }
            }
            KeyCode::Enter | KeyCode::Char(' ') => {
                if let Some(row) = rows.get(state.scopes_selected) {
                    if let Some(ref_id) = row.toggle_ref {
                        Self::toggle_expand(state, session, ref_id);
                    }
                }
            }
            KeyCode::Char('e') => {
                if let Some(row) = rows.get(state.scopes_selected) {
                    if let Some(name) = &row.var_name {
                        state.status_message = format!("Edit variable: {name}");
                    }
                }
            }
            KeyCode::Char('r') => {
                if let Some(row) = rows.get(state.scopes_selected) {
                    if let Some(name) = &row.var_name {
                        state.status_message = format!("Send to REPL: {name}");
                    }
                }
            }
            _ => {}
        }

        PanelAction::None
    }
}
