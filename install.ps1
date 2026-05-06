# luban one-shot installer for Windows.
#
# Usage:    irm https://github.com/Coh1e/luban/raw/main/install.ps1 | iex
#
# Drops luban.exe + luban-shim.exe into ~/.local/bin (XDG-style, shared with
# uv/pipx/claude-code), verifies SHA256, and offers to run `luban env --user`
# (HKCU PATH registration) and bootstrap the foundation toolchain via
# `luban bp src add Coh1e/luban-bps` + `luban bp apply main/cpp-base`.
#
# Idempotent: re-running on an already-installed machine detects existing
# binaries (matching SHA = no-op; stale = update), checks PATH membership
# with native-separator normalization (no duplicate entries), and only
# bootstraps the toolchain when it is not already registered.
#
# Override the install dir with $env:LUBAN_INSTALL_DIR or pre-create the
# target dir; the installer never elevates and never writes outside it.
# Set $env:LUBAN_FORCE_REINSTALL=1 to force redownload even when SHAs match.

#Requires -Version 5
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

# ---- target dir -------------------------------------------------------------
# Always materialize the path with native separators (Join-Path on Windows
# emits '\') so downstream PATH comparisons line up.
$installDir = if ($env:LUBAN_INSTALL_DIR) { $env:LUBAN_INSTALL_DIR }
              else { Join-Path $env:USERPROFILE '.local\bin' }
$installDir = [System.IO.Path]::GetFullPath($installDir)

if (Test-Path $installDir) {
    Write-Host "→ Install dir: $installDir (exists)"
} else {
    Write-Host "→ Install dir: $installDir (creating)"
    New-Item -ItemType Directory -Path $installDir -Force | Out-Null
}

$forceReinstall = [bool]$env:LUBAN_FORCE_REINSTALL

# ---- discover latest release -----------------------------------------------
Write-Host "→ Querying github.com/Coh1e/luban for the latest release..."
$release = Invoke-RestMethod `
    -Uri 'https://api.github.com/repos/Coh1e/luban/releases/latest' `
    -Headers @{ 'User-Agent' = 'luban-installer' }
$tag = $release.tag_name
Write-Host "  found $tag"

function Get-AssetUrl($name) {
    $a = $release.assets | Where-Object { $_.name -eq $name }
    if (-not $a) { throw "release $tag has no asset '$name'" }
    return $a.browser_download_url
}

# ---- SHA256SUMS -------------------------------------------------------------
$sumsTmp = Join-Path $env:TEMP "luban-SHA256SUMS-$([Guid]::NewGuid()).txt"
Invoke-WebRequest -Uri (Get-AssetUrl 'SHA256SUMS') -OutFile $sumsTmp -UseBasicParsing
$sums = @{}
foreach ($line in Get-Content $sumsTmp) {
    $parts = $line -split '\s\s', 2
    if ($parts.Length -eq 2) { $sums[$parts[1].Trim()] = $parts[0].Trim() }
}
Remove-Item $sumsTmp -Force

function Test-Already-Installed($name) {
    $target = Join-Path $installDir $name
    if (-not (Test-Path $target)) { return $false }
    $expected = $sums[$name]
    if (-not $expected) { return $false }
    $actual = (Get-FileHash -Path $target -Algorithm SHA256).Hash.ToLower()
    return ($actual -eq $expected)
}

function Install-Asset($name) {
    if ((-not $forceReinstall) -and (Test-Already-Installed $name)) {
        Write-Host "  $name already installed (SHA256 matches latest) — skipping"
        return
    }
    $expected = $sums[$name]
    if (-not $expected) { throw "SHA256SUMS does not list $name" }
    $tmp = Join-Path $env:TEMP "luban-$name-$([Guid]::NewGuid()).part"
    Write-Host "→ Downloading $name..."
    Invoke-WebRequest -Uri (Get-AssetUrl $name) -OutFile $tmp -UseBasicParsing
    $actual = (Get-FileHash -Path $tmp -Algorithm SHA256).Hash.ToLower()
    if ($actual -ne $expected) {
        Remove-Item $tmp -Force
        throw "SHA256 mismatch for $name`n  expected $expected`n  got      $actual"
    }
    $target = Join-Path $installDir $name
    Move-Item -Path $tmp -Destination $target -Force
    Write-Host "  installed → $target"
}

