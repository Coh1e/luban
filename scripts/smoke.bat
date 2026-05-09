@echo off
:: luban smoke test (Windows). Exercises the daily-driver path on the
:: v1.0 verb surface (DESIGN §5):
::   new -> add/remove -> build -> run -> doctor -> describe ->
::   bp src add + bp apply.
::
:: Pre-req: luban + toolchain already installed (caller has applied
:: `main/bootstrap` and run `luban env --user`). The script does NOT
:: bootstrap luban itself.
::
:: Exit codes:
::   0        all checks passed
::   non-zero first failed step's number (stderr says which assertion fired)

setlocal enabledelayedexpansion

:: ---- locate luban ----
:: Prefer local build artifacts (so dev smoke tests what was just compiled).
:: Fall back to luban.exe on PATH for release / install-flow smoke.
if exist "%~dp0..\build\release\luban.exe" (
    set LUBAN=%~dp0..\build\release\luban.exe
) else if exist "%~dp0..\build\default\luban.exe" (
    set LUBAN=%~dp0..\build\default\luban.exe
) else (
    where luban.exe >nul 2>&1
    if !errorlevel! == 0 (
        set LUBAN=luban.exe
    ) else (
        echo SMOKE: luban.exe not found in build/^{release,default^}/ or on PATH
        exit /b 99
    )
)
echo SMOKE: using %LUBAN%

:: ---- isolate ----
:: Random temp dir per run so smoke is re-runnable + parallel-safe in CI.
set TMP_PROJ=%TEMP%\luban-smoke-%RANDOM%-%RANDOM%
mkdir "%TMP_PROJ%" || (echo SMOKE: could not mkdir & exit /b 1)
pushd "%TMP_PROJ%" || (echo SMOKE: could not cd & exit /b 1)

set STEP=0

:: ---- 1. new app ----
set /a STEP+=1
"%LUBAN%" new app smoketest --no-build || (echo SMOKE step %STEP% FAIL: new & goto :fail)
cd smoketest || (echo SMOKE: smoketest dir missing & goto :fail)
if not exist vcpkg.json   (echo SMOKE step %STEP% FAIL: vcpkg.json   & goto :fail)
if not exist luban.cmake  (echo SMOKE step %STEP% FAIL: luban.cmake  & goto :fail)
if not exist src\smoketest\main.cpp (echo SMOKE step %STEP% FAIL: src\smoketest\main.cpp & goto :fail)

