#!/usr/bin/env bash
set -euo pipefail

# Adapters and tools for ignored integration tests.
sudo apt-get update
sudo apt-get install -y g++ python3 python3-debugpy lldb gdb

# rr is optional; tests skip when it is missing.
sudo apt-get install -y rr || true

bash crates/daptor-core/tests/fixtures/native/build.sh

python3 -c "import debugpy"
