use tui_debug_core::highlight::{
    HighlightKind, highlight_viewport, init, language_from_path,
};

fn colored_span_count(language: &str, source: &str) -> usize {
    highlight_viewport(language, source, 1, 20)
        .iter()
        .flat_map(|line| line.spans.iter())
        .filter(|span| span.kind != HighlightKind::Default)
        .count()
}

#[test]
fn language_from_path_maps_common_extensions() {
    assert_eq!(language_from_path("foo.py"), "python");
    assert_eq!(language_from_path("foo.rs"), "rust");
    assert_eq!(language_from_path("foo.c"), "c");
    assert_eq!(language_from_path("foo.cpp"), "cpp");
    assert_eq!(language_from_path("foo.h"), "c");
    assert_eq!(language_from_path("foo.hpp"), "cpp");
    assert_eq!(language_from_path("foo.js"), "javascript");
}

#[test]
fn builtin_languages_highlight_representative_syntax() {
    init();

    let samples = [
        ("python", "def main():\n    if True:\n        return 1\n# c\n"),
        ("c", "static int foo() {\n  if (x) return 1;\n  // comment\n}\n"),
        (
            "cpp",
            "#include <iostream>\nstatic void foo() {\n  if (x) throw 1;\n  // comment\n}\n",
        ),
        ("rust", "fn main() {\n    let x = 1;\n    // comment\n}\n"),
        ("hpp", "class Foo {\n  int x;\n};\n"),
        ("h", "static int foo(void) {\n  return 0;\n}\n"),
    ];

    for (language, source) in samples {
        let count = colored_span_count(language, source);
        assert!(
            count >= 4,
            "expected rich highlighting for {language}, got {count} colored spans"
        );
    }
}
