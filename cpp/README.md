# tui-debug-ui (C++ / Tuinator)

Terminal UI binary that links **Tuinator** (C++) and **tui-debug-core** (Rust static library).

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
./cpp/build/tui-debug-ui fixtures/hello.py

# Frontend-only: mock session, no debugpy/Rust at runtime
./cpp/build/tui-debug-ui --mock fixtures/hello.py

# Or via the Rust CLI wrapper
cargo run -p tui-debug-cli -- run --mock --program fixtures/hello.py
```

### Layout / resize

- **Mouse:** drag the cyan dividers between main areas; click pane names in the header to switch views, or `◄` / `►` when not all names fit.
- **Keyboard:** `Alt` + arrow keys resize sidebar width and bottom tray height.
- **Panel swap:** with sidebar or bottom tray focused, press `[` / `]` (or `<` / `>`) to cycle views. When all names fit, they are shown inline (active name highlighted). Otherwise arrows and `(n/total)` are reserved first; as many names as fit are shown between `◄` and `►`. Only when fewer than two names fit does the header collapse to `◄ Name ►`.

## Layout

| Path | Role |
|------|------|
| `cmake/Tuinator.cmake` | `FetchContent` for Tuinator; links `tuinator::tuinator` |
| `cmake/Corrosion.cmake` | `FetchContent` for [Corrosion](https://github.com/corrosion-rs/corrosion); imports crate `tui-debug-core` as CMake target `tui_debug_core` |
| `../include/tui_debug.h` | C API header (from `cbindgen` in the Rust crate) |
| `../crates/tui-debug-core` | DAP engine / staticlib source |

## Clean rebuild

```bash
rm -rf cpp/build
cmake -S cpp -B cpp/build
cmake --build cpp/build
```
