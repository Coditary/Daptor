# Daptor

Terminal debugger built on the [Debug Adapter Protocol](https://microsoft.github.io/debug-adapter-protocol/). Inspired by [nvim-dap-ui](https://github.com/rcarriga/nvim-dap-ui).

**Daptor** pairs a Rust DAP engine with a C++/[Tuinator](https://github.com/Coditary/Tuinator) terminal UI — fast stepping, scopes, breakpoints, REPL, and a layout you can shape to your workflow.

> **Early development (0.1).**

## Supported targets

| Language / runtime | Adapter | How to run |
|--------------------|---------|------------|
| Python | [debugpy](https://github.com/microsoft/debugpy) | `./cpp/build/daptor path/to/script.py` |
| C / C++ (native ELF) | [lldb-dap](https://github.com/llvm/llvm-project) | Auto-selected for binaries, or `--lldb` |
| Reverse debugging (Linux) | [rr](https://rr-project.org/) | `--profile rr path/to/binary` |

## Prerequisites

**Python debugging**

```bash
python3 -m pip install debugpy
```

**Build toolchain**

- Rust (`cargo`, `rustc`)
- CMake 3.20+, C++20, `pkg-config`, **ncursesw** dev headers

**Optional — syntax highlighting**

Tree-sitter grammars under `~/.local/share/dap/tree-sitter/<language>/` (or set `DAP_TREE_SITTER_DIR` / `paths.tree_sitter_dir` in config).

## Quick start

```bash
# Launch definitions (adapters + profiles)
mkdir -p ~/.config/daptor
cp config/config.yaml ~/.config/daptor/config.yaml
cp config/definitions.yaml ~/.config/daptor/definitions.yaml

# Engine + CLI
cargo build -p daptor-cli

# UI (first run fetches Tuinator + Corrosion)
cmake -S cpp -B cpp/build
cmake --build cpp/build

# Debug a Python script
./cpp/build/daptor path/to/script.py

# Or via the Rust launcher
cargo run -p daptor-cli -- run path/to/script.py

# Frontend-only mock (no debugpy)
./cpp/build/daptor --mock path/to/script.py
```

**Headless DAP smoke test** (Rust only, no UI):

```bash
cargo run -p daptor-cli -- test path/to/script.py
```

## Configuration

User config: `~/.config/daptor/` (or `$XDG_CONFIG_HOME/daptor/`).

| File | Purpose |
|------|---------|
| `config.yaml` | Theme, layout, keybindings, paths |
| `definitions.yaml` | Launch adapters and profiles |
| `theme.json` | UI + syntax colors |

Starter templates live in [`config/`](config/):

```bash
mkdir -p ~/.config/daptor
cp config/config.yaml ~/.config/daptor/config.yaml
cp config/definitions.yaml ~/.config/daptor/definitions.yaml
cp config/theme.json ~/.config/daptor/theme.json
```

**`config.yaml` highlights**

```yaml
theme:
  file: theme.json

paths:
  tree_sitter_dir: ~/.local/share/dap/tree-sitter

layout:
  sidebar_pct: 25
  bottom_pct: 35
  docks:
    left:
      - panel: variables
      - panel: variables
        scope: Locals
      - panel: threads
      - panel: breakpoints
      - panel: watches
    center:
      - panel: source
    bottom:
      - panel: repl
      - panel: console

keybindings:
  continue: c
  step_over: n
  step_into: i
  step_out: u
  breakpoint: b
```

**`theme.json` shortcuts** — `background`, `text`, `muted`, `accent`, `divider`, `selected`, plus per-role `colors` under `ui` and `syntax`.

**Environment overrides**

| Variable | Effect |
|----------|--------|
| `DAPTOR_CONFIG` | Path to `config.yaml` |
| `DAPTOR_THEME` | Path to `theme.json` |
| `DAP_TREE_SITTER_DIR` | Tree-sitter grammar directory |

Schemas: [`schemas/config.schema.yaml`](schemas/config.schema.yaml), [`schemas/theme.schema.json`](schemas/theme.schema.json).

## Keyboard & layout

Default keys (all remappable in `config.yaml`):

| Key | Action |
|-----|--------|
| `c` | Continue |
| `n` / `i` / `u` | Step over / into / out |
| `b` / `Space` | Toggle breakpoint (source focus) |
| `r` | Focus REPL |
| `[` / `]` | Cycle sidebar or bottom panels |
| `Tab` | Cycle focus between areas |
| `F5`–`F12` | Continue / step (function keys) |

- **Mouse:** drag dividers; click panel tabs to switch.
- **Keyboard:** `Alt` + arrows resize sidebar and bottom tray.
- **Layout:** drag panels between regions; configure default docks in `config.yaml`.

See [cpp/README.md](cpp/README.md) for UI build and resize details.

## Features

| Area | Status |
|------|--------|
| Split layout (sidebar / source / bottom) | ✅ |
| Source view + tree-sitter highlighting | ✅ |
| Scopes, stacks, watches, breakpoints | ✅ |
| REPL + console | ✅ |
| Conditional & data breakpoints | ✅ |
| Memory, disassembly, file tree | ✅ |
| Network panel + compose templates | ✅ |
| Resizable splits + drag-and-drop layout | ✅ |
| Custom theme, layout, keybindings | ✅ |
| Layout persist on exit | 🔜 |
| Attach to running process | 🔜 |
| Installable release / packages | 🔜 |

## Project structure

Daptor is one product with two build trees:

```
crates/                 # Rust — engine + CLI launcher
  daptor-core/          # DAP client, session, tree-sitter, C API
  daptor-cli/           # `cargo run` wrapper → starts the UI binary

cpp/                    # C++ — terminal UI (Tuinator)
  include/tui_debug_ui/ # Panels, theme, layout
  src/ui/               # Application shell → builds `daptor`

config/                 # Starter templates for user config
schemas/                # JSON/YAML schemas for user config
reference/nvim-dap-ui/  # Upstream UX reference (git submodule)
```

## Architecture

```
┌─────────────────────────────────────┐
│  daptor  (C++ / Tuinator)           │  panels, input, layout, theme
├─────────────────────────────────────┤
│  tui_debug.h C API                  │
├─────────────────────────────────────┤
│  daptor-core  (Rust)                │  DAP client, session, highlight
└─────────────────────────────────────┘
         │ debugpy / lldb-dap / rr
         ▼
    debuggee process
```

## C API (Rust → C++)

| Function | Purpose |
|----------|---------|
| `tui_debug_session_launch` | Launch debugpy session |
| `tui_debug_session_launch_lldb` | Launch lldb-dap session |
| `tui_debug_session_launch_rr` | Launch rr replay session |
| `tui_debug_poll` | Non-blocking DAP events |
| `tui_debug_sync_snapshot` | Full debugger state snapshot |
| `tui_debug_command` | `continue`, `step_over`, … |
| `tui_debug_evaluate` | REPL expression evaluation |
| `tui_debug_set_breakpoints` | Source breakpoints |
| `tui_debug_fetch_variables` | Lazy-load scope variables |
| `tui_debug_highlight_viewport` | Tree-sitter spans as JSON |
| `tui_debug_set_tree_sitter_dir` | Override grammar search path |

## Development

### CI checks (local)

```bash
./scripts/ci/check.sh      # fmt, clippy, tests, C++ build
./scripts/ci/format.sh     # auto-format Rust (+ C++ if clang-format is installed)
```

GitHub Actions runs the same checks on push/PR to `main`. Integration tests (debugpy, lldb-dap, rr) run in a separate job with `--ignored`.

### Release

Tag a version to publish a Linux tarball:

```bash
git tag v0.1.0
git push origin v0.1.0
```

The release workflow builds `daptor` (UI) and `daptor-cli`, bundles config templates, and attaches `daptor-<version>-linux-x86_64.tar.gz` to the GitHub release.

## License

MIT
