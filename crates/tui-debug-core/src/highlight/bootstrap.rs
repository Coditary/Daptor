use std::sync::Once;

use anyhow::Result;

use super::tree_sitter::{language_dir, normalize_language, register_language_from_dir, register_language_static};

static INIT: Once = Once::new();

/// Register built-in and external tree-sitter grammars once per process.
pub fn init() {
    INIT.call_once(bootstrap_languages);
}

fn bootstrap_languages() {
    let _ = register_builtin_python();
    let _ = register_builtin_c();
    let _ = register_builtin_cpp();
    let _ = register_builtin_rust();

    // External grammars in DAP_TREE_SITTER_DIR override the built-ins when present.
    for language in ["python", "c", "cpp", "rust"] {
        let _ = try_load_external_grammar(language);
    }
}

fn try_load_external_grammar(language: &str) -> Result<()> {
    let language = normalize_language(language);
    let dir = language_dir(language);
    if !dir.is_dir() {
        anyhow::bail!("grammar directory not found: {}", dir.display());
    }
    register_language_from_dir(language, &dir)
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

fn register_builtin_c() -> Result<()> {
    register_language_static(
        "c",
        tree_sitter_c::LANGUAGE.into(),
        tree_sitter_c::HIGHLIGHT_QUERY,
        "",
        "",
    )
}

fn register_builtin_cpp() -> Result<()> {
    // tree-sitter-cpp's stock query omits most shared C syntax; the C highlights
    // query matches the same node names on the C++ grammar.
    let highlights_query = format!(
        "{}{}",
        tree_sitter_c::HIGHLIGHT_QUERY,
        include_str!("cpp_highlights_extra.scm"),
    );
    register_language_static(
        "cpp",
        tree_sitter_cpp::LANGUAGE.into(),
        &highlights_query,
        "",
        "",
    )
}

fn register_builtin_rust() -> Result<()> {
    register_language_static(
        "rust",
        tree_sitter_rust::LANGUAGE.into(),
        tree_sitter_rust::HIGHLIGHTS_QUERY,
        tree_sitter_rust::INJECTIONS_QUERY,
        "",
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::highlight::{function_name_at_line, highlight_viewport, language_is_loaded};

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

    #[test]
    fn init_registers_c_for_function_detection() {
        init();
        assert!(language_is_loaded("c"));

        let source = "static int add(int a, int b) {\n    return a + b;\n}\n";
        assert_eq!(
            function_name_at_line("c", source, 1),
            Some("add".to_string())
        );
    }
}
