#!/usr/bin/env bash
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
gcc -g -O0 -fno-omit-frame-pointer \
  -o "$root/fixtures/dap_panels_demo" \
  "$root/fixtures/dap_panels_demo.c"
echo "Built: $root/fixtures/dap_panels_demo"
