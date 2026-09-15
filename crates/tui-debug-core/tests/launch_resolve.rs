use std::path::PathBuf;

use tui_debug_core::launch::{ResolveRequest, resolve_launch};

fn example_config_path() -> PathBuf {
    let config_dir = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../examples/config");
    config_dir.join("test-config.yaml")
}

#[test]
fn resolves_python_profile_from_extension() {
    let request = ResolveRequest {
        target: PathBuf::from("examples/python/hello.py"),
        args: Vec::new(),
        profile: None,
        adapter: None,
        binary: None,
        workspace: None,
    };

    let resolved = resolve_launch(Some(&example_config_path()), &request).expect("python profile should match");
    assert_eq!(resolved.profile, "python");
    assert_eq!(resolved.adapter_label, "python");
    assert_eq!(resolved.adapter_command, "python3");
}

#[test]
fn resolves_go_profile_from_go_source() {
    let request = ResolveRequest {
        target: PathBuf::from("main.go"),
        args: Vec::new(),
        profile: Some("go".to_string()),
        adapter: None,
        binary: None,
        workspace: None,
    };

    let resolved = resolve_launch(Some(&example_config_path()), &request).expect("go profile should match");
    assert_eq!(resolved.profile, "go");
    assert_eq!(resolved.adapter_command, "dlv");
    assert_eq!(
        resolved.launch.get("mode").and_then(|value| value.as_str()),
        Some("debug")
    );
}

#[test]
fn resolves_rr_profile_when_explicit() {
    let request = ResolveRequest {
        target: PathBuf::from("examples/native/reverse_demo.c"),
        args: Vec::new(),
        profile: Some("rr".to_string()),
        adapter: None,
        binary: None,
        workspace: None,
    };

    let error = resolve_launch(Some(&example_config_path()), &request)
        .expect_err("rr profile should be selected before launch validation");
    assert!(
        error.to_string().contains("rr requires a built executable"),
        "unexpected error: {error}"
    );
}

#[test]
fn explicit_profile_override_wins() {
    let request = ResolveRequest {
        target: PathBuf::from("examples/python/hello.py"),
        args: Vec::new(),
        profile: Some("native".to_string()),
        adapter: None,
        binary: None,
        workspace: None,
    };

    let resolved = resolve_launch(Some(&example_config_path()), &request).expect("explicit profile should resolve");
    assert_eq!(resolved.profile, "native");
    assert!(resolved.ui.lldb_stdio);
}

#[test]
fn missing_definitions_file_fails_with_helpful_error() {
    let request = ResolveRequest {
        target: PathBuf::from("main.go"),
        args: Vec::new(),
        profile: None,
        adapter: None,
        binary: None,
        workspace: None,
    };

    let missing = PathBuf::from("/tmp/daptor-missing-config/config.yaml");
    let error = resolve_launch(Some(&missing), &request).expect_err("missing definitions should fail");
    assert!(
        error.to_string().contains("no launch definitions found"),
        "unexpected error: {error}"
    );
}
