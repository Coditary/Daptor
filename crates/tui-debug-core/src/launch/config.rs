use std::collections::HashMap;
use std::path::{Path, PathBuf};

use anyhow::{Context, Result, bail};
use serde::Deserialize;
use serde_json::Value;

#[derive(Debug, Clone, Deserialize, Default)]
pub struct LaunchConfigFile {
    #[serde(default)]
    pub default_profile: Option<String>,
    #[serde(default)]
    pub adapters: HashMap<String, AdapterDefinition>,
    #[serde(default)]
    pub profiles: HashMap<String, ProfileDefinition>,
}

#[derive(Debug, Clone, Deserialize, Default)]
pub struct AdapterUiSettings {
    #[serde(default)]
    pub lldb_stdio: bool,
    #[serde(default)]
    pub debugpy_launch_defaults: bool,
    #[serde(default)]
    pub line_buffered_console: bool,
    #[serde(default)]
    pub lldb_goto_line_fallback: bool,
    #[serde(default)]
    pub assume_function_breakpoints: bool,
    #[serde(default)]
    pub show_reverse_continue_hint: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct AdapterDefinition {
    pub command: String,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default = "default_transport")]
    pub transport: String,
    #[serde(default)]
    pub initialize: Value,
    #[serde(default)]
    pub launch: Value,
    #[serde(default)]
    pub ui: AdapterUiSettings,
}

fn default_transport() -> String {
    "stdio".to_string()
}

#[derive(Debug, Clone, Copy, Deserialize, Default, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum ProfileBackend {
    #[default]
    Dap,
    Rr,
}

#[derive(Debug, Clone, Deserialize)]
pub struct ProfileDefinition {
    #[serde(default)]
    pub backend: ProfileBackend,
    #[serde(default)]
    pub adapter: Option<String>,
    #[serde(default)]
    pub priority: i32,
    #[serde(default)]
    pub r#match: MatchRules,
    #[serde(default)]
    pub resolve: HashMap<String, String>,
    #[serde(default)]
    pub launch: Value,
    #[serde(default)]
    pub prelaunch: Vec<String>,
    #[serde(default)]
    pub workspace: Option<String>,
    #[serde(default)]
    pub path_map: Vec<PathMapEntry>,
}

#[derive(Debug, Clone, Deserialize, Default)]
pub struct MatchRules {
    #[serde(default)]
    pub extensions: Vec<String>,
    #[serde(default)]
    pub files: Vec<String>,
    #[serde(default)]
    pub executable: bool,
    #[serde(default)]
    pub go_binary: bool,
}

#[derive(Debug, Clone, Deserialize)]
pub struct PathMapEntry {
    pub from: String,
    pub to: String,
}

#[derive(Debug, Clone, Deserialize, Default)]
struct ConfigRoot {
    #[serde(default)]
    definitions: Option<String>,
    #[serde(default)]
    default_profile: Option<String>,
    #[serde(default)]
    adapters: HashMap<String, AdapterDefinition>,
    #[serde(default)]
    profiles: HashMap<String, ProfileDefinition>,
}

fn resolve_config_relative(base: &Path, candidate: &Path) -> PathBuf {
    if candidate.is_absolute() {
        candidate.to_path_buf()
    } else {
        base.join(candidate)
    }
}

fn parse_launch_file(path: &Path) -> Result<LaunchConfigFile> {
    let content = std::fs::read_to_string(path)
        .with_context(|| format!("failed to read launch definitions: {}", path.display()))?;
    serde_yaml::from_str(&content)
        .with_context(|| format!("failed to parse launch definitions: {}", path.display()))
}

fn merge_launch_config(base: LaunchConfigFile, overlay: ConfigRoot) -> LaunchConfigFile {
    let mut adapters = base.adapters;
    for (name, definition) in overlay.adapters {
        adapters.insert(name, definition);
    }

    let mut profiles = base.profiles;
    for (name, definition) in overlay.profiles {
        profiles.insert(name, definition);
    }

    LaunchConfigFile {
        default_profile: overlay.default_profile.or(base.default_profile),
        adapters,
        profiles,
    }
}

pub fn default_config_directory() -> PathBuf {
    if let Ok(dir) = std::env::var("XDG_CONFIG_HOME") {
        if !dir.is_empty() {
            return PathBuf::from(dir).join("tui-debug");
        }
    }
    dirs_home().join(".config").join("tui-debug")
}

fn dirs_home() -> PathBuf {
    if let Ok(home) = std::env::var("HOME") {
        return PathBuf::from(home);
    }
    PathBuf::from(".")
}

pub fn load_launch_config(config_path: Option<&Path>) -> Result<LaunchConfigFile> {
    let config_path = match config_path {
        Some(path) => path.to_path_buf(),
        None => default_config_directory().join("config.yaml"),
    };
    let config_dir = config_path
        .parent()
        .map(Path::to_path_buf)
        .unwrap_or_else(default_config_directory);

    let root: ConfigRoot = if config_path.is_file() {
        let content = std::fs::read_to_string(&config_path)
            .with_context(|| format!("failed to read config: {}", config_path.display()))?;
        serde_yaml::from_str(&content)
            .with_context(|| format!("failed to parse config: {}", config_path.display()))?
    } else {
        ConfigRoot::default()
    };

    let definitions_path = root
        .definitions
        .as_deref()
        .map(Path::new)
        .map(|path| resolve_config_relative(&config_dir, path))
        .unwrap_or_else(|| config_dir.join("definitions.yaml"));

    let mut config = if definitions_path.is_file() {
        parse_launch_file(&definitions_path)?
    } else if !root.adapters.is_empty() || !root.profiles.is_empty() {
        LaunchConfigFile {
            default_profile: root.default_profile.clone(),
            adapters: root.adapters.clone(),
            profiles: root.profiles.clone(),
        }
    } else {
        bail!(
            "no launch definitions found.\n\
             Create {} or add adapters/profiles to {}.\n\
             Example: cp examples/config/definitions.example.yaml {}",
            definitions_path.display(),
            config_path.display(),
            definitions_path.display()
        );
    };

    config = merge_launch_config(config, root);

    if config.adapters.is_empty() {
        bail!("launch definitions contain no adapters");
    }
    if config.profiles.is_empty() {
        bail!("launch definitions contain no profiles");
    }

    Ok(config)
}

pub fn example_definitions_path() -> PathBuf {
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../../examples/config/definitions.example.yaml")
}
