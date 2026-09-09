pub mod app;
pub mod chrome;
pub mod controls;
pub mod float;
pub mod icons;
pub mod input_debug;
pub mod layout;
pub mod panel;
pub mod resize;
pub mod panels;
pub mod state;
pub mod sync;
pub mod terminal;
pub mod theme;
pub mod workspace;

pub use app::run_tui;
pub use float::{close as close_float, open_element};
pub use state::{AppState, FloatElement, Focus};
