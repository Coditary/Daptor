#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DIR="${ROOT}/examples/native"

gcc -g -O0 -Wall -Wextra -o "${DIR}/reverse_demo" "${DIR}/reverse_demo.c"
gcc -g -O0 -Wall -Wextra -o "${DIR}/interactive_demo" "${DIR}/interactive_demo.c"
g++ -g -O0 -std=c++17 -Wall -Wextra -o "${DIR}/exception_demo" "${DIR}/exception_demo.cpp"
gcc -g -O0 -Wall -Wextra -fno-omit-frame-pointer \
  -o "${DIR}/dap_panels_demo" "${DIR}/dap_panels_demo.c"

echo "Built native examples in ${DIR}/"
