#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

echo "==> rustfmt"
cargo fmt --all -- --check

echo "==> clippy"
cargo clippy --workspace --all-targets -- -D warnings

echo "==> rust unit tests"
cargo test -p daptor-core --lib
cargo test -p daptor-core --test launch_resolve
cargo test -p daptor-core --test highlight_smoke
cargo build -p daptor-cli

echo "==> C++ build"
cmake -S cpp -B cpp/build -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j"$(nproc)"

echo "==> cbindgen header"
test -f include/tui_debug.h

echo "All checks passed."
