use std::sync::Once;

use anyhow::Result;

use super::tree_sitter::{register_language_static, try_load_language};

static INIT: Once = Once::new();

/// Register built-in and external tree-sitter grammars once per process.
pub fn init() {
    INIT.call_once(bootstrap_languages);
}

fn bootstrap_languages() {
    // External grammars in DAP_TREE_SITTER_DIR take precedence when present.
    if try_load_language("python").is_err() {
        let _ = register_builtin_python();
    }
}

fn register_builtin_python() -> Result<()> {
    register_language_static(
        "python",
        tree_sitter_python::LANGUAGE.into(),
        tree_sitter_python::HIGHLIGHTS_QUERY,
        "",
        "",
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::highlight::{highlight_viewport, language_is_loaded};

    #[test]
    fn init_registers_python_when_external_grammar_missing() {
        init();
        assert!(language_is_loaded("python"));

        let lines = highlight_viewport("python", "def main():\n    return 1\n", 1, 2);
        assert_eq!(lines.len(), 2);
        assert!(lines[0]
            .spans
            .iter()
            .any(|span| span.kind == crate::highlight::HighlightKind::Keyword));
    }
}
