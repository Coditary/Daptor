use std::collections::HashMap;
use std::path::{Path, PathBuf};

use anyhow::{bail, Context, Result};
use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

use super::config::{
    load_launch_config, AdapterUiSettings, LaunchConfigFile, PathMapEntry, ProfileBackend,
    ProfileDefinition,
};
use super::prelaunch::run_prelaunch_commands;
use super::probe::{
    default_go_binary, find_nearest_file, go_package_dir, infer_workspace, is_elf_executable,
    is_go_binary, resolve_native_launch_path,
};

#[derive(Debug, Clone, Deserialize, Default)]
pub struct ResolveRequest {
    pub target: PathBuf,
    #[serde(default)]
    pub args: Vec<String>,
    #[serde(default)]
    pub profile: Option<String>,
    #[serde(default)]
    pub adapter: Option<String>,
    #[serde(default)]
    pub binary: Option<PathBuf>,
    #[serde(default)]
    pub workspace: Option<PathBuf>,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq, Serialize, Deserialize)]
#[serde(rename_all = "lowercase")]
pub enum LaunchBackend {
    Dap,
    Rr,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize, Default)]
pub struct ResolvedUiSettings {
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

impl From<AdapterUiSettings> for ResolvedUiSettings {
    fn from(settings: AdapterUiSettings) -> Self {
        Self {
            lldb_stdio: settings.lldb_stdio,
            debugpy_launch_defaults: settings.debugpy_launch_defaults,
            line_buffered_console: settings.line_buffered_console,
            lldb_goto_line_fallback: settings.lldb_goto_line_fallback,
            assume_function_breakpoints: settings.assume_function_breakpoints,
            show_reverse_continue_hint: settings.show_reverse_continue_hint,
        }
    }
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize, Deserialize)]
pub struct ResolvedLaunch {
    pub backend: LaunchBackend,
    pub adapter_label: String,
    pub profile: String,
    pub program: PathBuf,
    pub target: PathBuf,
    pub workspace: PathBuf,
    #[serde(default)]
    pub display_source: Option<PathBuf>,
    #[serde(default)]
    pub program_args: Vec<String>,
    pub adapter_command: String,
    #[serde(default)]
    pub adapter_args: Vec<String>,
    #[serde(default)]
    pub initialize: Value,
    #[serde(default)]
    pub launch: Value,
    #[serde(default, flatten)]
    pub ui: ResolvedUiSettings,
}

#[derive(Debug, Clone)]
pub(crate) struct LaunchResolveContext {
    target: PathBuf,
    pub(crate) workspace: PathBuf,
    package: Option<PathBuf>,
    binary: Option<PathBuf>,
    program: Option<PathBuf>,
    values: HashMap<String, String>,
}

pub fn resolve_launch(
    config_path: Option<&Path>,
    request: &ResolveRequest,
) -> Result<ResolvedLaunch> {
    let config = load_launch_config(config_path)?;
    let target = match request.target.canonicalize() {
        Ok(path) => path,
        Err(_) => request.target.clone(),
    };

    let workspace = request
        .workspace
        .clone()
        .or_else(|| Some(infer_workspace(&target)))
        .map(|path| path.canonicalize().unwrap_or(path))
        .unwrap_or_else(|| PathBuf::from("."));

    let profile_name = pick_profile_name(&config, &target, request)?;
    let profile = config
        .profiles
        .get(&profile_name)
        .cloned()
        .with_context(|| format!("profile '{profile_name}' is not defined"))?;

    let mut ctx = build_context(&target, &workspace, request.binary.clone());
    apply_profile_resolve(&mut ctx, &profile.resolve);

    let program = ctx
        .program
        .clone()
        .or_else(|| ctx.binary.clone())
        .or_else(|| Some(target.clone()))
        .with_context(|| format!("profile '{profile_name}' did not resolve a program path"))?;

    if profile.backend == ProfileBackend::Rr {
        return build_rr_launch(
            &profile_name,
            "rr",
            &target,
            &workspace,
            program,
            request.args.clone(),
        );
    }

    let adapter_name = request
        .adapter
        .clone()
        .or(profile.adapter.clone())
        .with_context(|| format!("profile '{profile_name}' requires an adapter"))?;
    let adapter = config
        .adapters
        .get(&adapter_name)
        .cloned()
        .with_context(|| format!("adapter '{adapter_name}' is not defined"))?;

    run_prelaunch_commands(&profile.prelaunch, &ctx)?;

    let launch = merge_launch_payload(
        &adapter.launch,
        &profile.launch,
        &profile.path_map,
        &ctx,
        &program,
        &request.args,
    );
    let initialize = merge_initialize_payload(&adapter.initialize);
    let ui = ResolvedUiSettings::from(adapter.ui);

    let display_source =
        if target.is_file() && !is_elf_executable(&target) { Some(target.clone()) } else { None };

    Ok(ResolvedLaunch {
        backend: LaunchBackend::Dap,
        adapter_label: adapter_name,
        profile: profile_name,
        program,
        target,
        workspace,
        display_source,
        program_args: request.args.clone(),
        adapter_command: adapter.command,
        adapter_args: adapter.args,
        initialize,
        launch,
        ui,
    })
}

