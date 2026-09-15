#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${ROOT}/cpp/build"
UI_BIN="${BUILD_DIR}/tui-debug-ui"
export TMPDIR="${ROOT}/.tmp"
mkdir -p "${TMPDIR}" "${BUILD_DIR}"

if [[ "${1:-}" == "run" ]]; then
    shift
    cmake --build "${BUILD_DIR}"
    if [[ ! -x "${UI_BIN}" ]]; then
        echo "error: ${UI_BIN} not found after build" >&2
        exit 1
    fi
    if [[ ! -t 0 ]]; then
        echo "tui-debug-ui requires an interactive terminal (stdin must be a TTY)." >&2
        echo "Run this directly in a terminal, not via a pipe or background job." >&2
        exit 1
    fi
    exec "${UI_BIN}" "$@"
fi

cmake --build "${BUILD_DIR}" "$@"
echo ""
echo "Built ${UI_BIN}"
echo "Start the debugger in a terminal:"
echo "  ${BASH_SOURCE[0]} run examples/python/hello.py"
echo "  ${BASH_SOURCE[0]} run --mock examples/python/hello.py"
