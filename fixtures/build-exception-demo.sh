#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
g++ -g -O0 -std=c++17 -o "${ROOT}/fixtures/exception_demo" "${ROOT}/fixtures/exception_demo.cpp"
echo "Built ${ROOT}/fixtures/exception_demo"