fn build_rr_launch(
    profile_name: &str,
    adapter_name: &str,
    target: &Path,
    workspace: &Path,
    program: PathBuf,
    program_args: Vec<String>,
) -> Result<ResolvedLaunch> {
    if !is_elf_executable(&program) {
        bail!(
            "rr requires a built executable, not a source file.\n\
             Build first, then pass the binary path."
        );
    }

    Ok(ResolvedLaunch {
        backend: LaunchBackend::Rr,
        adapter_label: adapter_name.to_string(),
        profile: profile_name.to_string(),
        program,
        target: target.to_path_buf(),
        workspace: workspace.to_path_buf(),
        display_source: None,
        program_args,
        adapter_command: String::new(),
        adapter_args: Vec::new(),
        initialize: Value::Null,
        launch: Value::Null,
        ui: ResolvedUiSettings::default(),
    })
}

fn pick_profile_name(
    config: &LaunchConfigFile,
    target: &Path,
    request: &ResolveRequest,
) -> Result<String> {
    if let Some(profile) = &request.profile {
        return Ok(profile.clone());
    }

    let mut matches: Vec<(i32, String)> = Vec::new();
    for (name, profile) in &config.profiles {
        if profile.backend == ProfileBackend::Rr {
            continue;
        }
        if profile_matches(target, profile) {
            matches.push((profile.priority, name.clone()));
        }
    }

    matches.sort_by(|left, right| right.0.cmp(&left.0));
    if let Some((_, name)) = matches.first() {
        return Ok(name.clone());
    }

    if let Some(default_profile) = &config.default_profile {
        return Ok(default_profile.clone());
    }

    bail!(
        "no launch profile matched '{}'. Use --profile or add a profile to config.yaml",
        target.display()
    );
}

fn profile_matches(target: &Path, profile: &ProfileDefinition) -> bool {
    let rules = &profile.r#match;
    let has_rules = !rules.extensions.is_empty()
        || !rules.files.is_empty()
        || rules.executable
        || rules.go_binary;

    if !has_rules {
        return false;
    }

    if !rules.extensions.is_empty() {
        let extension =
            target.extension().map(|ext| format!(".{}", ext.to_string_lossy())).unwrap_or_default();
        if rules.extensions.iter().any(|candidate| candidate == &extension) {
            return true;
        }
    }

    if !rules.files.is_empty() {
        for marker in &rules.files {
            if target.file_name().is_some_and(|name| name == marker.as_str()) {
                return true;
            }
            if find_nearest_file(target, marker).is_some() {
                return true;
            }
        }
    }

    if rules.go_binary && is_go_binary(target) {
        return true;
    }

    if rules.executable && is_elf_executable(target) && !rules.go_binary {
        return !is_go_binary(target);
    }

    false
}

fn build_context(
    target: &Path,
    workspace: &Path,
    binary_override: Option<PathBuf>,
) -> LaunchResolveContext {
    let package = if target.extension().is_some_and(|ext| ext == "go")
        || find_nearest_file(target, "go.mod").is_some()
    {
        Some(go_package_dir(target))
    } else {
        None
    };

    let binary = binary_override.or_else(|| {
        if is_elf_executable(target) {
            Some(target.to_path_buf())
        } else if let Some(package) = &package {
            let candidate = default_go_binary(workspace, package);
            if candidate.is_file() {
                Some(candidate)
            } else {
                None
            }
        } else {
            None
        }
    });

    let mut values = HashMap::new();
    values.insert("target".to_string(), target.display().to_string());
    values.insert("workspace".to_string(), workspace.display().to_string());
    if let Some(package) = &package {
        values.insert("package".to_string(), package.display().to_string());
    }
    if let Some(binary) = &binary {
        values.insert("binary".to_string(), binary.display().to_string());
    }

    LaunchResolveContext {
        target: target.to_path_buf(),
        workspace: workspace.to_path_buf(),
        package,
        binary,
        program: None,
        values,
    }
}

fn apply_profile_resolve(ctx: &mut LaunchResolveContext, resolve: &HashMap<String, String>) {
    const ORDER: &[&str] = &["workspace", "package", "binary", "program"];

    for key in ORDER {
        if let Some(template) = resolve.get(*key) {
            apply_resolve_entry(ctx, key, template);
        }
    }

    for (key, template) in resolve {
        if ORDER.contains(&key.as_str()) {
            continue;
        }
        apply_resolve_entry(ctx, key, template);
    }
}

