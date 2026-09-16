use std::path::{Path, PathBuf};

/// True when `path` points at an ELF executable (magic bytes or execute bit).
pub fn is_elf_executable(path: &Path) -> bool {
    if path.as_os_str().is_empty() {
        return false;
    }
    if let Ok(metadata) = std::fs::metadata(path) {
        #[cfg(unix)]
        {
            use std::os::unix::fs::PermissionsExt;
            if metadata.is_file() && metadata.permissions().mode() & 0o111 != 0 {
                return true;
            }
        }
    }

    let mut file = match std::fs::File::open(path) {
        Ok(file) => file,
        Err(_) => return false,
    };
    let mut magic = [0u8; 4];
    use std::io::Read;
    if file.read_exact(&mut magic).is_err() {
        return false;
    }
    magic == [0x7f, b'E', b'L', b'F']
}

/// Heuristic: ELF file containing Go build info.
pub fn is_go_binary(path: &Path) -> bool {
    if !is_elf_executable(path) {
        return false;
    }
    let bytes = match std::fs::read(path) {
        Ok(bytes) => bytes,
        Err(_) => return false,
    };
    bytes.windows(12).any(|window| window == b"Go buildinf:")
}

/// Walk parents from `start` looking for `marker` (file name).
pub fn find_nearest_file(start: &Path, marker: &str) -> Option<PathBuf> {
    let start = if start.is_file() { start.parent().unwrap_or(start) } else { start };

    for directory in start.ancestors() {
        let candidate = directory.join(marker);
        if candidate.is_file() {
            return Some(candidate);
        }
    }
    None
}

/// Workspace root: directory containing `go.mod`, else parent of a file target, else cwd.
pub fn infer_workspace(target: &Path) -> PathBuf {
    if let Some(go_mod) = find_nearest_file(target, "go.mod") {
        return go_mod.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    }

    if target.is_file() {
        return target.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    }

    if target.is_dir() {
        return target.to_path_buf();
    }

    std::env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

const SOURCE_EXTENSIONS: &[&str] = &[".c", ".cpp", ".cc", ".cxx", ".rs", ".go"];

/// If `path` is a source file, return the usual output binary path (stem without extension).
pub fn sibling_executable_for_source(path: &Path) -> Option<PathBuf> {
    let path_str = path.to_string_lossy();
    for extension in SOURCE_EXTENSIONS {
        if path_str.ends_with(extension) {
            return Some(PathBuf::from(path_str.trim_end_matches(extension)));
        }
    }
    None
}

/// Resolve a native launch path: ELF passthrough, else binary stem for source files.
pub fn resolve_native_launch_path(path: &Path) -> PathBuf {
    if is_elf_executable(path) {
        return path.to_path_buf();
    }
    sibling_executable_for_source(path).unwrap_or_else(|| path.to_path_buf())
}

/// Go package directory for a `.go` file or directory target.
pub fn go_package_dir(target: &Path) -> PathBuf {
    if target.extension().is_some_and(|ext| ext == "go") {
        return target.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."));
    }
    target.to_path_buf()
}

/// Default binary path for a Go package under the workspace.
pub fn default_go_binary(workspace: &Path, package: &Path) -> PathBuf {
    let name = package
        .file_name()
        .map(|value| value.to_string_lossy().to_string())
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| "main".to_string());
    workspace.join("bin").join(name)
}
