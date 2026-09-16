#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
bash "${ROOT}/scripts/ci/install-apt-build-deps.sh"
sudo apt-get install -y patchelf zstd
