#!/usr/bin/env bash
# luban one-shot installer for Linux/macOS.
#
# Usage:   curl -fsSL https://github.com/Coh1e/luban/raw/main/install.sh | sh
#
# Placeholder. Linux/macOS support is M4+ work — the binary now builds
# for POSIX (CI gating green) but a few install-flow pieces remain (PATH
# integration via shellrc, default blueprint apply UX). When that lands
# this script will mirror install.ps1: download luban + luban-shim into
# ~/.local/bin, verify SHA256, offer to run `luban bp apply embedded:cpp-base`.

set -euo pipefail

echo "luban does not yet ship Linux/macOS binaries."
echo "Track progress at https://github.com/Coh1e/luban/issues — port"
echo "from Windows to POSIX is open M4 work."
exit 1
