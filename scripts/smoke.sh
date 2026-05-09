#!/usr/bin/env bash
# luban smoke test (POSIX). Mirrors scripts/smoke.bat for Linux/macOS.
#
# Same 7 steps as the Windows variant, on the v1.0 verb surface (DESIGN §5):
#   new -> add/remove -> build -> run -> doctor -> describe ->
#   bp src add + bp apply.
#
# Pre-req: luban + toolchain already on PATH (caller has applied
# `main/bootstrap` and run `luban env --user` and re-sourced their
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

# ---- 5. doctor (text + --json) ----
# Smoke runs without applying any bp, so applied.txt may be empty —
# doctor --strict would exit 1, but plain doctor exits 0 unconditionally.
# --json must contain a "schema" key, proving valid output (not a crash).
STEP=$((STEP + 1))
"${LUBAN}" doctor >/dev/null || { echo "SMOKE step ${STEP} FAIL: doctor exit nonzero" >&2; fail; }
"${LUBAN}" doctor --json | grep -q '"schema":' || {
    echo "SMOKE step ${STEP} FAIL: doctor --json missing schema field" >&2
    fail
}

# ---- 6. describe (text + --json + introspection prefixes) ----
# DESIGN §5 + §10 require describe --json to expose a stable schema for
# IDE plugins / agents. Confirm the schema marker, then exercise the
# introspection prefixes (DESIGN §5: `port:` and `tool:`).
STEP=$((STEP + 1))
"${LUBAN}" describe >/dev/null || { echo "SMOKE step ${STEP} FAIL: describe exit nonzero" >&2; fail; }
"${LUBAN}" describe --json | grep -q '"schema":' || {
    echo "SMOKE step ${STEP} FAIL: describe --json missing schema field" >&2
    fail
}
"${LUBAN}" describe --json | grep -q '"luban_version":' || {
    echo "SMOKE step ${STEP} FAIL: describe --json missing luban_version field" >&2
    fail
}
"${LUBAN}" describe port:fmt | grep -q 'fmt::fmt' || {
    echo "SMOKE step ${STEP} FAIL: describe port:fmt did not surface fmt::fmt" >&2
    fail
}

# ---- 7. blueprint pipeline (bp src add + bp apply, no network) ----
# Exercises the v1.0 verb table for blueprints: bp src add (file:// source),
# bp apply (DESIGN §5). DESIGN §11 explicitly drops bp unapply / bp rollback,
# so this no longer round-trips removal — applying the bp and verifying the
# file landed is the assertion. Sandboxed via LUBAN_PREFIX + HOME.
#
# DESIGN §2 #6: bp source must be `.lua` (TOML accepted only as static
# projection, not as parser input).
STEP=$((STEP + 1))
BP_SANDBOX="$(mktemp -d -t luban-smoke-bp-XXXXXX)"
mkdir -p "${BP_SANDBOX}/bp-src/blueprints" || { echo "SMOKE step ${STEP} FAIL: mkdir bp sandbox" >&2; fail; }

cat >"${BP_SANDBOX}/bp-src/blueprints/smoke-files.lua" <<'EOF'
return {
  schema = 1,
  name = "smoke-files",
  description = "smoke test blueprint - one file",
  files = {
    ["~/luban-smoke-marker.txt"] = {
      content = "smoke-content-v1",
      mode = "replace",
    },
  },
}
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
"${LUBAN}" bp apply smoke/smoke-files --yes || { echo "SMOKE step ${STEP} FAIL: bp apply" >&2; bp_fail; }
[ -f "${BP_SANDBOX}/luban-smoke-marker.txt" ] || { echo "SMOKE step ${STEP} FAIL: marker not deployed by apply" >&2; bp_fail; }

# dry-run on a re-apply must NOT touch state — applied.txt entries should
# stay as-is and the marker file shouldn't get re-written. We don't try to
# assert mtime stability (filesystem-resolution-dependent), but a clean
# exit + persisted marker is sufficient evidence dry-run is wired.
"${LUBAN}" bp apply smoke/smoke-files --dry-run --yes || { echo "SMOKE step ${STEP} FAIL: bp apply --dry-run" >&2; bp_fail; }
[ -f "${BP_SANDBOX}/luban-smoke-marker.txt" ] || { echo "SMOKE step ${STEP} FAIL: marker vanished after --dry-run" >&2; bp_fail; }

export LUBAN_PREFIX="${_OLD_PREFIX}"
export HOME="${_OLD_HOME}"
rm -rf "${BP_SANDBOX}"

# ---- cleanup ----
cd "${SCRIPT_DIR}"
rm -rf "${TMP_PROJ}"
echo "SMOKE OK (7/7 steps passed)"
exit 0
