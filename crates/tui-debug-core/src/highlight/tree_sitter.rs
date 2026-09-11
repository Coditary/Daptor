//! Tree-sitter integration. Language grammars load from `default_tree_sitter_dir()`
//! (or `DAP_TREE_SITTER_DIR`) as shared libraries; highlight queries live beside them.

use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

use anyhow::{Context, Result};
use libloading::{Library, Symbol};
use tree_sitter::{Language, Parser, Tree};
use tree_sitter_highlight::{
    Highlight, HighlightConfiguration, HighlightEvent, Highlighter,
};

use super::language_id::{highlight_language_candidates, normalize_language as normalize_language_id};
use super::spans::{HighlightKind, StyledSpan};

pub use super::language_id::normalize_language;

type LanguageFn = unsafe extern "C" fn() -> *const ();

pub const HIGHLIGHT_NAMES: &[&str] = &[
    "attribute",
    "boolean",
    "carriage-return",
    "comment",
    "constant",
    "constructor",
    "embedded",
    "emphasis",
    "emphasis.strong",
    "error",
    "escape",
    "function",
    "function.builtin",
    "function.macro",
    "function.method",
    "include",
    "keyword",
    "keyword.function",
    "keyword.macro",
    "keyword.operator",
    "keyword.return",
    "keyword.type",
    "module",
    "number",
    "operator",
    "property",
    "property.builtin",
    "punctuation",
    "punctuation.bracket",
    "punctuation.delimiter",
    "punctuation.special",
    "string",
    "string.escape",
    "string.regex",
    "string.special",
    "tag",
    "type",
    "type.builtin",
    "variable",
    "variable.builtin",
    "variable.parameter",
];

/// Default directory for tree-sitter grammar shared libraries and queries.
pub fn default_tree_sitter_dir() -> PathBuf {
    std::env::var("DAP_TREE_SITTER_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|_| {
            std::env::var("XDG_DATA_HOME")
                .map(PathBuf::from)
                .or_else(|_| {
                    std::env::var("HOME").map(|home| PathBuf::from(home).join(".local/share"))
                })
                .unwrap_or_else(|_| PathBuf::from("/tmp"))
                .join("dap/tree-sitter")
        })
}

struct LoadedLanguage {
    _library: Option<Library>,
    language: Language,
    highlights_query: String,
    injections_query: String,
    locals_query: String,
}

struct LanguageRuntime {
    configuration: HighlightConfiguration,
}

struct TreeSitterState {
    languages: HashMap<String, LoadedLanguage>,
    runtimes: HashMap<String, LanguageRuntime>,
}

static STATE: OnceLock<Mutex<TreeSitterState>> = OnceLock::new();

fn state() -> &'static Mutex<TreeSitterState> {
    STATE.get_or_init(|| {
        Mutex::new(TreeSitterState {
            languages: HashMap::new(),
            runtimes: HashMap::new(),
        })
    })
}

/// Register a language from an external tree-sitter grammar directory.
pub(crate) fn register_language_from_dir(language: &str, dir: &Path) -> Result<()> {
    let loaded = load_language_from_dir(language, dir)?;
    let mut guard = state().lock().expect("tree-sitter state lock");
    guard.languages.insert(language.to_string(), loaded);
    guard.runtimes.remove(language);
    Ok(())
}

/// Register a statically linked tree-sitter language (tests / dev tooling).
pub fn register_language_static(
    language: &str,
    grammar: Language,
    highlights_query: &str,
    injections_query: &str,
    locals_query: &str,
) -> Result<()> {
    let mut guard = state().lock().expect("tree-sitter state lock");
    guard.languages.insert(
        language.to_string(),
        LoadedLanguage {
            _library: None,
            language: grammar,
            highlights_query: highlights_query.to_string(),
            injections_query: injections_query.to_string(),
            locals_query: locals_query.to_string(),
        },
    );
    guard.runtimes.remove(language);
    Ok(())
}