:: ---- 2. add a vcpkg dep (offline-safe: only edits manifests, no fetch) ----
:: We `add fmt` and `remove fmt` purely to exercise the manifest-edit code
:: path. We don't `luban build` after `add` because that would fetch fmt
:: from vcpkg-the-network, which is fragile in restricted environments
:: (we hit this writing the smoke: vcpkg pulled PowerShell as a build helper
:: and curl 56'd in CI). The build assertion below uses a no-deps project.
set /a STEP+=1
"%LUBAN%" add fmt || (echo SMOKE step %STEP% FAIL: add fmt & goto :fail)
findstr /c:"fmt" vcpkg.json >nul || (echo SMOKE step %STEP% FAIL: fmt not in vcpkg.json & goto :fail)
findstr /c:"fmt::fmt" luban.cmake >nul || (echo SMOKE step %STEP% FAIL: fmt::fmt not in luban.cmake & goto :fail)
"%LUBAN%" remove fmt || (echo SMOKE step %STEP% FAIL: remove fmt & goto :fail)
findstr /c:"fmt::fmt" luban.cmake >nul && (
    echo SMOKE step %STEP% FAIL: fmt::fmt still in luban.cmake after remove & goto :fail
)

:: ---- 3. build (no deps; offline) ----
:: With deps removed, build auto-picks the `no-vcpkg` preset; the exe lands
:: under build/no-vcpkg/src/smoketest/. Recursive search for portability so
:: this script doesn't break if preset selection logic changes.
set /a STEP+=1
"%LUBAN%" build || (echo SMOKE step %STEP% FAIL: build & goto :fail)
set EXE_FOUND=
for /r build %%f in (smoketest.exe) do (
    if exist "%%f" set EXE_FOUND=%%f
)
if not defined EXE_FOUND (echo SMOKE step %STEP% FAIL: smoketest.exe missing & goto :fail)
if not exist compile_commands.json (echo SMOKE step %STEP% FAIL: compile_commands.json & goto :fail)

:: ---- 4. run + check stdout ----
set /a STEP+=1
"%EXE_FOUND%" > smoke-out.txt 2>&1
findstr /c:"hello from smoketest" smoke-out.txt >nul || (
    echo SMOKE step %STEP% FAIL: stdout match
    type smoke-out.txt
    goto :fail
)

:: ---- 5. doctor (text + --json) ----
:: Smoke runs without applying any bp, so applied.txt may be empty —
:: doctor --strict would exit 1, but plain doctor exits 0 unconditionally.
:: --json must contain a "schema" key, proving valid output (not a crash).
set /a STEP+=1
"%LUBAN%" doctor >nul || (echo SMOKE step %STEP% FAIL: doctor exit nonzero & goto :fail)
"%LUBAN%" doctor --json | findstr /c:"\"schema\":" >nul || (
    echo SMOKE step %STEP% FAIL: doctor --json missing schema field & goto :fail
)

:: ---- 6. describe (text + --json + introspection prefixes) ----
:: DESIGN §5 + §10 require describe --json to expose a stable schema for
:: IDE plugins / agents. Confirm the schema marker, then exercise the
:: introspection prefixes (DESIGN §5: `port:` and `tool:`).
set /a STEP+=1
"%LUBAN%" describe >nul || (echo SMOKE step %STEP% FAIL: describe exit nonzero & goto :fail)
"%LUBAN%" describe --json | findstr /c:"\"schema\":" >nul || (
    echo SMOKE step %STEP% FAIL: describe --json missing schema field & goto :fail
)
"%LUBAN%" describe --json | findstr /c:"\"luban_version\":" >nul || (
    echo SMOKE step %STEP% FAIL: describe --json missing luban_version field & goto :fail
)
"%LUBAN%" describe port:fmt | findstr /c:"fmt::fmt" >nul || (
    echo SMOKE step %STEP% FAIL: describe port:fmt did not surface fmt::fmt & goto :fail
)

:: ---- 7. blueprint pipeline (bp src add + bp apply, no network) ----
:: Exercises the v1.0 verb table for blueprints: bp src add (file:// source),
:: bp apply (DESIGN §5). DESIGN §11 explicitly drops bp unapply / bp rollback,
:: so this no longer round-trips removal — applying the bp and verifying the
:: file landed is the assertion.
::
:: Runs in its own LUBAN_PREFIX/USERPROFILE sandbox so applied state never
:: touches the user's real ~/.local/bin or XDG dirs.
set /a STEP+=1
set BP_SANDBOX=%TEMP%\luban-smoke-bp-%RANDOM%-%RANDOM%
mkdir "%BP_SANDBOX%\bp-src\blueprints" || (echo SMOKE step %STEP% FAIL: mkdir bp sandbox & goto :fail)

:: One [files] entry — `~/luban-smoke-marker.txt`. Apply lays it down;
:: we assert it then exists. `mode = "replace"` overwrites any preexisting
:: file at the target path (sandbox is fresh, so no collision).
::
:: DESIGN §2 #6: bp source must be `.lua` (TOML accepted only as static
:: projection, not as parser input). The `[files]` entry's key needs the
:: bracketed string form because it contains a `~` and `/`.
> "%BP_SANDBOX%\bp-src\blueprints\smoke-files.lua" (
    echo return {
    echo   schema = 1,
    echo   name = "smoke-files",
    echo   description = "smoke test blueprint - one file",
    echo   files = {
    echo     ["~/luban-smoke-marker.txt"] = {
    echo       content = "smoke-content-v1",
    echo       mode = "replace",
    echo     },
    echo   },
    echo }
)

set _OLD_PREFIX=%LUBAN_PREFIX%
set _OLD_USERPROFILE=%USERPROFILE%
set LUBAN_PREFIX=%BP_SANDBOX%
set USERPROFILE=%BP_SANDBOX%

"%LUBAN%" bp src add "%BP_SANDBOX%\bp-src" --name smoke --yes || (echo SMOKE step %STEP% FAIL: bp src add & goto :bp_fail)
"%LUBAN%" bp apply smoke/smoke-files --yes || (echo SMOKE step %STEP% FAIL: bp apply & goto :bp_fail)
if not exist "%BP_SANDBOX%\luban-smoke-marker.txt" (echo SMOKE step %STEP% FAIL: marker not deployed by apply & goto :bp_fail)

:: dry-run on a re-apply must NOT touch state — applied.txt entries should
:: stay as-is and the marker file content shouldn't be re-written. We don't
:: try to assert mtime stability (filesystem-resolution-dependent), but a
:: clean exit + persisted marker is sufficient evidence dry-run is wired.
"%LUBAN%" bp apply smoke/smoke-files --dry-run --yes || (echo SMOKE step %STEP% FAIL: bp apply --dry-run & goto :bp_fail)
if not exist "%BP_SANDBOX%\luban-smoke-marker.txt" (echo SMOKE step %STEP% FAIL: marker vanished after --dry-run & goto :bp_fail)

set LUBAN_PREFIX=%_OLD_PREFIX%
set USERPROFILE=%_OLD_USERPROFILE%
rmdir /s /q "%BP_SANDBOX%" 2>nul
goto :smoke_ok

:bp_fail
set LUBAN_PREFIX=%_OLD_PREFIX%
set USERPROFILE=%_OLD_USERPROFILE%
goto :fail

:smoke_ok
:: ---- cleanup ----
popd
rmdir /s /q "%TMP_PROJ%" 2>nul
echo SMOKE OK (7/7 steps passed)
exit /b 0

:fail
popd 2>nul
:: Leave the temp dir intact for postmortem; CI can collect it as artifact.
echo SMOKE: failed at step %STEP% (project preserved at %TMP_PROJ%)
exit /b %STEP%
