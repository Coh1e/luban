#!/usr/bin/env bash
# luban one-shot installer for Linux/macOS.
#
# Usage:   curl -fsSL https://luban.coh1e.com/install.sh | sh
#
# Placeholder. Linux/macOS support is M4+ work — the binary doesn't yet
# build for non-Windows targets (proc.cpp / download.cpp / win_path.cpp
# still rely on Win32 APIs). When that lands this script will mirror
# install.ps1: download luban + luban-shim into ~/.local/bin, verify
# SHA256, offer to run `luban setup`.

set -euo pipefail

echo "luban does not yet ship Linux/macOS binaries."
echo "Track progress at https://github.com/Coh1e/luban/issues — port"
echo "from Windows to POSIX is open M4 work."
exit 1
