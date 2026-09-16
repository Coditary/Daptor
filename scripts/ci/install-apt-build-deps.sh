#!/usr/bin/env bash
set -euo pipefail

# Shared apt packages for CI/release C++ UI builds (Ubuntu 24.04+).
sudo apt-get update
sudo apt-get install -y cmake pkg-config libncurses-dev g++
