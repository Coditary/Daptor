# Daptor UI (C++ / Tuinator)

Terminal UI for **Daptor**. Links [Tuinator](https://github.com/Coditary/Tuinator) (C++) with `tui-debug-core` (Rust static library).

The build artifact is currently named `tui-debug-ui`; it will be renamed to `daptor` in a future release.

## Prerequisites

- CMake 3.20+
- C++20 compiler
- [Rust toolchain](https://rustup.rs/) (`cargo`, `rustc`)
- **ncursesw** development files (Fedora: `ncurses-devel`, Debian/Ubuntu: `libncursesw-dev`)
- `pkg-config`

Optional: clone [Tuinator](https://github.com/Coditary/Tuinator) next to this repo as `Coditary-Bundle/Tuinator` to avoid fetching it on every configure.

## Configure and build

From the repository root:

```bash
cmake -S cpp -B cpp/build
cmake --build cpp/build
```

The executable is `cpp/build/tui-debug-ui`.

## Run

```bash
# Full stack (Rust DAP + Tuinator UI)
./cpp/build/tui-debug-ui examples/python/hello.py

# Native binary (lldb-dap, auto-detected)
./examples/native/build.sh
./cpp/build/tui-debug-ui examples/native/reverse_demo

# Reverse debugging (Linux + rr)
./cpp/build/tui-debug-ui --rr examples/native/reverse_demo

# Frontend-only mock session
./cpp/build/tui-debug-ui --mock examples/python/hello.py

# Via the Rust CLI wrapper
cargo run -p tui-debug-cli -- run --program examples/python/hello.py
```

### Theme / config

Config directory: `~/.config/tui-debug/` (or `$XDG_CONFIG_HOME/tui-debug/`).

See the root [README.md](../README.md) for full `config.yaml` and `theme.json` documentation.

Overrides: `TUI_DEBUG_CONFIG`, `TUI_DEBUG_THEME`.

### Layout / resize

- **Mouse:** drag the cyan dividers between main areas; click pane names in the header to switch views, or `◄` / `►` when not all names fit.
- **Keyboard:** `Alt` + arrow keys resize sidebar width and bottom tray height.
- **Panel swap:** with sidebar or bottom tray focused, press `[` / `]` (or `<` / `>`) to cycle views.

## Build layout

| Path | Role |
|------|------|
| `cmake/Tuinator.cmake` | `FetchContent` for Tuinator; links `tuinator::tuinator` |
| `cmake/Corrosion.cmake` | Imports crate `tui-debug-core` as CMake target `tui_debug_core` |
| `../include/tui_debug.h` | C API header (cbindgen) |
| `../crates/tui-debug-core` | DAP engine / staticlib source |

## Clean rebuild

```bash
rm -rf cpp/build
cmake -S cpp -B cpp/build
cmake --build cpp/build
```
