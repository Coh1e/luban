#!/usr/bin/env bash
# luban smoke test (POSIX). Mirrors scripts/smoke.bat for Linux/macOS.
#
# Currently a placeholder: luban v0.2 doesn't ship Linux/macOS binaries
# yet (proc.cpp / download.cpp / win_path.cpp still rely on Win32 APIs;
# the POSIX port is M4+ work tracked in
# docs/specs/planning/open-questions.md OQ-4).
#
# When the port lands, mirror the .bat sequence:
#   1. luban new app smoketest --no-build
#   2. luban add fmt; verify vcpkg.json + luban.cmake updated
#   3. luban build; verify build/default/smoketest exists
#   4. ./build/default/smoketest matches "hello from smoketest"
#   5. luban specs init; verify AGENTS.md + specs/HOW-TO-USE.md
#   6. luban specs new onboarding; verify scene/pain/mvp .md scaffolded
#   7. luban target add lib mycore; rebuild
#   8. luban target remove mycore; luban remove fmt; luban sync;
#      verify fmt::fmt absent from luban.cmake
#   9. luban doctor --strict --json; verify all_ok=true

set -euo pipefail

echo "luban smoke (POSIX): not yet supported."
echo ""
echo "luban v0.2 ships Windows-only. Port progress at"
echo "  https://github.com/Coh1e/luban/issues"
echo "and design doc at"
echo "  docs/specs/planning/open-questions.md (OQ-4)."
exit 1
