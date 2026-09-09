//! Shared panel chrome matching nvim-dap-ui window borders.

use ratatui::layout::Rect;
use ratatui::style::Style;
use ratatui::widgets::{Block, Borders};
use ratatui::Frame;

use crate::ui::theme::{self, bg};

pub fn panel_block(title: &str, focused: bool) -> Block<'static> {
    let border_style = if focused {
        theme::border_focused()
    } else {
        theme::border_normal()
    };
    Block::default()
        .title(format!(" {title} "))
        .borders(Borders::ALL)
        .border_style(border_style)
        .style(Style::default().bg(bg::PANEL))
}

pub fn fill_background(frame: &mut Frame, area: Rect) {
    frame.render_widget(
        Block::default().style(Style::default().bg(bg::ROOT)),
        area,
    );
}
