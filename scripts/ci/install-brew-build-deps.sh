#!/usr/bin/env bash
set -euo pipefail

export HOMEBREW_NO_AUTO_UPDATE=1
export HOMEBREW_NO_INSTALL_UPGRADE=1

brew install cmake pkg-config ncurses zstd
NCURSES_PREFIX="$(brew --prefix ncurses)"
echo "PKG_CONFIG_PATH=${NCURSES_PREFIX}/lib/pkgconfig" >> "${GITHUB_ENV}"
echo "CMAKE_PREFIX_PATH=${NCURSES_PREFIX}" >> "${GITHUB_ENV}"
