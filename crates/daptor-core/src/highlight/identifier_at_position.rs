//! Resolve identifiers and expressions at a source position using tree-sitter.

use tree_sitter::Node;

use super::tree_sitter::{normalize_language, try_load_language};

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct IdentifierAtPosition {
    /// Expression suitable for a watch (e.g. `x` or `obj.field`).
    pub expression: String,
    /// Set when `expression` is a single simple identifier (no member/index access).
    pub simple_name: Option<String>,
}

fn parse_source(language: &str, source: &str) -> Option<tree_sitter::Tree> {
    let language = normalize_language(language);
    if try_load_language(language).is_err() {
        return None;
    }
    super::tree_sitter::parse_snippet(language, source).ok()
}

fn node_text(node: Node, source: &[u8]) -> Option<String> {
    node.utf8_text(source).ok().map(|text| text.to_string()).filter(|text| !text.is_empty())
}

fn is_member_access_kind(kind: &str) -> bool {
    matches!(
        kind,
        "field_expression"
            | "pointer_expression"
            | "subscript_expression"
            | "attribute"
            | "member_expression"
            | "scoped_identifier"
            | "qualified_identifier"
    )
}

fn is_skipped_identifier_kind(kind: &str) -> bool {
    matches!(
        kind,
        "type_identifier"
            | "primitive_type"
            | "storage_class_specifier"
            | "namespace_identifier"
            | "module_name"
    )
}

fn enclosing_member_expression(mut node: Node) -> Option<Node> {
    while let Some(parent) = node.parent() {
        if is_member_access_kind(parent.kind()) {
            return Some(parent);
        }
        node = parent;
    }
    None
}

fn is_function_callee(node: Node) -> bool {
    let Some(parent) = node.parent() else {
        return false;
    };
    if parent.kind() != "call_expression" {
        return false;
    }
    let Some(function) = parent.child_by_field_name("function") else {
        return false;
    };
    node.start_byte() >= function.start_byte() && node.end_byte() <= function.end_byte()
}

fn is_within_parameter_list(node: Node) -> bool {
    let mut current = node;
    while let Some(parent) = current.parent() {
        if parent.kind() == "parameter_list" {
            return true;
        }
        if matches!(
            parent.kind(),
            "function_definition" | "function_item" | "function_declaration" | "translation_unit"
        ) {
            break;
        }
        current = parent;
    }
    false
}

fn is_function_name_in_definition(node: Node) -> bool {
    if is_within_parameter_list(node) {
        return false;
    }

    if let Some(parent) = node.parent() {
        if parent.kind() == "function_declarator" {
            if let Some(name_declarator) = parent.child_by_field_name("declarator") {
                if node.start_byte() >= name_declarator.start_byte()
                    && node.end_byte() <= name_declarator.end_byte()
                {
                    return true;
                }
            }
        }
        if parent.kind() == "function_item" {
            if let Some(name_node) = parent.child_by_field_name("name") {
                return node.start_byte() == name_node.start_byte()
                    && node.end_byte() == name_node.end_byte();
            }
        }
    }

    false
}

fn identifier_from_node(node: Node, source: &[u8]) -> Option<IdentifierAtPosition> {
    if is_skipped_identifier_kind(node.kind()) {
        return None;
    }

    if matches!(
        node.kind(),
        "identifier"
            | "field_identifier"
            | "property_identifier"
            | "shorthand_property_identifier"
            | "shorthand_field_identifier"
    ) {
        if is_function_name_in_definition(node) || is_function_callee(node) {
            return None;
        }

        if let Some(member) = enclosing_member_expression(node) {
            let expression = node_text(member, source)?;
            return Some(IdentifierAtPosition { expression, simple_name: None });
        }

        let name = node_text(node, source)?;
        return Some(IdentifierAtPosition { expression: name.clone(), simple_name: Some(name) });
    }

    if is_member_access_kind(node.kind()) {
        let expression = node_text(node, source)?;
        return Some(IdentifierAtPosition { expression, simple_name: None });
    }

    None
}

fn walk_identifier_at_point(node: Node, source: &[u8]) -> Option<IdentifierAtPosition> {
    if is_skipped_identifier_kind(node.kind()) {
        return None;
    }

    if let Some(found) = identifier_from_node(node, source) {
        return Some(found);
    }

    let mut current = node;
    while let Some(parent) = current.parent() {
        if let Some(found) = identifier_from_node(parent, source) {
            return Some(found);
        }
        current = parent;
    }

    None
}

