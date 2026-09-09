use std::os::raw::c_char;

use serde::Serialize;

use crate::highlight::{HighlightKind, HighlightedLine, highlight_viewport};

use super::{clear_last_error, c_str_to_rust, set_last_error, write_json_to_buffer};

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
                .map(|span| SpanJson {
                    text: span.text.as_str(),
                    kind: span.kind,
                })
                .collect(),
        })
        .collect();

    serde_json::to_string(&payload).map_err(|err| format!("serialize highlight JSON: {err}"))
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

#[cfg(test)]
mod tests {
    use std::ffi::CString;
    use std::os::raw::c_char;

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
    fn c_api_writes_highlight_json() {
        register_python_from_dev_dep();

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

        let json = unsafe { std::ffi::CStr::from_ptr(out.as_ptr()) }
            .to_str()
            .unwrap();
        assert!(json.contains("\"kind\":\"keyword\""));
        assert!(json.contains("\"text\":\"if\""));
    }
}
