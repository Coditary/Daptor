//! Workspace file discovery for the source file picker.

use std::path::{Path, PathBuf};

const MAX_DEPTH: usize = 8;
const MAX_FILES: usize = 2000;

const SOURCE_EXTENSIONS: &[&str] = &[
    "py", "rs", "go", "js", "ts", "tsx", "jsx", "c", "cc", "cpp", "h", "hpp", "java", "lua", "rb",
    "sh", "bash", "zsh", "swift", "kt", "cs", "php", "ml", "zig",
];

const SKIP_DIRS: &[&str] = &[
    ".git",
    ".svn",
    "node_modules",
    "target",
    "__pycache__",
    ".venv",
    "venv",
    "dist",
    "build",
    ".cargo",
    "reference",
];

/// Collect source files under `root`, sorted by relative path.
pub fn list_source_files(root: &Path) -> Vec<PathBuf> {
    let mut files = Vec::new();
    collect_files(root, root, 0, &mut files);
    files.sort_by(|a, b| {
        relative_display(root, a).to_lowercase().cmp(&relative_display(root, b).to_lowercase())
    });
    files.truncate(MAX_FILES);
    files
}

fn collect_files(root: &Path, dir: &Path, depth: usize, out: &mut Vec<PathBuf>) {
    if depth > MAX_DEPTH || out.len() >= MAX_FILES {
        return;
    }

    let entries = match std::fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) => return,
    };

    for entry in entries.flatten() {
        if out.len() >= MAX_FILES {
            return;
        }

        let path = entry.path();
        let file_type = match entry.file_type() {
            Ok(ft) => ft,
            Err(_) => continue,
        };

        if file_type.is_dir() {
            let name = entry.file_name().to_string_lossy().to_string();
            if SKIP_DIRS.contains(&name.as_str()) {
                continue;
            }
            collect_files(root, &path, depth + 1, out);
        } else if file_type.is_file() && is_source_file(&path) {
            out.push(path);
        }
    }
}

fn is_source_file(path: &Path) -> bool {
    path.extension()
        .and_then(|ext| ext.to_str())
        .map(|ext| SOURCE_EXTENSIONS.contains(&ext.to_lowercase().as_str()))
        .unwrap_or(false)
}

pub fn relative_display(root: &Path, path: &Path) -> String {
    path.strip_prefix(root)
        .unwrap_or(path)
        .display()
        .to_string()
}

/// Simple case-insensitive substring filter.
pub fn filter_files(files: &[PathBuf], root: &Path, query: &str) -> Vec<PathBuf> {
    let query = query.trim().to_lowercase();
    if query.is_empty() {
        return files.to_vec();
    }
    files
        .iter()
        .filter(|path| relative_display(root, path).to_lowercase().contains(&query))
        .cloned()
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn finds_fixture_python_file() {
        let root = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
        let files = list_source_files(&root);
        assert!(files.iter().any(|p| p.ends_with("fixtures/hello.py")));
    }
}
