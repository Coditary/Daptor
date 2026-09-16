#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "${ROOT}"

echo "==> cargo fmt"
cargo fmt --all

if command -v clang-format >/dev/null 2>&1; then
    echo "==> clang-format (cpp/)"
    find cpp/include cpp/src \( -name '*.cpp' -o -name '*.hpp' \) -print0 \
        | xargs -0 clang-format -i
else
    echo "clang-format not found; skipping C++ formatting" >&2
fi
