//! Syntax highlighting via tree-sitter grammars loaded from shared libraries.

mod bootstrap;
mod spans;
mod tree_sitter;
mod viewport;

pub use bootstrap::init;
pub use spans::{HighlightKind, HighlightedLine, StyledSpan};
pub use tree_sitter::{
    default_tree_sitter_dir, language_is_loaded, language_setup_hint, parse_snippet,
    register_language_from_dir, register_language_static, try_load_language,
};
pub use viewport::highlight_viewport;