/// Return the watch expression at `line` (1-based) and `byte_column` (0-based UTF-8 offset in the line).
pub fn identifier_at_position(
    language: &str,
    source: &str,
    line: u32,
    byte_column: u32,
) -> Option<IdentifierAtPosition> {
    if line == 0 || source.is_empty() {
        return None;
    }

    let tree = parse_source(language, source)?;
    let target_row = (line - 1) as usize;
    let target_column = byte_column as usize;
    let root = tree.root_node();
    let node = root.descendant_for_point_range(
        tree_sitter::Point::new(target_row, target_column),
        tree_sitter::Point::new(target_row, target_column),
    )?;

    walk_identifier_at_point(node, source.as_bytes())
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::highlight::{init, tree_sitter};

    fn byte_column(source: &str, line: u32, display_col: usize) -> u32 {
        let mut current_line = 1u32;
        let mut line_start = 0usize;
        for (index, ch) in source.char_indices() {
            if ch == '\n' {
                if current_line == line {
                    break;
                }
                current_line += 1;
                line_start = index + 1;
            }
        }
        let line_text = if current_line == line {
            source[line_start..].split('\n').next().unwrap_or("")
        } else {
            ""
        };
        let mut byte_col = 0usize;
        for (index, ch) in line_text.char_indices() {
            if index >= display_col {
                break;
            }
            byte_col += ch.len_utf8();
        }
        byte_col as u32
    }

    #[test]
    fn c_simple_local_identifier() {
        init();
        let source = "int main() {\n    int x = 1;\n    return x;\n}\n";
        let col = byte_column(source, 3, 11);
        let found = identifier_at_position("c", source, 3, col).expect("identifier");
        assert_eq!(found.expression, "x");
        assert_eq!(found.simple_name.as_deref(), Some("x"));
    }

    #[test]
    fn c_member_access_is_not_simple() {
        init();
        let source = "void f() {\n    obj.field = 1;\n}\n";
        let col = byte_column(source, 2, 8);
        let found = identifier_at_position("c", source, 2, col).expect("identifier");
        assert_eq!(found.expression, "obj.field");
        assert_eq!(found.simple_name, None);
    }

    #[test]
    fn python_attribute_is_not_simple() {
        init();
        let source = "def f():\n    obj.field = 1\n";
        let col = byte_column(source, 2, 8);
        let found = identifier_at_position("python", source, 2, col).expect("identifier");
        assert_eq!(found.expression, "obj.field");
        assert_eq!(found.simple_name, None);
    }

    #[test]
    fn python_simple_name() {
        init();
        let source = "def f():\n    return value\n";
        let col = byte_column(source, 2, 11);
        let found = identifier_at_position("python", source, 2, col).expect("identifier");
        assert_eq!(found.expression, "value");
        assert_eq!(found.simple_name.as_deref(), Some("value"));
    }

    #[test]
    fn skips_type_identifier() {
        init();
        assert!(tree_sitter::language_is_loaded("c"));
        let source = "int x = 0;\n";
        let col = byte_column(source, 1, 0);
        assert!(identifier_at_position("c", source, 1, col).is_none());
    }

    #[test]
    fn parameter_declaration_name_is_simple_local() {
        init();
        let source = "static int add(int a, int b) {\n    return a + b;\n}\n";
        let a_col = byte_column(source, 1, 19);
        let found = identifier_at_position("c", source, 1, a_col).expect("param a");
        assert_eq!(found.expression, "a");
        assert_eq!(found.simple_name.as_deref(), Some("a"));
        let use_col = byte_column(source, 2, 15);
        let used = identifier_at_position("c", source, 2, use_col).expect("b use");
        assert_eq!(used.expression, "b");
        assert_eq!(used.simple_name.as_deref(), Some("b"));
    }

    #[test]
    fn skips_function_callee() {
        init();
        let source = "int main(void) {\n    printf(\"%d\", z);\n}\n";
        let printf_col = byte_column(source, 2, 4);
        assert!(identifier_at_position("c", source, 2, printf_col).is_none());
        let z_col = byte_column(source, 2, 17);
        let found = identifier_at_position("c", source, 2, z_col).expect("z arg");
        assert_eq!(found.expression, "z");
        assert_eq!(found.simple_name.as_deref(), Some("z"));
    }

    #[test]
    fn skips_primitive_type_in_declaration() {
        init();
        let source = "int main(void) {\n    int z = add(x, y);\n}\n";
        for display_col in 4..=6 {
            let col = byte_column(source, 2, display_col);
            assert!(
                identifier_at_position("c", source, 2, col).is_none(),
                "expected no identifier on primitive_type at display col {}",
                display_col
            );
        }
        let z_col = byte_column(source, 2, 8);
        let found = identifier_at_position("c", source, 2, z_col).expect("z");
        assert_eq!(found.expression, "z");
        assert_eq!(found.simple_name.as_deref(), Some("z"));
    }
}
