#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

gcc -g -O0 -Wall -Wextra -o "${DIR}/reverse_demo" "${DIR}/reverse_demo.c"
gcc -g -O0 -Wall -Wextra -o "${DIR}/interactive_demo" "${DIR}/interactive_demo.c"
g++ -g -O0 -std=c++17 -Wall -Wextra -o "${DIR}/cpp_step_demo" "${DIR}/cpp_step_demo.cpp"