fn apply_resolve_entry(ctx: &mut LaunchResolveContext, key: &str, template: &str) {
    let value = expand_template(template, ctx);
    ctx.values.insert(key.to_string(), value.clone());
    match key {
        "program" => ctx.program = Some(PathBuf::from(value)),
        "package" => ctx.package = Some(PathBuf::from(value)),
        "binary" => ctx.binary = Some(PathBuf::from(value)),
        "workspace" => ctx.workspace = PathBuf::from(value),
        _ => {}
    }
}

fn merge_initialize_payload(adapter_initialize: &Value) -> Value {
    let mut payload = json!({
        "clientID": "daptor",
        "clientName": "daptor",
        "pathFormat": "path",
        "linesStartAt1": true,
        "columnsStartAt1": true,
        "supportsVariableType": true,
        "supportsVariablePaging": false,
        "supportsSetVariable": true,
        "supportsRunInTerminalRequest": true,
        "supportsMemoryReferences": true,
        "supportsMemoryEvent": true,
    });

    if let Some(object) = adapter_initialize.as_object() {
        payload.as_object_mut().expect("initialize payload object").extend(object.clone());
    }

    payload
}

fn merge_launch_payload(
    adapter_launch: &Value,
    profile_launch: &Value,
    path_map: &[PathMapEntry],
    ctx: &LaunchResolveContext,
    program: &Path,
    args: &[String],
) -> Value {
    let mut launch = match adapter_launch {
        Value::Null => Map::new(),
        Value::Object(map) => map.clone(),
        other => {
            let mut map = Map::new();
            map.insert("type".to_string(), other.clone());
            map
        }
    };

    if let Value::Object(profile_map) = profile_launch {
        launch.extend(profile_map.clone());
    }

    launch.insert("program".to_string(), Value::String(program.display().to_string()));
    if !args.is_empty() {
        launch.insert("args".to_string(), json!(args));
    }
    if !launch.contains_key("request") {
        launch.insert("request".to_string(), Value::String("launch".to_string()));
    }
    if !launch.contains_key("cwd") {
        launch.insert("cwd".to_string(), Value::String(ctx.workspace.display().to_string()));
    }

    if !path_map.is_empty() {
        let mappings = path_map
            .iter()
            .map(|entry| {
                json!({
                    "from": expand_template(&entry.from, ctx),
                    "to": expand_template(&entry.to, ctx),
                })
            })
            .collect::<Vec<_>>();
        launch.insert("substitutePath".to_string(), Value::Array(mappings));
    }

    launch = expand_value_strings(launch, ctx);
    Value::Object(launch)
}

fn expand_value_strings(
    mut value: Map<String, Value>,
    ctx: &LaunchResolveContext,
) -> Map<String, Value> {
    for entry in value.iter_mut() {
        if let Value::String(text) = &entry.1 {
            *entry.1 = Value::String(expand_template(text, ctx));
        }
    }
    value
}

pub fn expand_template(template: &str, ctx: &LaunchResolveContext) -> String {
    let mut output = template.to_string();
    for _ in 0..8 {
        let Some((start, end)) = find_next_placeholder(&output) else {
            break;
        };
        let placeholder = output[start + 2..end - 1].to_string();
        let replacement = evaluate_placeholder(&placeholder, ctx);
        output.replace_range(start..end, &replacement);
    }
    output
}

fn find_next_placeholder(input: &str) -> Option<(usize, usize)> {
    let start = input.find("${")?;
    let mut index = start + 2;
    while index < input.len() {
        if input[index..].starts_with("${") {
            let nested = find_next_placeholder(&input[index..])?;
            index += nested.1;
            continue;
        }
        if input.as_bytes().get(index) == Some(&b'}') {
            return Some((start, index + 1));
        }
        index += 1;
    }
    None
}

fn evaluate_placeholder(placeholder: &str, ctx: &LaunchResolveContext) -> String {
    if let Some((func, arg)) = placeholder.split_once(':') {
        let expanded_arg = expand_template(arg, ctx);
        return match func {
            "dirname" => PathBuf::from(expanded_arg)
                .parent()
                .map(Path::display)
                .map(|value| value.to_string())
                .unwrap_or_else(|| ".".to_string()),
            "binary_probe" => {
                resolve_native_launch_path(Path::new(&expanded_arg)).display().to_string()
            }
            "go_package" => go_package_dir(Path::new(&expanded_arg)).display().to_string(),
            "name" => PathBuf::from(expanded_arg)
                .file_stem()
                .map(|value| value.to_string_lossy().to_string())
                .unwrap_or_default(),
            "nearest" => find_nearest_file(ctx.target.as_path(), arg)
                .map(|path| path.display().to_string())
                .unwrap_or_default(),
            _ => String::new(),
        };
    }

    ctx.values.get(placeholder).cloned().unwrap_or_default()
}

pub fn resolved_launch_to_json(resolved: &ResolvedLaunch) -> Result<String> {
    serde_json::to_string(resolved).context("failed to serialize resolved launch")
}

pub fn resolved_launch_from_json(json: &str) -> Result<ResolvedLaunch> {
    serde_json::from_str(json).context("failed to parse resolved launch JSON")
}
