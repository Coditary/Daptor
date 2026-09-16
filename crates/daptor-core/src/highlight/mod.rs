//! Syntax highlighting via tree-sitter grammars loaded from shared libraries.

mod bootstrap;
mod function_at_line;
mod identifier_at_position;
mod language_id;
mod spans;
mod tree_sitter;
mod viewport;

pub use bootstrap::init;
pub use function_at_line::{function_definition_line, function_name_at_line};
pub use identifier_at_position::{identifier_at_position, IdentifierAtPosition};
pub use language_id::{file_extension, highlight_language_candidates, language_from_path};
pub use spans::{HighlightKind, HighlightedLine, StyledSpan};
pub use tree_sitter::{
    default_tree_sitter_dir, language_is_loaded, language_setup_hint, normalize_language,
    parse_snippet, register_language_static, set_tree_sitter_dir, try_load_language,
};
pub use viewport::highlight_viewport;
