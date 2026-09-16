# Daptor UI (C++ / Tuinator)

Terminal UI for **Daptor**. Links [Tuinator](https://github.com/Coditary/Tuinator) (C++) with `daptor-core` (Rust static library).

The build artifact is `cpp/build/daptor`.

## Prerequisites

- CMake 3.20+
- C++20 compiler
- [Rust toolchain](https://rustup.rs/) (`cargo`, `rustc`)
- **ncursesw** development files (Fedora: `ncurses-devel`, Debian/Ubuntu: `libncurses-dev`)
- `pkg-config`

Optional: clone [Tuinator](https://github.com/Coditary/Tuinator) next to this repo as `Coditary-Bundle/Tuinator` to avoid fetching it on every configure.

## Configure and build

From the repository root:

```bash
cmake -S cpp -B cpp/build
cmake --build cpp/build
```

The executable is `cpp/build/daptor`.

## Run

```bash
# Full stack (Rust DAP + Tuinator UI)
./cpp/build/daptor path/to/script.py

# Native binary (lldb-dap, auto-detected)
./cpp/build/daptor path/to/binary

# Reverse debugging (Linux + rr)
./cpp/build/daptor --profile rr path/to/binary

# Frontend-only mock session
./cpp/build/daptor --mock path/to/script.py

# Via the Rust CLI wrapper
cargo run -p daptor-cli -- run path/to/script.py
```

### Theme / config

Config directory: `~/.config/daptor/` (or `$XDG_CONFIG_HOME/daptor/`).

See the root [README.md](../README.md) for full `config.yaml` and `theme.json` documentation. Starter templates are in [`config/`](../config/).

Overrides: `DAPTOR_CONFIG`, `DAPTOR_THEME`.

### Layout / resize

- **Mouse:** drag the cyan dividers between main areas; click pane names in the header to switch views, or `◄` / `►` when not all names fit.
- **Keyboard:** `Alt` + arrow keys resize sidebar width and bottom tray height.
- **Panel swap:** with sidebar or bottom tray focused, press `[` / `]` (or `<` / `>`) to cycle views.

## Build layout

| Path | Role |
|------|------|
| `cmake/Tuinator.cmake` | `FetchContent` for Tuinator; links `tuinator::tuinator` |
| `cmake/Corrosion.cmake` | Imports crate `daptor-core` as CMake target `daptor_core` |
| `../include/tui_debug.h` | C API header (cbindgen) |
| `../crates/daptor-core` | DAP engine / staticlib source |

## Clean rebuild

```bash
rm -rf cpp/build
cmake -S cpp -B cpp/build
cmake --build cpp/build
```
