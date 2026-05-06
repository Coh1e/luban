#!/usr/bin/env bash
# luban smoke test (POSIX). Mirrors scripts/smoke.bat for Linux/macOS.
#
# Same 9 steps: new -> add -> build -> run -> check -> clean ->
# target add/build -> remove/sync -> doctor.
#
# Pre-req: luban + toolchain already on PATH (caller has applied
# `main/cpp-toolchain` and run `luban env --user` and re-sourced their
# shell rc). The script does NOT bootstrap luban itself.
#
# Exit codes:
#   0        all checks passed
#   non-zero first failed step's number (stderr says which assertion fired)

set -uo pipefail

# ---- locate luban ----
# Prefer local build artifacts (so dev smoke tests what was just compiled).
# Fall back to luban on PATH for release / install-flow smoke.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ -x "${SCRIPT_DIR}/../build/release/luban" ]; then
    LUBAN="${SCRIPT_DIR}/../build/release/luban"
elif [ -x "${SCRIPT_DIR}/../build/default/luban" ]; then
    LUBAN="${SCRIPT_DIR}/../build/default/luban"
elif command -v luban >/dev/null 2>&1; then
    LUBAN="$(command -v luban)"
else
    echo "SMOKE: luban not found in build/{release,default}/ or on PATH" >&2
    exit 99
fi
echo "SMOKE: using ${LUBAN}"

# ---- isolate ----
# Random temp dir per run so smoke is re-runnable + parallel-safe in CI.
TMP_PROJ="$(mktemp -d -t luban-smoke-XXXXXX)" || {
    echo "SMOKE: could not mktemp" >&2
    exit 1
}
cd "${TMP_PROJ}" || {
    echo "SMOKE: could not cd into ${TMP_PROJ}" >&2
    exit 1
}

STEP=0
fail() {
    echo "SMOKE: failed at step ${STEP} (project preserved at ${TMP_PROJ})" >&2
    exit "${STEP}"
}

# ---- 1. new app ----
STEP=$((STEP + 1))
"${LUBAN}" new app smoketest --no-build || { echo "SMOKE step ${STEP} FAIL: new" >&2; fail; }
cd smoketest || { echo "SMOKE step ${STEP} FAIL: smoketest dir missing" >&2; fail; }
[ -f vcpkg.json ]                    || { echo "SMOKE step ${STEP} FAIL: vcpkg.json"                    >&2; fail; }
[ -f luban.cmake ]                   || { echo "SMOKE step ${STEP} FAIL: luban.cmake"                   >&2; fail; }
[ -f src/smoketest/main.cpp ]        || { echo "SMOKE step ${STEP} FAIL: src/smoketest/main.cpp"        >&2; fail; }

# ---- 2. add a vcpkg dep (offline-safe: only edits manifests, no fetch) ----
# `add fmt` then `remove fmt` purely exercises the manifest-edit code path.
# We don't `luban build` after `add` because that would fetch fmt from
# vcpkg-the-network, fragile in restricted CI. Build assertion below uses
# a no-deps project.
STEP=$((STEP + 1))
"${LUBAN}" add fmt    || { echo "SMOKE step ${STEP} FAIL: add fmt"    >&2; fail; }
grep -q '"fmt"'  vcpkg.json   || { echo "SMOKE step ${STEP} FAIL: fmt not in vcpkg.json"   >&2; fail; }
grep -q 'fmt::fmt' luban.cmake || { echo "SMOKE step ${STEP} FAIL: fmt::fmt not in luban.cmake" >&2; fail; }
"${LUBAN}" remove fmt || { echo "SMOKE step ${STEP} FAIL: remove fmt" >&2; fail; }
if grep -q 'fmt::fmt' luban.cmake; then
    echo "SMOKE step ${STEP} FAIL: fmt::fmt still in luban.cmake after remove" >&2
    fail
fi

# ---- 3. build (no deps; offline) ----
# With deps removed, build auto-picks the `no-vcpkg` preset; the exe lands
# under build/no-vcpkg/src/smoketest/. Recursive search for portability.
STEP=$((STEP + 1))
"${LUBAN}" build || { echo "SMOKE step ${STEP} FAIL: build" >&2; fail; }
EXE_FOUND="$(find build -name smoketest -type f -executable 2>/dev/null | head -n1 || true)"
if [ -z "${EXE_FOUND}" ]; then
    echo "SMOKE step ${STEP} FAIL: smoketest binary missing under build/" >&2
    fail
fi
[ -f compile_commands.json ] || { echo "SMOKE step ${STEP} FAIL: compile_commands.json" >&2; fail; }

# ---- 4. run + check stdout ----
STEP=$((STEP + 1))
"${EXE_FOUND}" >smoke-out.txt 2>&1 || true
if ! grep -q "hello from smoketest" smoke-out.txt; then
    echo "SMOKE step ${STEP} FAIL: stdout match" >&2
    cat smoke-out.txt >&2
    fail
fi

# ---- 5. check (cmake configure with -Wdev, no full rebuild) ----
# Reuses the existing build/ tree. -Wdev surfaces CMakeLists policy issues
# but doesn't fail the build; we only assert exit 0.
STEP=$((STEP + 1))
"${LUBAN}" check || { echo "SMOKE step ${STEP} FAIL: check" >&2; fail; }

