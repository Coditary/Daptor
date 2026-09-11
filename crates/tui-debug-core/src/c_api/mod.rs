mod highlight;
mod session;

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_int, c_void};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::ptr;
use std::sync::{LazyLock, Mutex};

use serde_json::Value;

use session::CSession;
use crate::session::{DataBreakpoint, SourceBreakpoint};

static LAST_ERROR: LazyLock<Mutex<CString>> =
    LazyLock::new(|| Mutex::new(CString::new("").expect("empty string has no NUL")));

pub(crate) fn set_last_error(message: impl Into<String>) {
    let message = message.into();
    let c_string = CString::new(message).unwrap_or_else(|_| {
        CString::new("invalid error message (embedded NUL byte)").expect("static error text")
    });
    if let Ok(mut guard) = LAST_ERROR.lock() {
        *guard = c_string;
    }
}

pub(crate) fn clear_last_error() {
    if let Ok(mut guard) = LAST_ERROR.lock() {
        *guard = CString::new("").expect("empty string has no NUL");
    }
}

pub(crate) fn c_str_to_rust<'a>(ptr: *const c_char, field: &str) -> Result<&'a str, String> {
    if ptr.is_null() {
        return Err(format!("{field} must not be null"));
    }
    unsafe { CStr::from_ptr(ptr) }
        .to_str()
        .map_err(|err| format!("invalid UTF-8 in {field}: {err}"))
}

pub(crate) fn write_json_to_buffer(
    json: &str,
    out: *mut c_char,
    cap: usize,
) -> Result<(), String> {
    if out.is_null() {
        return Err("json_out must not be null".into());
    }
    if cap == 0 {
        return Err("json_out buffer capacity must be greater than zero".into());
    }
    if json.len() + 1 > cap {
        return Err(format!(
            "json_out buffer too small: need {} bytes, have {cap}",
            json.len() + 1
        ));
    }
    unsafe {
        ptr::copy_nonoverlapping(json.as_ptr(), out as *mut u8, json.len());
        *out.add(json.len()) = 0;
    }
    Ok(())
}

fn session_from_ptr(session: *mut c_void) -> Option<&'static mut CSession> {
    if session.is_null() {
        set_last_error("session pointer is null");
        return None;
    }
    Some(unsafe { &mut *(session as *mut CSession) })
}

/// Return the most recent error message, or an empty string when none is set.
///
/// The returned pointer is valid until the next call into the tui-debug C API
/// on any thread.
#[no_mangle]
pub extern "C" fn tui_debug_last_error() -> *const c_char {
    LAST_ERROR
        .lock()
        .map(|guard| guard.as_ptr())
        .unwrap_or(ptr::null())
}

/// Launch a debug session for `program`.
///
/// Returns an opaque session pointer on success, or null on error.
///
/// After a successful launch, call [`tui_debug_sync_snapshot`] once to obtain
/// the stop-on-entry snapshot captured during initialization.
#[no_mangle]
pub extern "C" fn tui_debug_session_launch(program: *const c_char) -> *mut c_void {
    clear_last_error();

    if program.is_null() {
        set_last_error("program must not be null");
        return ptr::null_mut();
    }

    let program_str = match unsafe { CStr::from_ptr(program) }.to_str() {
        Ok(value) => value,
        Err(err) => {
            set_last_error(format!("invalid UTF-8 in program path: {err}"));
            return ptr::null_mut();
        }
    };

    match CSession::launch(program_str) {
        Ok(session) => Box::into_raw(Box::new(session)) as *mut c_void,
        Err(err) => {
            set_last_error(err.to_string());
            ptr::null_mut()
        }
    }
}

/// Launch a native binary debug session via lldb-dap.
#[no_mangle]
pub extern "C" fn tui_debug_session_launch_lldb(program: *const c_char) -> *mut c_void {
    clear_last_error();

    if program.is_null() {
        set_last_error("program must not be null");
        return ptr::null_mut();
    }

    let program_str = match unsafe { CStr::from_ptr(program) }.to_str() {
        Ok(value) => value,
        Err(err) => {
            set_last_error(format!("invalid UTF-8 in program path: {err}"));
            return ptr::null_mut();
        }
    };

    match CSession::launch_lldb(program_str) {
        Ok(session) => Box::into_raw(Box::new(session)) as *mut c_void,
        Err(err) => {
            set_last_error(err.to_string());
            ptr::null_mut()
        }
    }
}

