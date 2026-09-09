use serde::{Deserialize, Serialize};

/// Semantic token category for source rendering.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "snake_case")]
pub enum HighlightKind {
    Default,
    Keyword,
    String,
    Comment,
    Function,
    Type,
    Number,
    Operator,
    Variable,
}

/// A contiguous slice of source text with one highlight category.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct StyledSpan {
    pub text: String,
    pub kind: HighlightKind,
}

/// One source line split into styled spans for the UI.
#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct HighlightedLine {
    pub line_number: i64,
    pub spans: Vec<StyledSpan>,
}

impl HighlightKind {
    pub fn from_highlight_name(name: &str) -> Self {
        match name {
            n if n.starts_with("keyword") => Self::Keyword,
            n if n.starts_with("string") => Self::String,
            "comment" => Self::Comment,
            n if n.starts_with("function") => Self::Function,
            "type" | "type.builtin" => Self::Type,
            "number" | "boolean" | "constant" => Self::Number,
            "operator" | "punctuation" | "punctuation.bracket" | "punctuation.delimiter"
            | "punctuation.special" => Self::Operator,
            "variable" | "variable.builtin" | "variable.parameter" | "property"
            | "property.builtin" => Self::Variable,
            _ => Self::Default,
        }
    }
}
