//! Resolve function names at a source line using tree-sitter.

use anyhow::Result;
use tree_sitter::{Node, Tree};

use super::tree_sitter::{normalize_language, try_load_language};

fn parse_source(language: &str, source: &str) -> Result<Tree> {
    let language = normalize_language(language);
    try_load_language(language).map_err(|err| anyhow::anyhow!(err))?;
    super::tree_sitter::parse_snippet(language, source)
}

fn node_name_text(node: Node, source: &[u8]) -> Option<String> {
    node.utf8_text(source)
        .ok()
        .map(|text| text.to_string())
        .filter(|text| !text.is_empty())
}

fn identifier_from_declarator(node: Node, source: &[u8]) -> Option<String> {
    match node.kind() {
        "identifier" | "field_identifier" | "type_identifier" | "operator_name" | "destructor_name" => {
            node_name_text(node, source)
        }
        "pointer_declarator" | "reference_declarator" | "array_declarator" | "function_declarator"
        | "parenthesized_declarator" | "qualified_identifier" | "template_function" => {
            for index in 0..node.child_count() {
                let child = node.child(index)?;
                if let Some(name) = identifier_from_declarator(child, source) {
                    return Some(name);
                }
            }
            None
        }
        _ => {
            if let Some(name_node) = node.child_by_field_name("declarator") {
                if let Some(name) = identifier_from_declarator(name_node, source) {
                    return Some(name);
                }
            }
            for index in 0..node.child_count() {
                let child = node.child(index)?;
                if let Some(name) = identifier_from_declarator(child, source) {
                    return Some(name);
                }
            }
            None
        }
    }
}

fn is_function_definition_kind(kind: &str) -> bool {
    matches!(
        kind,
        "function_definition"
            | "function_item"
            | "method_definition"
            | "function_declaration"
            | "generator_function_declaration"
    )
}

fn function_name_from_definition_node(node: Node, source: &[u8]) -> Option<String> {
    if let Some(name_node) = node.child_by_field_name("name") {
        if let Some(name) = node_name_text(name_node, source) {
            return Some(name);
        }
    }
    if let Some(declarator) = node.child_by_field_name("declarator") {
        if let Some(name) = identifier_from_declarator(declarator, source) {
            return Some(name);
        }
    }
    None
}

fn line_matches_function_node(node: Node, target_row: usize) -> bool {
    if node.start_position().row == target_row {
        return true;
    }
    if let Some(name_node) = node.child_by_field_name("name") {
        if name_node.start_position().row == target_row {
            return true;
        }
    }
    false
}

fn walk_function_name_at_line(node: Node, source: &[u8], target_row: usize) -> Option<String> {
    if is_function_definition_kind(node.kind()) && line_matches_function_node(node, target_row) {
        return function_name_from_definition_node(node, source);
    }

    for index in 0..node.child_count() {
        let child = node.child(index)?;
        if let Some(name) = walk_function_name_at_line(child, source, target_row) {
            return Some(name);
        }
    }
    None
}

/// Return the function name defined on `line` (1-based), if any.
pub fn function_name_at_line(language: &str, source: &str, line: u32) -> Option<String> {
    if line == 0 || source.is_empty() {
        return None;
    }
    let target_row = (line - 1) as usize;

    let tree = match parse_source(language, source) {
        Ok(tree) => tree,
        Err(_) => return None,
    };

    walk_function_name_at_line(tree.root_node(), source.as_bytes(), target_row)
}

/// Find the first definition line (1-based) of `name` in `source`.
pub fn function_definition_line(language: &str, source: &str, name: &str) -> Option<u32> {
    if name.is_empty() || source.is_empty() {
        return None;
    }

    let tree = match parse_source(language, source) {
        Ok(tree) => tree,
        Err(_) => return None,
    };
    find_definition_line_recursive(tree.root_node(), source.as_bytes(), name)
}

fn find_definition_line_recursive(node: Node, source: &[u8], name: &str) -> Option<u32> {
    if is_function_definition_kind(node.kind()) {
        if function_name_from_definition_node(node, source).as_deref() == Some(name) {
            return Some(node.start_position().row as u32 + 1);
        }
    }

    for index in 0..node.child_count() {
        let child = node.child(index)?;
        if let Some(line) = find_definition_line_recursive(child, source, name) {
            return Some(line);
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::highlight::{init, tree_sitter};

    #[test]
    fn python_function_name_at_definition_line() {
        init();
        let source = "def foo(x):\n    return x\n\ndef bar():\n    pass\n";
        assert_eq!(
            function_name_at_line("python", source, 1),
            Some("foo".to_string())
        );
        assert_eq!(
            function_name_at_line("python", source, 4),
            Some("bar".to_string())
        );
        assert_eq!(function_name_at_line("python", source, 2), None);
        assert_eq!(
            function_definition_line("python", source, "bar"),
            Some(4)
        );
    }

    #[test]
    fn c_function_name_uses_builtin_grammar() {
        init();
        assert!(tree_sitter::language_is_loaded("c"));
        let source = "static int add(int a, int b) {\n    return a + b;\n}\n";
        assert_eq!(
            function_name_at_line("c", source, 1),
            Some("add".to_string())
        );
    }
}
