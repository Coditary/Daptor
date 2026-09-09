use super::spans::{HighlightKind, HighlightedLine, StyledSpan};
use super::tree_sitter;

/// Highlight a viewport of source lines using tree-sitter when available.
///
/// `first_line` is 1-based. Returns up to `line_count` lines. When no grammar is
/// loaded, each line is returned as a single `Default` span.
pub fn highlight_viewport(
    language: &str,
    source: &str,
    first_line: i64,
    line_count: i64,
) -> Vec<HighlightedLine> {
    if line_count <= 0 || first_line <= 0 {
        return Vec::new();
    }

    let lines: Vec<&str> = source.lines().collect();
    if lines.is_empty() {
        return Vec::new();
    }

    let start_idx = (first_line - 1).clamp(0, lines.len() as i64 - 1) as usize;
    let end_idx = (start_idx + line_count as usize).min(lines.len());
    if start_idx >= end_idx {
        return Vec::new();
    }

    let viewport_lines = &lines[start_idx..end_idx];
    let viewport_text = viewport_lines.join("\n");

    if let Some(spans) = tree_sitter::highlight_source(language, &viewport_text) {
        return spans_to_lines(spans, first_line, viewport_lines.len());
    }

    viewport_lines
        .iter()
        .enumerate()
        .map(|(offset, line)| HighlightedLine {
            line_number: first_line + offset as i64,
            spans: vec![StyledSpan {
                text: (*line).to_string(),
                kind: HighlightKind::Default,
            }],
        })
        .collect()
}

fn spans_to_lines(
    spans: Vec<StyledSpan>,
    first_line: i64,
    expected_line_count: usize,
) -> Vec<HighlightedLine> {
    let mut out = Vec::new();
    let mut current_line = first_line;
    let mut current_spans = Vec::new();

    for span in spans {
        let parts: Vec<&str> = span.text.split('\n').collect();
        for (part_index, part) in parts.iter().enumerate() {
            if part_index > 0 {
                out.push(HighlightedLine {
                    line_number: current_line,
                    spans: std::mem::take(&mut current_spans),
                });
                current_line += 1;
                current_spans = Vec::new();
            }
            if !part.is_empty() {
                push_span(&mut current_spans, part, span.kind);
            }
        }
    }

    out.push(HighlightedLine {
        line_number: current_line,
        spans: current_spans,
    });

    while out.len() < expected_line_count {
        out.push(HighlightedLine {
            line_number: first_line + out.len() as i64,
            spans: Vec::new(),
        });
    }

    if out.len() > expected_line_count {
        out.truncate(expected_line_count);
    }

    out
}

fn push_span(spans: &mut Vec<StyledSpan>, text: &str, kind: HighlightKind) {
    if text.is_empty() {
        return;
    }
    if let Some(last) = spans.last_mut() {
        if last.kind == kind {
            last.text.push_str(text);
            return;
        }
    }
    spans.push(StyledSpan {
        text: text.to_string(),
        kind,
    });
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::highlight::register_language_static;

    fn register_python_from_dev_dep() {
        let _ = register_language_static(
            "python",
            tree_sitter_python::LANGUAGE.into(),
            tree_sitter_python::HIGHLIGHTS_QUERY,
            "",
            "",
        );
    }

    #[test]
    fn highlights_python_viewport_keywords() {
        register_python_from_dev_dep();
        let source = "def main():\n    if True:\n        return 1\n";
        let lines = highlight_viewport("python", source, 1, 3);

        assert_eq!(lines.len(), 3);
        assert_eq!(lines[0].line_number, 1);
        assert!(lines[0].spans.iter().any(|span| span.text.contains("def")));
        assert!(lines[0]
            .spans
            .iter()
            .any(|span| span.kind == HighlightKind::Keyword));
        assert!(lines[2]
            .spans
            .iter()
            .any(|span| span.text.contains("return")));
    }

    #[test]
    fn falls_back_to_plain_spans_without_grammar() {
        let source = "fn main() {\n}\n";
        let lines = highlight_viewport("rust", source, 1, 2);

        assert_eq!(lines.len(), 2);
        assert_eq!(lines[0].spans.len(), 1);
        assert_eq!(lines[0].spans[0].kind, HighlightKind::Default);
        assert_eq!(lines[0].spans[0].text, "fn main() {");
    }
}