fn load_language_from_dir(language: &str, dir: &Path) -> Result<LoadedLanguage> {
    let parser_lib = find_parser_library(dir, language)?;
    let library = unsafe { Library::new(&parser_lib) }
        .with_context(|| format!("load tree-sitter grammar {}", parser_lib.display()))?;
    let symbol_name = format!("tree_sitter_{}", language.replace('-', "_"));
    let language_fn: Symbol<LanguageFn> = unsafe { library.get(symbol_name.as_bytes()) }
        .with_context(|| format!("symbol {symbol_name} in {}", parser_lib.display()))?;
    let raw = unsafe { language_fn() };
    let grammar = unsafe { Language::from_raw(raw as *const _) };

    let queries_dir = dir.join("queries");
    let highlights_query = read_query_file(&queries_dir, "highlights.scm")?;
    let injections_query = read_query_file(&queries_dir, "injections.scm").unwrap_or_default();
    let locals_query = read_query_file(&queries_dir, "locals.scm").unwrap_or_default();

    Ok(LoadedLanguage {
        _library: Some(library),
        language: grammar,
        highlights_query,
        injections_query,
        locals_query,
    })
}

fn find_parser_library(dir: &Path, language: &str) -> Result<PathBuf> {
    let candidates = [
        dir.join(format!("libtree-sitter-{language}.so")),
        dir.join(format!("tree-sitter-{language}.so")),
        dir.join("parser.so"),
        dir.join(format!("libtree-sitter-{language}.dylib")),
        dir.join(format!("tree-sitter-{language}.dylib")),
        dir.join("parser.dylib"),
    ];
    for candidate in candidates {
        if candidate.is_file() {
            return Ok(candidate);
        }
    }
    anyhow::bail!(
        "no tree-sitter parser library found in {} for language {}",
        dir.display(),
        language
    )
}

fn read_query_file(dir: &Path, name: &str) -> Result<String> {
    let path = dir.join(name);
    std::fs::read_to_string(&path)
        .with_context(|| format!("read tree-sitter query {}", path.display()))
}

fn ensure_runtime(language: &str) -> Result<()> {
    let mut guard = state().lock().expect("tree-sitter state lock");
    if guard.runtimes.contains_key(language) {
        return Ok(());
    }
    let loaded = guard
        .languages
        .get(language)
        .context("tree-sitter language not registered")?;
    let mut configuration = HighlightConfiguration::new(
        loaded.language.clone(),
        language,
        &loaded.highlights_query,
        &loaded.injections_query,
        &loaded.locals_query,
    )?;
    configuration.configure(HIGHLIGHT_NAMES);
    guard
        .runtimes
        .insert(language.to_string(), LanguageRuntime { configuration });
    Ok(())
}

pub(crate) fn language_dir(language: &str) -> PathBuf {
    default_tree_sitter_dir().join(language)
}

pub fn language_is_loaded(language: &str) -> bool {
    let language = normalize_language_id(language);
    state()
        .lock()
        .expect("tree-sitter state lock")
        .languages
        .contains_key(language)
}

/// Hint for installing an external tree-sitter grammar.
pub fn language_setup_hint(language: &str, reason: &str) -> String {
    format!(
        "tree-sitter grammar for '{language}' unavailable ({reason}).\n  \
         Install under {root}/{language}/:\n    \
         libtree-sitter-{language}.so (or parser.so)\n    \
         queries/highlights.scm\n  \
         Or set DAP_TREE_SITTER_DIR to another root.",
        root = default_tree_sitter_dir().display(),
    )
}

/// Load a grammar from the default directory; returns a user-facing message on failure.
pub fn try_load_language(language: &str) -> Result<(), String> {
    let language = normalize_language(language);
    if language_is_loaded(language) {
        return Ok(());
    }
    let dir = language_dir(language);
    if !dir.is_dir() {
        return Err(language_setup_hint(language, "directory not found"));
    }
    register_language_from_dir(language, &dir)
        .map_err(|err| language_setup_hint(language, &err.to_string()))?;
    ensure_runtime(language).map_err(|err| language_setup_hint(language, &err.to_string()))?;
    Ok(())
}

