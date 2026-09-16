use std::path::PathBuf;

use daptor_core::launch::{resolve_launch, ResolveRequest};

fn fixture_config_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/config/test-config.yaml")
}

fn fixture_path(relative: &str) -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join(relative)
}

#[test]
fn resolves_python_profile_from_extension() {
    let request = ResolveRequest {
        target: fixture_path("tests/fixtures/python/hello.py"),
        args: Vec::new(),
        profile: None,
        adapter: None,
        binary: None,
        workspace: None,
    };

    let resolved = resolve_launch(Some(&fixture_config_path()), &request)
        .expect("python profile should match");
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

    let resolved =
        resolve_launch(Some(&fixture_config_path()), &request).expect("go profile should match");
    assert_eq!(resolved.profile, "go");
    assert_eq!(resolved.adapter_command, "dlv");
    assert_eq!(resolved.launch.get("mode").and_then(|value| value.as_str()), Some("debug"));
}

#[test]
fn resolves_rr_profile_when_explicit() {
    let request = ResolveRequest {
        target: fixture_path("tests/fixtures/native/unbuilt_target.c"),
        args: Vec::new(),
        profile: Some("rr".to_string()),
        adapter: None,
        binary: None,
        workspace: None,
    };

    let error = resolve_launch(Some(&fixture_config_path()), &request)
        .expect_err("rr profile should be selected before launch validation");
    assert!(
        error.to_string().contains("rr requires a built executable"),
        "unexpected error: {error}"
    );
}

#[test]
fn resolves_cpp_profile_with_prelaunch_from_definitions() {
    let request = ResolveRequest {
        target: fixture_path("tests/fixtures/native/cpp_step_demo.cpp"),
        args: Vec::new(),
        profile: Some("cpp".to_string()),
        adapter: None,
        binary: None,
        workspace: None,
    };

    let resolved =
        resolve_launch(Some(&fixture_config_path()), &request).expect("cpp profile should resolve");
    assert_eq!(resolved.profile, "cpp");
    assert_eq!(resolved.adapter_label, "native");
    assert!(resolved.ui.lldb_stdio);
    assert!(
        resolved.program.to_string_lossy().ends_with("cpp_step_demo"),
        "program should resolve to built binary path, got {}",
        resolved.program.display()
    );
    assert_eq!(resolved.launch.get("type").and_then(|value| value.as_str()), Some("lldb"));
}

#[test]
fn explicit_profile_override_wins() {
    let request = ResolveRequest {
        target: fixture_path("tests/fixtures/python/hello.py"),
        args: Vec::new(),
        profile: Some("native".to_string()),
        adapter: None,
        binary: None,
        workspace: None,
    };

    let resolved = resolve_launch(Some(&fixture_config_path()), &request)
        .expect("explicit profile should resolve");
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
    let error =
        resolve_launch(Some(&missing), &request).expect_err("missing definitions should fail");
    assert!(error.to_string().contains("no launch definitions found"), "unexpected error: {error}");
}