Install-Asset 'luban.exe'
Install-Asset 'luban-shim.exe'

# ---- HKCU PATH integration (best-effort, prompted) -------------------------
# Read HKCU PATH directly (not $env:Path which is process-merged user+system),
# normalize each entry to backslash + lowercase, and check membership against
# the install dir. No duplicates if already present in any slash form.
function Test-OnUserPath($dir) {
    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $userPath) { return $false }
    $needle = $dir.TrimEnd('\','/').Replace('/', '\').ToLowerInvariant()
    foreach ($p in ($userPath -split ';')) {
        if (-not $p) { continue }
        $hay = $p.TrimEnd('\','/').Replace('/', '\').ToLowerInvariant()
        if ($hay -eq $needle) { return $true }
    }
    return $false
}

if (Test-OnUserPath $installDir) {
    Write-Host ""
    Write-Host "$installDir is already on your HKCU PATH — no change needed."
} else {
    Write-Host ""
    Write-Host "$installDir is not on your HKCU PATH yet."
    $ans = Read-Host "Add it (and register VCPKG_ROOT / EM_CONFIG when applicable)? [Y/n]"
    if ($ans -eq '' -or $ans -match '^[Yy]') {
        & (Join-Path $installDir 'luban.exe') env --user
    } else {
        Write-Host "Run \`luban env --user\` later to register on HKCU PATH."
    }
}

# ---- toolchain bootstrap (prompted, idempotent) ----------------------------
# luban.exe embeds zero blueprints (议题 AG); the foundation set lives in
# Coh1e/luban-bps. Skip prompting if `main` is already registered AND
# cpp-base is in the applied generation — the user is just refreshing.
$lubanExe = Join-Path $installDir 'luban.exe'

function Test-BpSourceRegistered($name) {
    try {
        $out = & $lubanExe bp src ls 2>$null
        if ($LASTEXITCODE -ne 0) { return $false }
        return ($out | Select-String -SimpleMatch -Pattern $name -Quiet)
    } catch { return $false }
}

function Test-BpApplied($bp) {
    try {
        $out = & $lubanExe bp ls 2>$null
        if ($LASTEXITCODE -ne 0) { return $false }
        return ($out | Select-String -SimpleMatch -Pattern $bp -Quiet)
    } catch { return $false }
}

$mainRegistered = Test-BpSourceRegistered 'main'
$cppApplied     = Test-BpApplied 'cpp-base'

Write-Host ""
if ($mainRegistered -and $cppApplied) {
    Write-Host "main/cpp-base already applied — bootstrap skipped."
} else {
    if ($mainRegistered) {
        $ans = Read-Host "main is registered. Apply main/cpp-base now? (toolchain: llvm-mingw / cmake / ninja / git / vcpkg) [Y/n]"
    } else {
        $ans = Read-Host "Register Coh1e/luban-bps and apply main/cpp-base now? (toolchain: llvm-mingw / cmake / ninja / git / vcpkg) [Y/n]"
    }
    if ($ans -eq '' -or $ans -match '^[Yy]') {
        if (-not $mainRegistered) {
            & $lubanExe bp src add Coh1e/luban-bps --name main --yes
        }
        & $lubanExe bp apply main/cpp-base
    } else {
        Write-Host "Later, run:"
        if (-not $mainRegistered) {
            Write-Host "  luban bp src add Coh1e/luban-bps --name main --yes"
        }
        Write-Host "  luban bp apply main/cpp-base"
    }
}

Write-Host ""
Write-Host "✓ luban $tag at $installDir"
Write-Host "  open a new shell for PATH changes to take effect."
