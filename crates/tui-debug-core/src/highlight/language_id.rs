//! File-extension → grammar id mapping and highlight fallback chains.

/// Return the file extension (lowercase) from a path, if any.
pub fn file_extension(path: &str) -> Option<&str> {
    let file_name = path.rsplit(['/', '\\']).next().unwrap_or(path);
    let dot = file_name.rfind('.')?;
    let ext = file_name.get(dot + 1..)?;
    if ext.is_empty() {
        None
    } else {
        Some(ext)
    }
}

/// Infer a tree-sitter language id from a source path.
pub fn language_from_path(path: &str) -> &str {
    match file_extension(path) {
        Some("py" | "pyw") => "python",
        Some("rs") => "rust",
        Some("c") => "c",
        Some("cpp" | "cc" | "cxx" | "C") => "cpp",
        Some("hpp" | "hh" | "hxx" | "h++" | "H") => "cpp",
        Some("h") => "c",
        Some("js" | "mjs" | "cjs" | "jsx") => "javascript",
        Some("ts" | "tsx") => "typescript",
        Some(ext) => ext,
        None => "python",
    }
}

/// Map aliases and extension ids to registered grammar keys.
pub fn normalize_language(language: &str) -> &str {
    match language {
        "cc" | "cxx" | "hpp" | "hh" | "hxx" | "h++" | "C" | "H" => "cpp",
        "h" => "c",
        "js" | "mjs" | "cjs" | "jsx" => "javascript",
        "ts" | "tsx" => "typescript",
        "py" | "pyw" => "python",
        _ => language,
    }
}

/// Languages to try when highlighting, best match first.
pub fn highlight_language_candidates(language: &str) -> Vec<String> {
    let primary = normalize_language(language);
    let mut candidates = vec![primary.to_string()];
    for fallback in highlight_fallback_ids(primary) {
        if !candidates.iter().any(|id| id == fallback) {
            candidates.push(fallback.to_string());
        }
    }
    candidates
}

fn highlight_fallback_ids(language: &str) -> &'static [&'static str] {
    match language {
        // C++ grammars share most node names with C; use C highlighting when C++ is thin.
        "cpp" => &["c"],
        // Unregistered web grammars: no built-in fallback yet (external via DAP_TREE_SITTER_DIR).
        "javascript" | "typescript" => &[],
        _ => &[],
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn highlight_candidates_include_cpp_c_fallback() {
        let candidates = highlight_language_candidates("cpp");
        assert_eq!(candidates, vec!["cpp".to_string(), "c".to_string()]);
    }

    #[test]
    fn header_extensions_map_to_c_family() {
        assert_eq!(language_from_path("widget.h"), "c");
        assert_eq!(language_from_path("widget.hpp"), "cpp");
        assert_eq!(normalize_language("hpp"), "cpp");
        assert_eq!(normalize_language("h"), "c");
    }
}