/// Launch a native binary debug session via rr record + replay (reverse debugging).
#[no_mangle]
pub extern "C" fn tui_debug_session_launch_rr(program: *const c_char) -> *mut c_void {
    clear_last_error();

    if program.is_null() {
        set_last_error("program must not be null");
        return ptr::null_mut();
    }

    let program_str = match unsafe { CStr::from_ptr(program) }.to_str() {
        Ok(value) => value,
        Err(err) => {
            set_last_error(format!("invalid UTF-8 in program path: {err}"));
            return ptr::null_mut();
        }
    };

    match CSession::launch_rr(program_str) {
        Ok(session) => Box::into_raw(Box::new(session)) as *mut c_void,
        Err(err) => {
            set_last_error(format!("{err:#}"));
            ptr::null_mut()
        }
    }
}

/// Free a session created by [`tui_debug_session_launch`].
#[no_mangle]
pub unsafe extern "C" fn tui_debug_session_free(session: *mut c_void) {
    if session.is_null() {
        return;
    }
    drop(Box::from_raw(session as *mut CSession));
}

/// Poll for the next session event.
///
/// Writes JSON into `json_out` when an event is available.
///
/// Returns `0` on success, `1` when no event is ready, and `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_poll(
    session: *mut c_void,
    json_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    if json_out.is_null() {
        set_last_error("json_out must not be null");
        return -1;
    }

    if cap == 0 {
        set_last_error("json_out buffer capacity must be greater than zero");
        return -1;
    }

    match session.poll_json() {
        Ok(Some(json)) => {
            if json.len() + 1 > cap {
                set_last_error(format!(
                    "json_out buffer too small: need {} bytes, have {cap}",
                    json.len() + 1
                ));
                return -1;
            }

            unsafe {
                ptr::copy_nonoverlapping(json.as_ptr(), json_out as *mut u8, json.len());
                *json_out.add(json.len()) = 0;
            }
            0
        }
        Ok(None) => 1,
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Execute a debugger command described as JSON, e.g. `{"op":"continue"}`.
#[no_mangle]
pub extern "C" fn tui_debug_command(session: *mut c_void, cmd_json: *const c_char) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    if cmd_json.is_null() {
        set_last_error("cmd_json must not be null");
        return -1;
    }

    let cmd_str = match unsafe { CStr::from_ptr(cmd_json) }.to_str() {
        Ok(value) => value,
        Err(err) => {
            set_last_error(format!("invalid UTF-8 in cmd_json: {err}"));
            return -1;
        }
    };

    match catch_unwind(AssertUnwindSafe(|| session.command(cmd_str))) {
        Ok(Ok(())) => 0,
        Ok(Err(err)) => {
            set_last_error(err.to_string());
            -1
        }
        Err(_) => {
            set_last_error("internal debugger command panic");
            -1
        }
    }
}

