#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
gcc -g -O0 -Wall -Wextra -o "${ROOT}/fixtures/interactive_demo" "${ROOT}/fixtures/interactive_demo.c"
echo "Built ${ROOT}/fixtures/interactive_demo"
