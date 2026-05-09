# Activate the project-local sandbox toolchain in the CURRENT shell.
#
# Usage:
#     . .\scripts\activate-toolchain.ps1     # dot-source so $env:PATH sticks
#                                            # in the parent shell (running it
#                                            # with `&` mutates a child shell
#                                            # only and is a no-op).
#
# What it does:
#   - Prepends .toolchain\{git\cmd, cmake\bin, ninja, llvm-mingw\bin} to the
#     CURRENT process $env:PATH. In-process only — does NOT touch HKCU PATH,
#     HKLM, or any registry key.
#   - Sets a transient $env:LUBAN_TOOLCHAIN_ACTIVE marker so re-sourcing is
#     idempotent (no duplicate PATH entries) and so prompts / status lines
#     can reflect the activation.
#
# Why a project-local sandbox at all: when you test a half-broken install.ps1
# / new bp / `luban self uninstall` regression, the global luban store at
# ~/.local/share/luban/ can get nuked or corrupted. The .toolchain/ tree is
# untouched by anything luban does — your dev env survives every "rm -rf".
# .toolchain/ is .gitignore'd so it never ships.
#
# To bootstrap from scratch (first time, or after a deliberate wipe):
#     scripts\bootstrap-toolchain.ps1     # downloads git + cmake + ninja +
#                                         # llvm-mingw into .toolchain/

#Requires -Version 5
$ErrorActionPreference = 'Stop'

# Resolve .toolchain/ relative to this script's location, regardless of cwd.
$scriptDir   = Split-Path -Parent $MyInvocation.MyCommand.Path
$projectRoot = Split-Path -Parent $scriptDir
$toolchain   = Join-Path $projectRoot '.toolchain'

if (-not (Test-Path $toolchain)) {
    Write-Host "✗ $toolchain does not exist." -ForegroundColor Red
    Write-Host "  Run:  scripts\bootstrap-toolchain.ps1" -ForegroundColor Red
    return
}

# The 4 sandbox bin directories. Order: git first so any tool that shells
# out to git (cmake's FetchContent, vcpkg) finds the sandboxed git before
# any system one. cmake/ninja/llvm-mingw order doesn't matter functionally.
$binDirs = @(
    (Join-Path $toolchain 'git\cmd'),
    (Join-Path $toolchain 'cmake\bin'),
    (Join-Path $toolchain 'ninja'),
    (Join-Path $toolchain 'llvm-mingw\bin')
)

# Idempotence: if already active, strip prior entries before re-prepending.
# This way calling `. activate-toolchain.ps1` twice doesn't bloat PATH.
if ($env:LUBAN_TOOLCHAIN_ACTIVE -eq $toolchain) {
    $existing = ($env:PATH -split ';') | Where-Object { $_ -and ($binDirs -notcontains $_) }
    $env:PATH = ($existing -join ';')
}

# Verify each dir exists + has the expected exe; warn if not (don't hard-
# fail because a partial sandbox is still better than no sandbox).
$missing = @()
$expected = @{
    'git\cmd'        = 'git.exe'
    'cmake\bin'      = 'cmake.exe'
    'ninja'          = 'ninja.exe'
    'llvm-mingw\bin' = 'clang.exe'
}
foreach ($k in $expected.Keys) {
    $exe = Join-Path (Join-Path $toolchain $k) $expected[$k]
    if (-not (Test-Path $exe)) { $missing += $exe }
}
if ($missing.Count -gt 0) {
    Write-Host "⚠ partial sandbox; missing:" -ForegroundColor Yellow
    $missing | ForEach-Object { Write-Host "    $_" -ForegroundColor Yellow }
    Write-Host "  Re-run: scripts\bootstrap-toolchain.ps1" -ForegroundColor Yellow
}

# Prepend (not append) so sandbox tools win over any same-named global
# install. Single in-process assignment — does NOT call SetEnvironmentVariable
# so HKCU / HKLM stay clean.
$env:PATH = ($binDirs -join ';') + ';' + $env:PATH
$env:LUBAN_TOOLCHAIN_ACTIVE = $toolchain

# Confirm versions so the user can eyeball that the sandbox is live.
Write-Host "✓ luban .toolchain sandbox active" -ForegroundColor Green
Write-Host "  $toolchain"
foreach ($k in $expected.Keys) {
    $exe = Join-Path (Join-Path $toolchain $k) $expected[$k]
    if (-not (Test-Path $exe)) { continue }
    $ver = & $exe --version 2>$null | Select-Object -First 1
    $name = (Split-Path -Leaf $exe).Replace('.exe','')
    $pad = $name.PadRight(9)
    Write-Host "  $pad  $ver"
}
Write-Host ""
Write-Host "  PATH only mutated in this shell (no HKCU writes)."
Write-Host "  Open a new shell or `Remove-Item Env:LUBAN_TOOLCHAIN_ACTIVE` to forget."
