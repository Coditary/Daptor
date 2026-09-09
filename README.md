# tui-debug

Standalone DAP debugger inspired by [nvim-dap-ui](https://github.com/rcarriga/nvim-dap-ui).

**Architecture:** Rust engine (`tui-debug-core`) + C++/[Tuinator](https://github.com/Coditary/Tuinator) UI.

## Prerequisites

```bash
python3 -m pip install debugpy
```

For the Tuinator UI (C++):

- CMake 3.20+, C++20, `pkg-config`, **ncursesw** dev headers
- Rust toolchain (`cargo`)

Optional: Tree-sitter grammars for syntax highlighting:

```text
~/.local/share/dap/tree-sitter/python/
  libtree-sitter-python.so
  queries/highlights.scm
```

Or set `DAP_TREE_SITTER_DIR`.

## Usage

```bash
# Build Rust CLI + engine
cargo build -p tui-debug-cli

# Build Tuinator UI (first time fetches Tuinator + Corrosion)
cmake -S cpp -B cpp/build
cmake --build cpp/build

# Launch debugger (delegates to cpp/build/tui-debug-ui)
cargo run -p tui-debug-cli -- run --program fixtures/hello.py

# Or run the UI binary directly
./cpp/build/tui-debug-ui fixtures/hello.py

# Headless DAP smoke test (Rust only)
cargo run -p tui-debug-cli -- test
```

## Project structure

```
crates/
  tui-debug-core/     # DAP, session, tree-sitter highlight, C API (staticlib)
  tui-debug-cli/      # CLI entry (run → UI binary, test → headless)
cpp/
  cmake/              # Tuinator + Corrosion
  include/tui_debug_ui/  # C++ view model, SourcePanel, SyntaxTheme
  src/ui/             # Tuinator Application shell
include/
  tui_debug.h         # Generated C API header (cbindgen)
src/                  # Legacy ratatui UI (deprecated, will be removed)
reference/nvim-dap-ui/
fixtures/hello.py
```

## C API (Rust → C++)

| Function | Purpose |
|----------|---------|
| `tui_debug_session_launch` | Connect to debugpy, launch program |
| `tui_debug_poll` | Non-blocking DAP events (JSON) |
| `tui_debug_sync_snapshot` | Full debugger state snapshot |
| `tui_debug_drain_console` | Program stdout/stderr |
| `tui_debug_command` | `continue`, `step_over`, … |
| `tui_debug_evaluate` | REPL expression evaluation |
| `tui_debug_set_breakpoints` | Set breakpoints for a source file |
| `tui_debug_fetch_variables` | Lazy-load scope variables |
| `tui_debug_highlight_viewport` | Tree-sitter spans as JSON |

## UI status

| Feature | Status |
|---------|--------|
| Tuinator split layout (sidebar / source / bottom) | ✅ |
| Source panel + tree-sitter highlight bridge | ✅ |
| DAP session via C API (async launch) | ✅ |
| Scopes (with locals) / stacks / console panels | ✅ |
| Keyboard: c/n/i/u step, Tab focus, b breakpoints | ✅ |
| REPL evaluate (`tui_debug_evaluate`) | ✅ |
| Breakpoints via DAP (`tui_debug_set_breakpoints`) | ✅ |
| Resizable split panes (Tuinator native drag) | 🔜 |
| Watches / breakpoint list panel | 🔜 |
| Legacy ratatui UI | deprecated |

See [cpp/README.md](cpp/README.md) for C++ build details.
