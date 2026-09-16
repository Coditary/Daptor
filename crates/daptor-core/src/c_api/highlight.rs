use std::os::raw::{c_char, c_int};
use std::path::PathBuf;

use serde::Serialize;

use crate::highlight::{
    function_definition_line, function_name_at_line, highlight_viewport, identifier_at_position,
    init, language_from_path, language_is_loaded, set_tree_sitter_dir, try_load_language,
    HighlightKind, HighlightedLine,
};

use super::{c_str_to_rust, clear_last_error, set_last_error, write_json_to_buffer};

#[derive(Serialize)]
struct HighlightedLineJson<'a> {
    line: i64,
    spans: Vec<SpanJson<'a>>,
}

#[derive(Serialize)]
struct SpanJson<'a> {
    text: &'a str,
    kind: HighlightKind,
}

fn lines_to_json(lines: &[HighlightedLine]) -> Result<String, String> {
    let payload: Vec<HighlightedLineJson<'_>> = lines
        .iter()
        .map(|line| HighlightedLineJson {
            line: line.line_number,
            spans: line
                .spans
                .iter()
                .map(|span| SpanJson { text: span.text.as_str(), kind: span.kind })
                .collect(),
        })
        .collect();

    serde_json::to_string(&payload).map_err(|err| format!("serialize highlight JSON: {err}"))
}

/// Override the tree-sitter grammar directory for this process.
///
/// Must be called before [`tui_debug_init`]. Returns `0` on success, `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_set_tree_sitter_dir(path: *const c_char) -> c_int {
    match c_str_to_rust(path, "tree_sitter_dir") {
        Ok(dir) => {
            set_tree_sitter_dir(PathBuf::from(dir));
            clear_last_error();
            0
        }
        Err(err) => {
            set_last_error(err);
            -1
        }
    }
}

/// Initialize tree-sitter grammars (built-in Python + optional external `.so` files).
///
/// Safe to call multiple times; initialization runs once per process.
#[no_mangle]
pub extern "C" fn tui_debug_init() {
    init();
}

/// Write the tree-sitter language id for `path` into `language_out`.
///
/// Returns `0` on success, `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_language_from_path(
    path: *const c_char,
    language_out: *mut c_char,
    cap: usize,
) -> i32 {
    clear_last_error();

    let path = match c_str_to_rust(path, "path") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match write_json_to_buffer(language_from_path(path), language_out, cap) {
        Ok(()) => 0,
        Err(err) => {
            set_last_error(err);
            -1
        }
    }
}

/// Returns `1` when a grammar for `language` is loaded, otherwise `0`.
#[no_mangle]
pub extern "C" fn tui_debug_language_is_loaded(language: *const c_char) -> i32 {
    clear_last_error();

    let language = match c_str_to_rust(language, "language") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return 0;
        }
    };

    if language_is_loaded(language) {
        1
    } else {
        0
    }
}

/// Try to load a grammar from `DAP_TREE_SITTER_DIR/<language>/`.
///
/// Returns `0` on success, `-1` on error (see [`tui_debug_last_error`]).
#[no_mangle]
pub extern "C" fn tui_debug_try_load_language(language: *const c_char) -> i32 {
    clear_last_error();

    let language = match c_str_to_rust(language, "language") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match try_load_language(language) {
        Ok(()) => 0,
        Err(err) => {
            set_last_error(err);
            -1
        }
    }
}

/// Highlight a source viewport and write JSON to `json_out`.
///
/// Returns `0` on success, `-1` on error. JSON schema:
/// `[{"line":1,"spans":[{"text":"def","kind":"keyword"}]}]`
#[no_mangle]
pub extern "C" fn tui_debug_highlight_viewport(
    language: *const c_char,
    source: *const c_char,
    first_line: i64,
    line_count: i64,
    json_out: *mut c_char,
    cap: usize,
) -> i32 {
    init();
    clear_last_error();

    let language = match c_str_to_rust(language, "language") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };
    let source = match c_str_to_rust(source, "source") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    let lines = highlight_viewport(language, source, first_line, line_count);
    let json = match lines_to_json(&lines) {
        Ok(json) => json,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    if let Err(err) = write_json_to_buffer(&json, json_out, cap) {
        set_last_error(err);
        return -1;
    }

    0
}