fn try_load_from_default_dir(language: &str) -> bool {
    try_load_language(language).is_ok()
}

/// Minimum ratio of non-default bytes before accepting a highlight result.
const MIN_COLORED_BYTE_RATIO: f64 = 0.03;

/// Highlight `source` into styled spans when a grammar is available.
///
/// Tries the requested language first, then any configured fallbacks (e.g. C for C++),
/// and keeps the result with the most semantic coloring.
pub fn highlight_source(language: &str, source: &str) -> Option<Vec<StyledSpan>> {
    if source.is_empty() {
        return Some(Vec::new());
    }

    let mut best: Option<Vec<StyledSpan>> = None;
    let mut best_ratio = 0.0;

    for candidate in highlight_language_candidates(language) {
        if let Some(spans) = highlight_source_for_language(&candidate, source) {
            let ratio = colored_byte_ratio(&spans);
            if ratio >= MIN_COLORED_BYTE_RATIO {
                return Some(spans);
            }
            if ratio > best_ratio {
                best_ratio = ratio;
                best = Some(spans);
            }
        }
    }

    best
}

fn highlight_source_for_language(language: &str, source: &str) -> Option<Vec<StyledSpan>> {
    let language = normalize_language(language);
    {
        let guard = state().lock().expect("tree-sitter state lock");
        if !guard.languages.contains_key(language) {
            drop(guard);
            if !try_load_from_default_dir(language) {
                return None;
            }
        }
    }

    ensure_runtime(language).ok()?;

    let guard = state().lock().expect("tree-sitter state lock");
    let runtime = guard.runtimes.get(language)?;
    let mut highlighter = Highlighter::new();
    let events = highlighter
        .highlight(&runtime.configuration, source.as_bytes(), None, |_| None)
        .ok()?;

    Some(spans_from_events(source, events))
}

fn colored_byte_ratio(spans: &[StyledSpan]) -> f64 {
    let total = spans.iter().map(|span| span.text.len()).sum::<usize>();
    if total == 0 {
        return 0.0;
    }
    let colored = spans
        .iter()
        .filter(|span| span.kind != HighlightKind::Default)
        .map(|span| span.text.len())
        .sum::<usize>();
    colored as f64 / total as f64
}

/// Parse-only helper for diagnostics.
pub fn parse_snippet(language: &str, source: &str) -> Result<Tree> {
    let language = normalize_language(language);
    let guard = state().lock().expect("tree-sitter state lock");
    let loaded = guard
        .languages
        .get(language)
        .context("tree-sitter language not registered")?;
    let mut parser = Parser::new();
    parser.set_language(&loaded.language)?;
    parser
        .parse(source, None)
        .context("parse source snippet")
}

fn spans_from_events(
    source: &str,
    events: impl Iterator<Item = Result<HighlightEvent, tree_sitter_highlight::Error>>,
) -> Vec<StyledSpan> {
    let mut spans = Vec::new();
    let mut active: Option<HighlightKind> = None;

    for event in events {
        match event {
            Ok(HighlightEvent::Source { start, end }) => {
                let text = &source[start..end];
                let kind = active.unwrap_or(HighlightKind::Default);
                push_span(&mut spans, text, kind);
            }
            Ok(HighlightEvent::HighlightStart(highlight)) => {
                active = Some(highlight_to_kind(highlight));
            }
            Ok(HighlightEvent::HighlightEnd) => active = None,
            Err(_) => {}
        }
    }

    spans
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

fn highlight_to_kind(highlight: Highlight) -> HighlightKind {
    let name = HIGHLIGHT_NAMES
        .get(highlight.0 as usize)
        .copied()
        .unwrap_or("variable");
    HighlightKind::from_highlight_name(name)
}
