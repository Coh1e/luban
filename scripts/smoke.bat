@echo off
:: luban smoke test (Windows). Exercises the daily-driver path:
:: new → add → build → run → specs init/new/status → target add/build → remove/sync.
:: Designed for CI: zero ambient state, isolated temp project, clear pass/fail.
::
:: Pre-req: luban + toolchain already installed (caller has run `luban setup`
:: + `luban env --user`). The script does NOT bootstrap luban itself.
::
:: Exit codes:
::   0        all checks passed
::   non-zero first failed step's number (stderr says which assertion fired)

setlocal enabledelayedexpansion

:: ---- locate luban ----
:: Prefer luban.exe on PATH (where releases ship). Fall back to local build
:: artifacts so devs can run smoke from a fresh clone after `cmake --build`.
where luban.exe >nul 2>&1
if %errorlevel% == 0 (
    set LUBAN=luban.exe
) else if exist "%~dp0..\build\release\luban.exe" (
    set LUBAN=%~dp0..\build\release\luban.exe
) else if exist "%~dp0..\build\default\luban.exe" (
    set LUBAN=%~dp0..\build\default\luban.exe
) else (
    echo SMOKE: luban.exe not found on PATH or in build/^{release,default^}/
    exit /b 99
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

:: ---- 5. specs init + scaffold check ----
set /a STEP+=1
"%LUBAN%" specs init || (echo SMOKE step %STEP% FAIL: specs init & goto :fail)
if not exist AGENTS.md          (echo SMOKE step %STEP% FAIL: AGENTS.md          & goto :fail)
if not exist CLAUDE.md          (echo SMOKE step %STEP% FAIL: CLAUDE.md          & goto :fail)
if not exist specs\HOW-TO-USE.md (echo SMOKE step %STEP% FAIL: HOW-TO-USE.md     & goto :fail)
findstr /c:"luban-managed: project-context" AGENTS.md >nul || (
    echo SMOKE step %STEP% FAIL: marker block missing in AGENTS.md & goto :fail
)

:: ---- 6. specs new <topic> + status ----
set /a STEP+=1
"%LUBAN%" specs new onboarding || (echo SMOKE step %STEP% FAIL: specs new & goto :fail)
if not exist specs\sage\onboarding\scene.md (echo SMOKE step %STEP% FAIL: scene.md & goto :fail)
if not exist specs\sage\onboarding\pain.md  (echo SMOKE step %STEP% FAIL: pain.md  & goto :fail)
if not exist specs\sage\onboarding\mvp.md   (echo SMOKE step %STEP% FAIL: mvp.md   & goto :fail)
"%LUBAN%" specs status >nul || (echo SMOKE step %STEP% FAIL: specs status & goto :fail)

:: ---- 7. target add lib + rebuild ----
set /a STEP+=1
"%LUBAN%" target add lib mycore || (echo SMOKE step %STEP% FAIL: target add & goto :fail)
if not exist src\mycore\CMakeLists.txt (echo SMOKE step %STEP% FAIL: mycore/CMakeLists.txt & goto :fail)
"%LUBAN%" build || (echo SMOKE step %STEP% FAIL: build after target add & goto :fail)

:: ---- 8. target remove + sync ----
set /a STEP+=1
"%LUBAN%" target remove mycore || (echo SMOKE step %STEP% FAIL: target remove & goto :fail)
"%LUBAN%" sync || (echo SMOKE step %STEP% FAIL: sync & goto :fail)

:: ---- 9. doctor --strict --json (re-runs project state, must exit 0) ----
set /a STEP+=1
"%LUBAN%" doctor --strict >nul || (echo SMOKE step %STEP% FAIL: doctor --strict & goto :fail)
"%LUBAN%" doctor --json | findstr /c:"\"all_ok\": true" >nul || (
    echo SMOKE step %STEP% FAIL: doctor --json: all_ok != true & goto :fail
)

:: ---- cleanup ----
popd
rmdir /s /q "%TMP_PROJ%" 2>nul
echo SMOKE OK (9/9 steps passed)
exit /b 0

:fail
popd 2>nul
:: Leave the temp dir intact for postmortem; CI can collect it as artifact.
echo SMOKE: failed at step %STEP% (project preserved at %TMP_PROJ%)
exit /b %STEP%