/// Resolve a function name on `line` (1-based) using tree-sitter when the grammar is loaded.
///
/// Returns `0` when a name was written to `name_out`, `1` when no function was found,
/// and `-1` on error (see [`tui_debug_last_error`]).
#[no_mangle]
pub extern "C" fn tui_debug_function_name_at_line(
    language: *const c_char,
    source: *const c_char,
    line: u32,
    name_out: *mut c_char,
    name_cap: usize,
) -> i32 {
    init();
    clear_last_error();

    let language = match c_str_to_rust(language, "language") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };
    let source = match c_str_to_rust(source, "source") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match function_name_at_line(language, source, line) {
        Some(name) => {
            if let Err(err) = write_json_to_buffer(&name, name_out, name_cap) {
                set_last_error(err);
                return -1;
            }
            0
        }
        None => 1,
    }
}

/// Find the first definition line (1-based) of `name` using tree-sitter.
///
/// Returns `0` when `line_out` was set, `1` when not found, and `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_function_definition_line(
    language: *const c_char,
    source: *const c_char,
    name: *const c_char,
    line_out: *mut u32,
) -> i32 {
    init();
    clear_last_error();

    if line_out.is_null() {
        set_last_error("line_out must not be null");
        return -1;
    }

    let language = match c_str_to_rust(language, "language") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };
    let source = match c_str_to_rust(source, "source") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };
    let name = match c_str_to_rust(name, "name") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match function_definition_line(language, source, name) {
        Some(line) => {
            unsafe {
                *line_out = line;
            }
            0
        }
        None => 1,
    }
}

/// Resolve a watch expression at `line` (1-based) and `byte_column` (UTF-8 byte offset in the line).
///
/// Returns `0` when `expression_out` was written. When the position refers to a simple identifier,
/// `simple_out` receives the same name (for data breakpoints on locals). Returns `1` when nothing
/// was found and `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_identifier_at_position(
    language: *const c_char,
    source: *const c_char,
    line: u32,
    byte_column: u32,
    expression_out: *mut c_char,
    expression_cap: usize,
    simple_out: *mut c_char,
    simple_cap: usize,
) -> i32 {
    init();
    clear_last_error();

    let language = match c_str_to_rust(language, "language") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };
    let source = match c_str_to_rust(source, "source") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match identifier_at_position(language, source, line, byte_column) {
        Some(found) => {
            if let Err(err) =
                write_json_to_buffer(&found.expression, expression_out, expression_cap)
            {
                set_last_error(err);
                return -1;
            }
            if !simple_out.is_null() && simple_cap > 0 {
                let simple = found.simple_name.as_deref().unwrap_or("");
                if let Err(err) = write_json_to_buffer(simple, simple_out, simple_cap) {
                    set_last_error(err);
                    return -1;
                }
            }
            0
        }
        None => 1,
    }
}

#[cfg(test)]
mod tests {
    use std::ffi::CString;
    use std::os::raw::c_char;

    use crate::highlight::init;

    #[test]
    fn c_api_highlights_large_python_stdlib_file() {
        init();

        let path = std::path::Path::new("/usr/lib64/python3.14/json/encoder.py");
        if !path.is_file() {
            return;
        }

        let source = std::fs::read_to_string(path).expect("read encoder.py");
        let line_count = source.lines().count() as i64;
        let language = CString::new("python").unwrap();
        let source = CString::new(source).expect("encoder source");
        let mut out = vec![0_i8; 256 * 1024];

        let status = super::tui_debug_highlight_viewport(
            language.as_ptr(),
            source.as_ptr(),
            1,
            line_count,
            out.as_mut_ptr() as *mut c_char,
            out.len(),
        );
        assert_eq!(status, 0);

        let json = unsafe { std::ffi::CStr::from_ptr(out.as_ptr()) }.to_str().unwrap();
        assert!(json.contains("\"kind\":\"keyword\""));
        assert!(json.len() > 65_536, "expected highlighted JSON to exceed legacy 64KiB buffer");
    }

    #[test]
    fn c_api_writes_highlight_json() {
        init();

        let language = CString::new("python").unwrap();
        let source = CString::new("if True:\n    return 1\n").unwrap();
        let mut out = vec![0_i8; 4096];

        let status = super::tui_debug_highlight_viewport(
            language.as_ptr(),
            source.as_ptr(),
            1,
            2,
            out.as_mut_ptr() as *mut c_char,
            out.len(),
        );
        assert_eq!(status, 0);

        let json = unsafe { std::ffi::CStr::from_ptr(out.as_ptr()) }.to_str().unwrap();
        assert!(json.contains("\"kind\":\"keyword\""));
        assert!(json.contains("\"text\":\"if\""));
    }
}