# ---- 6. clean + rebuild (round-trip) ----
# clean must return 0 and remove build/. Subsequent build must reproduce
# the exe, proving the project is reproducible from manifest+source alone.
STEP=$((STEP + 1))
"${LUBAN}" clean || { echo "SMOKE step ${STEP} FAIL: clean" >&2; fail; }
[ ! -d build ] || { echo "SMOKE step ${STEP} FAIL: build dir not removed" >&2; fail; }
"${LUBAN}" build || { echo "SMOKE step ${STEP} FAIL: rebuild after clean" >&2; fail; }
EXE_FOUND="$(find build -name smoketest -type f -executable 2>/dev/null | head -n1 || true)"
if [ -z "${EXE_FOUND}" ]; then
    echo "SMOKE step ${STEP} FAIL: smoketest binary missing after rebuild" >&2
    fail
fi

# ---- 7. target add lib + rebuild ----
STEP=$((STEP + 1))
"${LUBAN}" target add lib mycore || { echo "SMOKE step ${STEP} FAIL: target add" >&2; fail; }
[ -f src/mycore/CMakeLists.txt ] || { echo "SMOKE step ${STEP} FAIL: mycore/CMakeLists.txt" >&2; fail; }
"${LUBAN}" build || { echo "SMOKE step ${STEP} FAIL: build after target add" >&2; fail; }

# ---- 8. target remove + sync ----
STEP=$((STEP + 1))
"${LUBAN}" target remove mycore || { echo "SMOKE step ${STEP} FAIL: target remove" >&2; fail; }
"${LUBAN}" sync || { echo "SMOKE step ${STEP} FAIL: sync" >&2; fail; }

# ---- 9. doctor (verify it runs and emits valid JSON; not --strict) ----
# Smoke skips `luban setup` (saves 250 MB of downloads on every CI run),
# so installed.json is empty → doctor --strict would exit 1, and
# doctor --json reports all_ok: false. That's not a luban bug, just the
# smoke runner's choice to test luban-the-binary rather than a fully-
# provisioned host.
STEP=$((STEP + 1))
"${LUBAN}" doctor >/dev/null || { echo "SMOKE step ${STEP} FAIL: doctor exit nonzero" >&2; fail; }
"${LUBAN}" doctor --json | grep -q '"schema":' || {
    echo "SMOKE step ${STEP} FAIL: doctor --json missing schema field" >&2
    fail
}

# ---- 10. blueprint pipeline (apply + unapply + rollback, no network) ----
# Mirrors smoke.bat step 10. Drops a one-file blueprint under a file://
# bp source, applies/unapplies/rolls-back, and asserts the deployed file
# really comes and goes on disk. Sandboxed via LUBAN_PREFIX + HOME.
STEP=$((STEP + 1))
BP_SANDBOX="$(mktemp -d -t luban-smoke-bp-XXXXXX)"
mkdir -p "${BP_SANDBOX}/bp-src/blueprints" || { echo "SMOKE step ${STEP} FAIL: mkdir bp sandbox" >&2; fail; }

cat >"${BP_SANDBOX}/bp-src/blueprints/smoke-files.toml" <<'EOF'
name = "smoke-files"
description = "smoke test blueprint - one file"

[files."~/luban-smoke-marker.txt"]
content = "smoke-content-v1"
mode = "replace"
EOF

_OLD_PREFIX="${LUBAN_PREFIX:-}"
_OLD_HOME="${HOME}"
export LUBAN_PREFIX="${BP_SANDBOX}"
export HOME="${BP_SANDBOX}"

bp_fail() {
    export LUBAN_PREFIX="${_OLD_PREFIX}"
    export HOME="${_OLD_HOME}"
    fail
}

"${LUBAN}" bp src add "${BP_SANDBOX}/bp-src" --name smoke --yes || { echo "SMOKE step ${STEP} FAIL: bp src add" >&2; bp_fail; }
"${LUBAN}" bp apply smoke/smoke-files || { echo "SMOKE step ${STEP} FAIL: bp apply 1" >&2; bp_fail; }
[ -f "${BP_SANDBOX}/luban-smoke-marker.txt" ] || { echo "SMOKE step ${STEP} FAIL: marker not deployed by apply" >&2; bp_fail; }

# Use the qualified `<source>/<bp>` form on purpose: unapply normalizes
# by stripping the source prefix. Earlier versions silently no-op'd
# this case (records key on bare name); the assertion right below
# guards that fix.
"${LUBAN}" bp unapply smoke/smoke-files || { echo "SMOKE step ${STEP} FAIL: bp unapply" >&2; bp_fail; }
[ ! -f "${BP_SANDBOX}/luban-smoke-marker.txt" ] || { echo "SMOKE step ${STEP} FAIL: marker still present after unapply" >&2; bp_fail; }

"${LUBAN}" bp apply smoke/smoke-files || { echo "SMOKE step ${STEP} FAIL: bp apply 2" >&2; bp_fail; }
[ -f "${BP_SANDBOX}/luban-smoke-marker.txt" ] || { echo "SMOKE step ${STEP} FAIL: marker not redeployed" >&2; bp_fail; }

"${LUBAN}" bp rollback || { echo "SMOKE step ${STEP} FAIL: bp rollback" >&2; bp_fail; }
[ ! -f "${BP_SANDBOX}/luban-smoke-marker.txt" ] || { echo "SMOKE step ${STEP} FAIL: marker still present after rollback" >&2; bp_fail; }

export LUBAN_PREFIX="${_OLD_PREFIX}"
export HOME="${_OLD_HOME}"
rm -rf "${BP_SANDBOX}"

# ---- cleanup ----
cd "${SCRIPT_DIR}"
rm -rf "${TMP_PROJ}"
echo "SMOKE OK (10/10 steps passed)"
exit 0