/// Refresh debugger state and write a snapshot as JSON into `json_out`.
///
/// Returns the cached stop-on-entry snapshot from launch when still available,
/// otherwise fetches a fresh snapshot from the debug adapter.
///
/// JSON shape: `{"type":"snapshot","snapshot":{...}}`
///
/// Returns `0` on success and `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_sync_snapshot(
    session: *mut c_void,
    json_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    match session.sync_snapshot_json() {
        Ok(json) => match write_json_to_buffer(&json, json_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Drain buffered program console output into `json_out` as a JSON array.
///
/// Each entry has `category` and `text` fields.
///
/// Returns `0` on success and `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_drain_console(
    session: *mut c_void,
    json_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    match session.drain_console_json() {
        Ok(json) => match write_json_to_buffer(&json, json_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Evaluate an expression in the given stack frame.
///
/// `context` is typically `"repl"` or `"watch"`. Result is written to `result_out`.
///
/// Returns `0` on success and `-1` on error.
#[no_mangle]
pub extern "C" fn tui_debug_evaluate(
    session: *mut c_void,
    expression: *const c_char,
    frame_id: i64,
    context: *const c_char,
    result_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    let expression = match c_str_to_rust(expression, "expression") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    let context = match c_str_to_rust(context, "context") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match session.evaluate(expression, frame_id, context) {
        Ok(result) => match write_json_to_buffer(&result, result_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Set breakpoints for `path`. `lines_json` is a JSON array of line numbers (`[1,5]`) or
/// breakpoint objects (`[{"line":24,"condition":"x > 1","hitCondition":">= 5"}]`).
/// On success, writes adapter results to `results_out` as
/// `[{"line":24,"verified":true,"hitCount":3}]` when `results_out` is non-null.
#[no_mangle]
pub extern "C" fn tui_debug_set_breakpoints(
    session: *mut c_void,
    path: *const c_char,
    lines_json: *const c_char,
    results_out: *mut c_char,
    results_cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    let path = match c_str_to_rust(path, "path") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    let lines_json = match c_str_to_rust(lines_json, "lines_json") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    let parsed: Value = match serde_json::from_str(lines_json) {
        Ok(value) => value,
        Err(err) => {
            set_last_error(format!("invalid lines_json: {err}"));
            return -1;
        }
    };

    let breakpoints: Vec<SourceBreakpoint> = match parsed {
        Value::Array(items) => items
            .into_iter()
            .filter_map(|item| {
                if let Some(line) = item.as_u64() {
                    return Some(SourceBreakpoint {
                        line: line as u32,
                        condition: None,
                        hit_condition: None,
                    });
                }
                if !item.is_object() {
                    return None;
                }
                let line = item.get("line").and_then(|value| value.as_u64())? as u32;
                let condition = item
                    .get("condition")
                    .and_then(|value| value.as_str())
                    .map(str::to_string)
                    .filter(|value| !value.is_empty());
                let hit_condition = item
                    .get("hitCondition")
                    .or_else(|| item.get("hit_condition"))
                    .and_then(|value| value.as_str())
                    .map(str::to_string)
                    .filter(|value| !value.is_empty());
                Some(SourceBreakpoint {
                    line,
                    condition,
                    hit_condition,
                })
            })
            .collect(),
        _ => {
            set_last_error("lines_json must be a JSON array of line numbers or breakpoint objects");
            return -1;
        }
    };

    match session.set_breakpoints(path, &breakpoints) {
        Ok(results_json) => {
            if !results_out.is_null() && results_cap > 0 {
                if let Err(err) = write_json_to_buffer(&results_json, results_out, results_cap) {
                    set_last_error(err);
                    return -1;
                }
            }
            0
        }
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Resolve a variable to a DAP `dataId` for `setDataBreakpoints`.
/// Writes `{"dataId":"...","description":"...","accessTypes":["write"]}` to `json_out`.
#[no_mangle]
pub extern "C" fn tui_debug_data_breakpoint_info(
    session: *mut c_void,
    variables_reference: i64,
    frame_id: i64,
    name: *const c_char,
    json_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    let name = if name.is_null() {
        None
    } else {
        match c_str_to_rust(name, "name") {
            Ok(value) => Some(value),
            Err(err) => {
                set_last_error(err);
                return -1;
            }
        }
    };

    match session.data_breakpoint_info(variables_reference, frame_id, name.as_deref()) {
        Ok(json) => match write_json_to_buffer(&json, json_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Replace all data breakpoints. `breakpoints_json` is a JSON array of
/// `{"dataId":"...","description":"x","accessType":"write","condition":"..."}`.
/// On success, writes adapter results to `results_out` when non-null.
#[no_mangle]
pub extern "C" fn tui_debug_set_data_breakpoints(
    session: *mut c_void,
    breakpoints_json: *const c_char,
    results_out: *mut c_char,
    results_cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    let breakpoints_json = match c_str_to_rust(breakpoints_json, "breakpoints_json") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    let parsed: Value = match serde_json::from_str(breakpoints_json) {
        Ok(value) => value,
        Err(err) => {
            set_last_error(format!("invalid breakpoints_json: {err}"));
            return -1;
        }
    };

    let breakpoints: Vec<DataBreakpoint> = match parsed {
        Value::Array(items) => items
            .into_iter()
            .filter_map(|item| {
                if !item.is_object() {
                    return None;
                }
                let data_id = item
                    .get("dataId")
                    .or_else(|| item.get("data_id"))
                    .and_then(|value| value.as_str())
                    .map(str::to_string)
                    .filter(|value| !value.is_empty())?;
                let description = item
                    .get("description")
                    .and_then(|value| value.as_str())
                    .map(str::to_string)
                    .unwrap_or_default();
                let access_type = item
                    .get("accessType")
                    .or_else(|| item.get("access_type"))
                    .and_then(|value| value.as_str())
                    .map(str::to_string)
                    .unwrap_or_else(|| "write".to_string());
                let condition = item
                    .get("condition")
                    .and_then(|value| value.as_str())
                    .map(str::to_string)
                    .filter(|value| !value.is_empty());
                Some(DataBreakpoint {
                    data_id,
                    description,
                    access_type,
                    condition,
                })
            })
            .collect(),
        _ => {
            set_last_error("breakpoints_json must be a JSON array");
            return -1;
        }
    };

    match session.set_data_breakpoints(&breakpoints) {
        Ok(results_json) => {
            if !results_out.is_null() && results_cap > 0 {
                if let Err(err) = write_json_to_buffer(&results_json, results_out, results_cap) {
                    set_last_error(err);
                    return -1;
                }
            }
            0
        }
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Fetch variables for a DAP `variablesReference` into `json_out` as a JSON array.
#[no_mangle]
pub extern "C" fn tui_debug_fetch_variables(
    session: *mut c_void,
    variables_reference: i64,
    scope_name: *const c_char,
    json_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    let scope = if scope_name.is_null() {
        None
    } else {
        match c_str_to_rust(scope_name, "scope_name") {
            Ok(name) => Some(name),
            Err(err) => {
                set_last_error(err);
                return -1;
            }
        }
    };

    match session.fetch_variables_json(variables_reference, scope.as_deref()) {
        Ok(json) => match write_json_to_buffer(&json, json_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Set a variable value in the given scope (`variablesReference`).
#[no_mangle]
pub extern "C" fn tui_debug_set_variable(
    session: *mut c_void,
    variables_reference: i64,
    name: *const c_char,
    value: *const c_char,
    result_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    let name = match c_str_to_rust(name, "name") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    let value = match c_str_to_rust(value, "value") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match session.set_variable(variables_reference, name, value) {
        Ok(json) => match write_json_to_buffer(&json, result_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Fetch source text for a DAP `sourceReference` into `source_out`.
#[no_mangle]
pub extern "C" fn tui_debug_fetch_source(
    session: *mut c_void,
    source_reference: i64,
    source_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    match session.fetch_source(source_reference) {
        Ok(source) => match write_json_to_buffer(&source, source_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Fetch possible step-in targets for a stack frame into `json_out` as a JSON array.
#[no_mangle]
pub extern "C" fn tui_debug_fetch_step_in_targets(
    session: *mut c_void,
    frame_id: i64,
    json_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    match session.fetch_step_in_targets_json(frame_id) {
        Ok(json) => match write_json_to_buffer(&json, json_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}

/// Fetch possible goto targets for a source location into `json_out` as a JSON array.
#[no_mangle]
pub extern "C" fn tui_debug_fetch_goto_targets(
    session: *mut c_void,
    path: *const c_char,
    line: i64,
    column: i64,
    source_reference: i64,
    json_out: *mut c_char,
    cap: usize,
) -> c_int {
    clear_last_error();

    let Some(session) = session_from_ptr(session) else {
        return -1;
    };

    let path = match c_str_to_rust(path, "path") {
        Ok(value) => value,
        Err(err) => {
            set_last_error(err);
            return -1;
        }
    };

    match session.fetch_goto_targets_json(&path, line, column, source_reference) {
        Ok(json) => match write_json_to_buffer(&json, json_out, cap) {
            Ok(()) => 0,
            Err(err) => {
                set_last_error(err);
                -1
            }
        },
        Err(err) => {
            set_last_error(err.to_string());
            -1
        }
    }
}
