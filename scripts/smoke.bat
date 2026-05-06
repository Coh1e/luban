@echo off
:: luban smoke test (Windows). Exercises the daily-driver path:
:: new -> add -> build -> run -> check -> clean -> target add/build -> remove/sync -> doctor.
:: Designed for CI: zero ambient state, isolated temp project, clear pass/fail.
::
:: Pre-req: luban + toolchain already installed (caller has applied
:: `main/cpp-toolchain` and run `luban env --user`). The script does NOT
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

:: ---- 5. check (cmake configure with -Wdev, no full rebuild) ----
:: Reuses the existing build/ tree. -Wdev surfaces CMakeLists policy issues
:: but doesn't fail the build; we only assert exit 0.
set /a STEP+=1
"%LUBAN%" check || (echo SMOKE step %STEP% FAIL: check & goto :fail)

:: ---- 6. clean + rebuild (round-trip) ----
:: clean must return 0 and remove build/. Subsequent build must reproduce
:: the exe, proving the project is reproducible from manifest+source alone.
set /a STEP+=1
"%LUBAN%" clean || (echo SMOKE step %STEP% FAIL: clean & goto :fail)
if exist build (echo SMOKE step %STEP% FAIL: build dir not removed & goto :fail)
"%LUBAN%" build || (echo SMOKE step %STEP% FAIL: rebuild after clean & goto :fail)
set EXE_FOUND=
for /r build %%f in (smoketest.exe) do (
    if exist "%%f" set EXE_FOUND=%%f
)
if not defined EXE_FOUND (echo SMOKE step %STEP% FAIL: smoketest.exe missing after rebuild & goto :fail)

:: ---- 7. target add lib + rebuild ----
set /a STEP+=1
"%LUBAN%" target add lib mycore || (echo SMOKE step %STEP% FAIL: target add & goto :fail)
if not exist src\mycore\CMakeLists.txt (echo SMOKE step %STEP% FAIL: mycore/CMakeLists.txt & goto :fail)
"%LUBAN%" build || (echo SMOKE step %STEP% FAIL: build after target add & goto :fail)

:: ---- 8. target remove + sync ----
set /a STEP+=1
"%LUBAN%" target remove mycore || (echo SMOKE step %STEP% FAIL: target remove & goto :fail)
"%LUBAN%" sync || (echo SMOKE step %STEP% FAIL: sync & goto :fail)

:: ---- 9. doctor (verify it runs and emits valid JSON; not --strict) ----
:: Smoke skips `luban setup` (saves 250 MB of downloads on every CI run),
:: so installed.json is empty → doctor --strict would exit 1, and
:: doctor --json reports all_ok: false. That's not a luban bug, just the
:: smoke runner's choice to test luban-the-binary rather than a fully-
:: provisioned host. So the asserts here are: doctor exits 0, --json
:: produces a "schema" key (proving valid output, not a crash).
set /a STEP+=1
"%LUBAN%" doctor >nul || (echo SMOKE step %STEP% FAIL: doctor exit nonzero & goto :fail)
"%LUBAN%" doctor --json | findstr /c:"\"schema\":" >nul || (
    echo SMOKE step %STEP% FAIL: doctor --json missing schema field & goto :fail
)

:: ---- 10. blueprint pipeline (apply + unapply + rollback, no network) ----
:: Exercises the v1.0 verbs added in S6/S7: bp src add (file:// source),
:: bp apply, bp unapply (now restores disk), bp rollback (now reconciles
:: disk to target generation). Runs in its own LUBAN_PREFIX/USERPROFILE
:: sandbox so applied state never touches the user's real ~/.local/bin
:: or XDG dirs.
set /a STEP+=1
set BP_SANDBOX=%TEMP%\luban-smoke-bp-%RANDOM%-%RANDOM%
mkdir "%BP_SANDBOX%\bp-src\blueprints" || (echo SMOKE step %STEP% FAIL: mkdir bp sandbox & goto :fail)

:: One [files] entry — `~/luban-smoke-marker.txt`. Apply lays it down,
:: unapply / rollback should remove it.
> "%BP_SANDBOX%\bp-src\blueprints\smoke-files.toml" (
    echo name = "smoke-files"
    echo description = "smoke test blueprint - one file"
    echo.
    echo [files."~/luban-smoke-marker.txt"]
    echo content = "smoke-content-v1"
    echo mode = "replace"
)

set _OLD_PREFIX=%LUBAN_PREFIX%
set _OLD_USERPROFILE=%USERPROFILE%
set LUBAN_PREFIX=%BP_SANDBOX%
set USERPROFILE=%BP_SANDBOX%

"%LUBAN%" bp src add "%BP_SANDBOX%\bp-src" --name smoke --yes || (echo SMOKE step %STEP% FAIL: bp src add & goto :bp_fail)
"%LUBAN%" bp apply smoke/smoke-files || (echo SMOKE step %STEP% FAIL: bp apply 1 & goto :bp_fail)
if not exist "%BP_SANDBOX%\luban-smoke-marker.txt" (echo SMOKE step %STEP% FAIL: marker not deployed by apply & goto :bp_fail)

:: Use the qualified `<source>/<bp>` form on purpose: unapply normalizes
:: by stripping the source prefix. Earlier versions silently no-op'd
:: this case (records key on bare name); the assertion right below
:: guards that fix.
"%LUBAN%" bp unapply smoke/smoke-files || (echo SMOKE step %STEP% FAIL: bp unapply & goto :bp_fail)
if exist "%BP_SANDBOX%\luban-smoke-marker.txt" (echo SMOKE step %STEP% FAIL: marker still present after unapply & goto :bp_fail)

"%LUBAN%" bp apply smoke/smoke-files || (echo SMOKE step %STEP% FAIL: bp apply 2 & goto :bp_fail)
if not exist "%BP_SANDBOX%\luban-smoke-marker.txt" (echo SMOKE step %STEP% FAIL: marker not redeployed by apply 2 & goto :bp_fail)

"%LUBAN%" bp rollback || (echo SMOKE step %STEP% FAIL: bp rollback & goto :bp_fail)
if exist "%BP_SANDBOX%\luban-smoke-marker.txt" (echo SMOKE step %STEP% FAIL: marker still present after rollback & goto :bp_fail)

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
echo SMOKE OK (10/10 steps passed)
exit /b 0

:fail
popd 2>nul
:: Leave the temp dir intact for postmortem; CI can collect it as artifact.
echo SMOKE: failed at step %STEP% (project preserved at %TMP_PROJ%)
exit /b %STEP%
