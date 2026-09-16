use std::path::PathBuf;
use std::process::Command;
use std::sync::{Mutex, MutexGuard, Once};

static INTEGRATION_LOCK: Mutex<()> = Mutex::new(());

/// Hold while spawning debug adapters or rr — they conflict when run in parallel.
pub fn integration_test_lock() -> MutexGuard<'static, ()> {
    INTEGRATION_LOCK.lock().expect("integration test lock poisoned")
}

pub fn native_fixtures_dir() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("tests/fixtures/native")
}

/// Build and return a native test fixture binary from `tests/fixtures/native/`.
pub fn ensure_native_binary(name: &str) -> PathBuf {
    let path = native_fixtures_dir().join(name);
    if path.exists() {
        return path.canonicalize().unwrap_or(path);
    }

    static BUILD_ONCE: Once = Once::new();
    BUILD_ONCE.call_once(|| {
        let script = native_fixtures_dir().join("build.sh");
        let status = Command::new("bash")
            .arg(&script)
            .status()
            .expect("failed to run tests/fixtures/native/build.sh");
        assert!(status.success(), "tests/fixtures/native/build.sh failed");
    });

    path.canonicalize()
        .unwrap_or_else(|_| panic!("native fixture binary {name} missing after build.sh"))
}

pub fn lldb_dap_available() -> bool {
    Command::new("lldb-dap")
        .arg("--help")
        .output()
        .map(|output| output.status.success())
        .unwrap_or(false)
}
